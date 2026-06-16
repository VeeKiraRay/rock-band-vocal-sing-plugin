#include "PluginEditor.h"
#include "EditorVisuals.h"

// The four draw routines (drawHighway/drawStats/drawStatusBar and drawLyricStrip)
// live in PluginEditorHighway.cpp and PluginEditorLyrics.cpp. This file holds the
// editor's lifecycle, the VBlank frame loop, layout, and the paint dispatcher.

// ─── Tooltip LookAndFeel ─────────────────────────────────────────────────────
// Bumps tooltip font to 15pt. JUCE 7 has no getTooltipFont() virtual; must
// replicate both drawTooltip() and getTooltipBounds() so size and draw stay in sync.
// Applied to the editor's TooltipWindow so it covers main panel + settings popup.

struct TooltipLAF : public juce::LookAndFeel_V4
{
    static constexpr float kTipFont = 15.0f;

    static juce::AttributedString makeTipString (const juce::String& tip, juce::Colour c)
    {
        juce::AttributedString s;
        s.setJustification (juce::Justification::centred);
        s.append (tip, juce::Font (kTipFont), c);
        return s;
    }

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText,
                                            juce::Point<int>    screenPos,
                                            juce::Rectangle<int> parentArea) override
    {
        juce::TextLayout tl;
        tl.createLayoutWithBalancedLineLengths (makeTipString (tipText, juce::Colours::black), 400.0f);
        const int w = (int) (tl.getWidth()  + 14.0f);
        const int h = (int) (tl.getHeight() +  6.0f);
        return juce::Rectangle<int> (
                   screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12)
                                                          : screenPos.x + 24,
                   juce::jmax (parentArea.getY(),
                               juce::jmin (screenPos.y - 6, parentArea.getBottom() - h)),
                   w, h)
               .constrainedWithin (parentArea);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h) override
    {
        const auto b = juce::Rectangle<float> ((float) w, (float) h);
        g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
        g.fillRoundedRectangle (b, 5.0f);
        g.setColour (findColour (juce::TooltipWindow::outlineColourId));
        g.drawRoundedRectangle (b.reduced (0.5f), 5.0f, 1.0f);
        juce::TextLayout tl;
        tl.createLayoutWithBalancedLineLengths (
            makeTipString (text, findColour (juce::TooltipWindow::textColourId)), 400.0f);
        tl.draw (g, b);
    }
};

// ─── Settings Popup ───────────────────────────────────────────────────────────
// Launched as a CalloutBox when the user clicks "⚙ Settings". Owns all the
// "configure-once" controls that were formerly in the always-visible panel.

class SettingsPanel : public juce::Component
{
public:
    struct Callbacks
    {
        double                      initFrameInterval  { 1.0 / 60.0 };
        std::function<void(double)> setFrameInterval;
        bool                        initReaperPriority { false };
        std::function<void(bool)>   setReaperPriority;
        int                        initRenderer        { 1 };   // combo item ID: 1=CPU, 2=session-only, 3=persist
        std::function<void(int)>   setRenderer;
        int                         initCalib          { 0 };
        std::function<void(int)>    setCalib;
        float                       initSpeed          { 200.0f };
        std::function<void(float)>  setSpeed;
        bool                        initWrap           { true };
        std::function<void(bool)>   setWrap;
        bool                        showLoadMidi       { false };
        std::function<void()>       loadMidi;
    };

    explicit SettingsPanel (RBVocalSingAudioProcessor& proc, Callbacks cbs)
        : proc (proc), cbs (std::move (cbs))
    {
        // ── Section headers ───────────────────────────────────────────────────
        setupHeader (perfHeader,    "PERFORMANCE");
        setupHeader (highwayHeader, "HIGHWAY");
        setupHeader (singingHeader, "SINGING");

        // ── Performance ───────────────────────────────────────────────────────
        static const juce::String kTipRate     = "Limit redraws per second. Lower values reduce CPU load on REAPER's UI thread.";
        static const juce::String kTipPriority = "Skips frames when paint takes too long, keeping REAPER's UI responsive. Recommended at large window sizes.";
        static const juce::String kTipRenderer = "CPU: software renderer (default). OpenGL: GPU rendering, may help at large window sizes. 'session only' reverts on next load; 'persist' saves the choice.";
        static const juce::String kTipCalib    = "Shift note timing to compensate for audio output latency. Positive = notes appear later.";
        static const juce::String kTipSpeed    = "How fast notes scroll across the highway. Higher = notes spread further apart.";
        static const juce::String kTipWrap     = "Wraps your detected pitch by octave to match the chart range when you sing an octave too high or low.";
        static const juce::String kTipGate     = "Minimum mic volume before pitch detection activates. Raise if background noise triggers false readings.";

        setupLabel (rateLabel, "Frame cap", kTipRate);
        rateCombo.addItem ("20 fps",   1);
        rateCombo.addItem ("30 fps",   2);
        rateCombo.addItem ("50 fps",   3);
        rateCombo.addItem ("60 fps",   4);
        rateCombo.addItem ("Uncapped", 5);
        {
            const double ivs[] = { 1.0/20, 1.0/30, 1.0/50, 1.0/60, 0.0 };
            int selId = 4;  // default 60 fps
            for (int i = 0; i < 5; ++i)
                if (std::abs (this->cbs.initFrameInterval - ivs[i]) < 1e-9)
                    { selId = i + 1; break; }
            rateCombo.setSelectedId (selId, juce::dontSendNotification);
        }
        styleCombo (rateCombo);
        rateCombo.setTooltip (kTipRate);
        rateCombo.onChange = [this]
        {
            const double ivs[] = { 1.0/20, 1.0/30, 1.0/50, 1.0/60, 0.0 };
            const int idx = rateCombo.getSelectedId() - 1;
            if (idx >= 0 && idx < 5) this->cbs.setFrameInterval (ivs[idx]);
        };
        addAndMakeVisible (rateCombo);

        setupLabel (reaperPriorityLabel, "REAPER priority", kTipPriority);
        setupToggle (reaperPriorityToggle, this->cbs.initReaperPriority, kTipPriority);
        reaperPriorityToggle.onClick = [this]
        {
            this->cbs.setReaperPriority (reaperPriorityToggle.getToggleState());
        };

        setupLabel (rendererLabel, "Renderer", kTipRenderer);
        rendererCombo.addItem ("CPU (software)",       1);
        rendererCombo.addItem ("OpenGL - session only", 2);
        rendererCombo.addItem ("OpenGL - persist",      3);
        rendererCombo.setSelectedId (this->cbs.initRenderer, juce::dontSendNotification);
        styleCombo (rendererCombo);
        rendererCombo.setTooltip (kTipRenderer);
        rendererCombo.onChange = [this]
        {
            this->cbs.setRenderer (rendererCombo.getSelectedId());
        };
        addAndMakeVisible (rendererCombo);

        // ── Highway ───────────────────────────────────────────────────────────
        setupLabel (calibLabel, "Calibration", kTipCalib);
        calibSlider.setSliderStyle (juce::Slider::IncDecButtons);
        calibSlider.setRange (-200, 500, 20);
        calibSlider.setValue (this->cbs.initCalib, juce::dontSendNotification);
        calibSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 65, 28);
        calibSlider.setTextValueSuffix (" ms");
        calibSlider.setTooltip (kTipCalib);
        calibSlider.onValueChange = [this]
        {
            this->cbs.setCalib ((int) calibSlider.getValue());
        };
        addAndMakeVisible (calibSlider);

        setupLabel (speedLabel, "Note Speed", kTipSpeed);
        noteSpeedSlider.setSliderStyle (juce::Slider::IncDecButtons);
        noteSpeedSlider.setRange (100, 400, 25);
        noteSpeedSlider.setValue (this->cbs.initSpeed, juce::dontSendNotification);
        noteSpeedSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 65, 28);
        noteSpeedSlider.setTextValueSuffix (" px/s");
        noteSpeedSlider.setTooltip (kTipSpeed);
        noteSpeedSlider.onValueChange = [this]
        {
            this->cbs.setSpeed ((float) noteSpeedSlider.getValue());
        };
        addAndMakeVisible (noteSpeedSlider);

        // ── Singing ───────────────────────────────────────────────────────────
        setupLabel (wrapLabel, "Octave Wrapping", kTipWrap);
        setupToggle (wrapToggle, this->cbs.initWrap, kTipWrap);
        wrapToggle.onClick = [this]
        {
            this->cbs.setWrap (wrapToggle.getToggleState());
        };

        setupLabel (gateLabel, "Mic Gate", kTipGate);
        gateSlider.setRange (0.001, 0.05, 0.0);
        gateSlider.setValue (proc.gateThreshold.load (std::memory_order_relaxed),
                             juce::dontSendNotification);
        gateSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        gateSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 55, 28);
        gateSlider.setNumDecimalPlacesToDisplay (3);
        gateSlider.setTooltip (kTipGate);
        gateSlider.onValueChange = [this]
        {
            this->proc.gateThreshold.store ((float) gateSlider.getValue(),
                                            std::memory_order_relaxed);
        };
        addAndMakeVisible (gateSlider);

        // ── Non-Reaper (only when REAPER API is unavailable) ──────────────────
        if (this->cbs.showLoadMidi)
        {
            setupHeader (nonReaperHeader, "NON-REAPER");
            loadMidiBtn.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff2a2a2a));
            loadMidiBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::lightgrey);
            loadMidiBtn.setTooltip ("Load a MIDI file containing PART VOCALS / HARM tracks.");
            loadMidiBtn.onClick = [this] { this->cbs.loadMidi(); };
            addAndMakeVisible (loadMidiBtn);
        }

        static constexpr int kExtraH = 18 + 6 + 28 + 14;  // hdrH + hdrGap + rowH + secGap
        setSize (kW, kH + (this->cbs.showLoadMidi ? kExtraH : 0));
    }

    void resized() override
    {
        static constexpr int kPad    = 14;
        static constexpr int kRowH   = 28;
        static constexpr int kRowGap = 6;
        static constexpr int kSecGap = 14;
        static constexpr int kHdrH   = 18;
        static constexpr int kHdrGap = 6;
        static constexpr int kLblGap = 8;

        auto area     = getLocalBounds().reduced (kPad);
        const int w   = area.getWidth();
        const int lblW = w * 42 / 100;          // 42% for right-aligned label
        const int ctrlW = w - lblW - kLblGap;   // rest for control

        // Lay out one labelled control row, optionally overriding the control width.
        auto layoutRow = [&] (juce::Label& lbl, juce::Component& ctrl, int ctrlWOvr = -1)
        {
            auto row = area.removeFromTop (kRowH);
            lbl.setBounds (row.removeFromLeft (lblW));
            row.removeFromLeft (kLblGap);
            ctrl.setBounds (row.removeFromLeft (ctrlWOvr > 0 ? ctrlWOvr : ctrlW));
            area.removeFromTop (kRowGap);
        };
        auto layoutHdr = [&] (juce::Label& hdr)
        {
            hdr.setBounds (area.removeFromTop (kHdrH));
            area.removeFromTop (kHdrGap);
        };

        layoutHdr (perfHeader);
        layoutRow (rateLabel,           rateCombo,             130);
        layoutRow (reaperPriorityLabel, reaperPriorityToggle,  24);
        layoutRow (rendererLabel,       rendererCombo,         160);

        area.removeFromTop (kSecGap);
        layoutHdr (highwayHeader);
        layoutRow (calibLabel, calibSlider);
        layoutRow (speedLabel, noteSpeedSlider);

        area.removeFromTop (kSecGap);
        layoutHdr (singingHeader);
        layoutRow (wrapLabel, wrapToggle, 24);
        layoutRow (gateLabel, gateSlider);

        if (cbs.showLoadMidi)
        {
            area.removeFromTop (kSecGap);
            layoutHdr (nonReaperHeader);
            auto row = area.removeFromTop (kRowH);
            loadMidiBtn.setBounds (row);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1e1e1e));
    }

    ~SettingsPanel() override = default;

    static constexpr int kW = 360;
    static constexpr int kH = 366;

private:
    void setupHeader (juce::Label& lbl, const juce::String& text)
    {
        lbl.setText (text, juce::dontSendNotification);
        lbl.setFont (juce::Font (11.0f, juce::Font::bold));
        lbl.setColour (juce::Label::textColourId, juce::Colour (0xffcc8844));
        lbl.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (lbl);
    }

    void setupLabel (juce::Label& lbl, const juce::String& text,
                     const juce::String& tooltip = {})
    {
        lbl.setText (text, juce::dontSendNotification);
        lbl.setFont (juce::Font (15.0f));
        lbl.setColour (juce::Label::textColourId, juce::Colour (0xffaaaaaa));
        lbl.setJustificationType (juce::Justification::centredRight);
        if (tooltip.isNotEmpty()) lbl.setTooltip (tooltip);
        addAndMakeVisible (lbl);
    }

    void setupToggle (juce::ToggleButton& btn, bool state, const juce::String& tip)
    {
        btn.setButtonText ("");
        btn.setToggleState (state, juce::dontSendNotification);
        btn.setTooltip (tip);
        addAndMakeVisible (btn);
    }

    static void styleCombo (juce::ComboBox& cb)
    {
        cb.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff2a2a2a));
        cb.setColour (juce::ComboBox::textColourId,       juce::Colours::white);
        cb.setColour (juce::ComboBox::arrowColourId,      juce::Colours::lightgrey);
    }

    RBVocalSingAudioProcessor& proc;
    Callbacks                  cbs;

    juce::Label         perfHeader, highwayHeader, singingHeader;

    juce::Label         rateLabel;
    juce::ComboBox      rateCombo;
    juce::Label         reaperPriorityLabel;
    juce::ToggleButton  reaperPriorityToggle;
    juce::Label         rendererLabel;
    juce::ComboBox      rendererCombo;

    juce::Label         calibLabel;
    juce::Slider        calibSlider;
    juce::Label         speedLabel;
    juce::Slider        noteSpeedSlider;

    juce::Label         wrapLabel;
    juce::ToggleButton  wrapToggle;
    juce::Label         gateLabel;
    juce::Slider        gateSlider;

    juce::Label         nonReaperHeader;
    juce::TextButton    loadMidiBtn    { "Load MIDI File" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsPanel)
};

// ─── Playrate step helpers ────────────────────────────────────────────────────

static double stepRateDown (double rate)
{
    double rounded = std::floor (rate * 10.0 + 1e-9) / 10.0;
    if (rounded >= rate - 1e-6)
        rounded -= 0.1;
    return std::max (0.1, std::round (rounded * 10.0) / 10.0);
}

static double stepRateUp (double rate)
{
    double rounded = std::ceil (rate * 10.0 - 1e-9) / 10.0;
    if (rounded <= rate + 1e-6)
        rounded += 0.1;
    return std::min (4.0, std::round (rounded * 10.0) / 10.0);
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

RBVocalSingAudioProcessorEditor::RBVocalSingAudioProcessorEditor (RBVocalSingAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), audioProcessor (p)
{
    // ── Segmented button helper ───────────────────────────────────────────────
    // Style a TextButton as one segment of a connected pair.
    auto styleSegment = [] (juce::TextButton& btn, int connEdges, const juce::String& tip)
    {
        btn.setClickingTogglesState (true);
        btn.setConnectedEdges (connEdges);
        btn.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff1a1a1a));
        btn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3c3c3c));
        btn.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff777777));
        btn.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        btn.setTooltip (tip);
    };

    // ── Vocal Mode ────────────────────────────────────────────────────────────
    const juce::String vocalTip = "Solo: reads PART VOCALS track. Harmonies: reads HARM1/2/3 tracks.";
    vocalSoloBtn.setRadioGroupId (1);
    styleSegment (vocalSoloBtn, juce::TextButton::ConnectedOnRight, vocalTip);
    vocalSoloBtn.setToggleState (true, juce::dontSendNotification);
    vocalSoloBtn.onClick = [this]()
    {
        if (! vocalSoloBtn.getToggleState()) return;
        audioProcessor.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);
        audioProcessor.reloadChart();
        lastStateCount = -1;
        setSize (kWindowW, harmoniesMode() ? kWindowHHarm : kWindowH);
        resized();
    };
    addAndMakeVisible (vocalSoloBtn);

    vocalHarmBtn.setRadioGroupId (1);
    styleSegment (vocalHarmBtn, juce::TextButton::ConnectedOnLeft, vocalTip);
    vocalHarmBtn.onClick = [this]()
    {
        if (! vocalHarmBtn.getToggleState()) return;
        audioProcessor.vocalMode.store ((int) VocalMode::Harmonies, std::memory_order_relaxed);
        audioProcessor.reloadChart();
        lastStateCount = -1;
        setSize (kWindowW, harmoniesMode() ? kWindowHHarm : kWindowH);
        resized();
    };
    addAndMakeVisible (vocalHarmBtn);

    // ── Lyric Mode ────────────────────────────────────────────────────────────
    const juce::String lyricTip = "Scrolling: lyrics move with notes. Static: shows current phrase text.";
    lyricScrollBtn.setRadioGroupId (2);
    styleSegment (lyricScrollBtn, juce::TextButton::ConnectedOnRight, lyricTip);
    lyricScrollBtn.setToggleState (true, juce::dontSendNotification);
    lyricScrollBtn.onClick = [this]()
    {
        if (lyricScrollBtn.getToggleState()) { lyricMode = LyricMode::Scrolling; audioProcessor.savedLyricMode = 0; }
    };
    addAndMakeVisible (lyricScrollBtn);

    lyricStaticBtn.setRadioGroupId (2);
    styleSegment (lyricStaticBtn, juce::TextButton::ConnectedOnLeft, lyricTip);
    lyricStaticBtn.onClick = [this]()
    {
        if (lyricStaticBtn.getToggleState()) { lyricMode = LyricMode::Static; audioProcessor.savedLyricMode = 1; }
    };
    addAndMakeVisible (lyricStaticBtn);

    // ── Settings button ───────────────────────────────────────────────────────
    settingsButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff2a2a2a));
    settingsButton.setColour (juce::TextButton::textColourOffId, juce::Colours::lightgrey);
    settingsButton.onClick = [this]()
    {
        juce::Component::SafePointer<RBVocalSingAudioProcessorEditor> safe = this;

        SettingsPanel::Callbacks cbs;
        cbs.initFrameInterval  = targetFrameInterval;
        cbs.setFrameInterval   = [safe] (double v) { if (safe) { safe->targetFrameInterval = v; safe->audioProcessor.savedFrameInterval = v; } };
        cbs.initReaperPriority = reaperPriority;
        cbs.setReaperPriority  = [safe] (bool v)   { if (safe) { safe->reaperPriority = v; safe->audioProcessor.savedReaperPriority = v; } };
        // Reflect the actual current renderer state as a combo item ID so the panel
        // shows the right selection even after an auto-close triggered by GL attach.
        if (! glContext.isAttached())
            cbs.initRenderer = 1;
        else if (audioProcessor.savedRenderer == 2)
            cbs.initRenderer = 3;
        else
            cbs.initRenderer = 2;
        cbs.setRenderer = [safe] (int id)
        {
            if (! safe) return;
            if (id == 1)       // CPU (software)
            {
                safe->glContext.detach();
                safe->audioProcessor.savedRenderer = 0;
            }
            else if (id == 2)  // OpenGL - session only (savedRenderer intentionally unchanged)
            {
                // Defer attach so the CalloutBox onChange finishes before the component
                // hierarchy change caused by attachTo() triggers a modal dismiss.
                juce::MessageManager::callAsync ([safe] { if (safe) safe->glContext.attachTo (*safe); });
            }
            else if (id == 3)  // OpenGL - persist
            {
                safe->audioProcessor.savedRenderer = 2;  // write now so it's correct if editor closes
                juce::MessageManager::callAsync ([safe] { if (safe) safe->glContext.attachTo (*safe); });
            }
        };
        cbs.initCalib          = latencyOffsetMs;
        cbs.setCalib           = [safe] (int v)    { if (safe) { safe->latencyOffsetMs = v; safe->audioProcessor.savedLatencyOffsetMs = v; safe->repaint(); } };
        cbs.initSpeed          = pixPerSec;
        cbs.setSpeed           = [safe] (float v)  { if (safe) { safe->pixPerSec = v; safe->audioProcessor.savedPixPerSec = v; safe->repaint(); } };
        cbs.initWrap           = octaveWrap;
        cbs.setWrap            = [safe] (bool v)   { if (safe) { safe->octaveWrap = v; safe->audioProcessor.savedOctaveWrap = v; } };
        cbs.showLoadMidi       = !audioProcessor.reaperApi.isLoaded();
        cbs.loadMidi           = [safe]()           { if (safe) safe->openMidiFile(); };

        auto panel = std::make_unique<SettingsPanel> (audioProcessor, std::move (cbs));
        juce::CallOutBox::launchAsynchronously (
            std::move (panel),
            settingsButton.getBoundsInParent(),
            this);
    };
    addAndMakeVisible (settingsButton);

    // ── Transport buttons (rewind / forward) ─────────────────────────────────
    rewindBtn.setTooltip ("Rewind 5 s (or press Left arrow when plugin is focused)");
    rewindBtn.onClick = [this]()
    {
        if (! audioProcessor.reaperApi.isLoaded() || ! audioProcessor.reaperApi.SetEditCurPos) return;
        audioProcessor.reaperApi.SetEditCurPos (std::max (0.0, cachedPlayTime - 5.0), true, true);
    };
    addAndMakeVisible (rewindBtn);

    forwardBtn.setTooltip ("Forward 5 s (or press Right arrow when plugin is focused)");
    forwardBtn.onClick = [this]()
    {
        if (! audioProcessor.reaperApi.isLoaded() || ! audioProcessor.reaperApi.SetEditCurPos) return;
        audioProcessor.reaperApi.SetEditCurPos (cachedPlayTime + 5.0, true, true);
    };
    addAndMakeVisible (forwardBtn);

    // ── Allow edits toggle (note pitch editing) ───────────────────────────────
    // Only usable when REAPER is connected and the write-side API resolved; otherwise
    // the chart is read-only (file-based fallback or an older REAPER).
    {
        const bool canEdit = audioProcessor.reaperApi.isLoaded()
                          && audioProcessor.reaperApi.canEdit();
        editToggle.setTooltip ("Drag a note bar up/down on the highway to change its pitch. "
                               "Off: clicking the highway only moves the playhead.");
        editToggle.setColour (juce::ToggleButton::textColourId,   juce::Colour (0xffbbbbbb));
        editToggle.setColour (juce::ToggleButton::tickColourId,   juce::Colour (0xff66ccff));
        editToggle.setColour (juce::ToggleButton::tickDisabledColourId, juce::Colour (0xff555555));
        editToggle.setEnabled (canEdit);
        editToggle.onClick = [this]() { editMode = editToggle.getToggleState(); };
        addAndMakeVisible (editToggle);
    }

    setWantsKeyboardFocus (true);

    // Pitch detection fixed to MPM.
    audioProcessor.pitchMethod.store (2, std::memory_order_relaxed);

    // Mode persists in the processor across editor reopen — reflect it in the buttons.
    if (harmoniesMode())
    {
        vocalHarmBtn.setToggleState (true, juce::dontSendNotification);
        vocalSoloBtn.setToggleState (false, juce::dontSendNotification);
    }

    // Restore persisted settings from the processor.
    targetFrameInterval = audioProcessor.savedFrameInterval;
    reaperPriority      = audioProcessor.savedReaperPriority;
    octaveWrap          = audioProcessor.savedOctaveWrap;
    latencyOffsetMs     = audioProcessor.savedLatencyOffsetMs;
    pixPerSec           = audioProcessor.savedPixPerSec;

    if (audioProcessor.savedRenderer == 2)
        glContext.attachTo (*this);

    if (audioProcessor.savedLyricMode == 1)
    {
        lyricStaticBtn.setToggleState  (true,  juce::dontSendNotification);
        lyricScrollBtn.setToggleState  (false, juce::dontSendNotification);
        lyricMode = LyricMode::Static;
    }

    tooltipLaf = std::make_unique<TooltipLAF>();
    tooltipWindow.setLookAndFeel (tooltipLaf.get());
    tooltipWindow.setMillisecondsBeforeTipAppears (600);

    setOpaque (true);
    setSize (kWindowW, harmoniesMode() ? kWindowHHarm : kWindowH);
    setResizable (true, true);
    setResizeLimits (kWindowW, kWindowH, 10000, 10000);

    vblankAttachment = juce::VBlankAttachment (this, [this]() { onFrame(); });
}

RBVocalSingAudioProcessorEditor::~RBVocalSingAudioProcessorEditor()
{
    tooltipWindow.setLookAndFeel (nullptr);
    glContext.detach();
}

bool RBVocalSingAudioProcessorEditor::harmoniesMode() const
{
    return audioProcessor.vocalMode.load (std::memory_order_relaxed)
               == (int) VocalMode::Harmonies;
}

// ─── Highway area helper ──────────────────────────────────────────────────────

juce::Rectangle<int> RBVocalSingAudioProcessorEditor::getHighwayArea() const
{
    const int topStripH = harmoniesMode() ? kLyricH : 0;
    return getLocalBounds()
               .withTop    (topStripH)
               .withBottom (getHeight() - kControlsH - kMessageH - kStatusH - kStatsH - kLyricH);
}

// ─── Mouse / keyboard transport controls ──────────────────────────────────────

// Returns the chartNotes index of the editable note under a component-space point, or -1.
// Uses the geometry cached by the last drawHighway() so the hit zone matches the screen.
int RBVocalSingAudioProcessorEditor::hitTestNote (juce::Point<int> pt) const
{
    if (lastHighwayArea.isEmpty() || ! lastHighwayArea.contains (pt)) return -1;
    if (lastNoteH <= 0.0f) return -1;

    const auto&  notes = audioProcessor.getChartNotes();
    const float  phX   = (float) lastHighwayArea.getX() + (float) lastHighwayArea.getWidth() * 0.3f;
    const double t     = cachedPlayTime - (latencyOffsetMs / 1000.0);

    auto pitchY = [&] (int p) -> float {
        return (float) lastHighwayArea.getBottom() - ((float) p - lastFMinP + 0.5f) * lastNoteH;
    };

    // Test in z-order — Main on top, then Harm2, then Harm3 (matches draw priority, so the
    // lead wins an overlap). In Solo mode only Main is visible.
    const VocalPart order[] = { VocalPart::Main, VocalPart::Harm2, VocalPart::Harm3 };
    const bool harm = harmoniesMode();

    for (VocalPart part : order)
    {
        if (! harm && part != VocalPart::Main) continue;
        for (int i = 0; i < (int) notes.size(); ++i)
        {
            const auto& n = notes[(size_t) i];
            if (n.part != part)          continue;
            if (n.sourceTake == nullptr) continue;  // file-based note → not editable

            const float x1 = phX + (float) (n.startTime - t) * pixPerSec;
            const float x2 = phX + (float) (n.endTime   - t) * pixPerSec;
            if ((float) pt.x < x1 || (float) pt.x > x2) continue;

            const float cy     = pitchY (n.pitch);
            const float barH   = (lastNoteH - 1.0f) * n.thickScale;
            const float halfH  = std::max (barH * 0.5f, 4.0f);  // min grab height for thin lanes
            if ((float) pt.y < cy - halfH || (float) pt.y > cy + halfH) continue;

            return i;
        }
    }
    return -1;
}

void RBVocalSingAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! audioProcessor.reaperApi.isLoaded()) return;

    const auto highway = getHighwayArea();
    if (! highway.contains (e.getPosition())) return;

    // Edit mode: a press on a note bar starts a vertical pitch drag instead of seeking.
    if (editMode && audioProcessor.reaperApi.canEdit())
    {
        const int idx = hitTestNote (e.getPosition());
        if (idx >= 0)
        {
            if (auto* n = audioProcessor.mutableChartNoteForEdit (idx))
            {
                dragActive      = true;
                dragNoteIndex   = idx;
                dragAnchorPitch = n->pitch;
                dragNewPitch    = n->pitch;
                dragAnchorY     = e.y;
                return;
            }
        }
    }

    // Otherwise: seek the playhead (unchanged).
    if (! audioProcessor.reaperApi.SetEditCurPos) return;

    const float  phX      = (float) highway.getX() + (float) highway.getWidth() * 0.3f;
    const double seekTime = cachedPlayTime + (e.x - phX) / (double) pixPerSec;

    audioProcessor.reaperApi.SetEditCurPos (std::max (0.0, seekTime), true, true);
}

void RBVocalSingAudioProcessorEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragActive || lastNoteH <= 0.0f) return;

    // Up = higher pitch. Snap the vertical travel to whole semitones.
    const int delta = (int) std::lround ((double) (dragAnchorY - e.y) / (double) lastNoteH);

    // Clamp to the visible lanes intersected with a safe singable band so a drag can never
    // land on REAPER's reserved marker pitches (0 = range-shift, 105/116 = phrase/overdrive,
    // >= 96 reserved) and silently vanish on the next reload.
    const int lo = std::max ((int) std::ceil  (lastFMinP), 1);
    const int hi = std::min ((int) std::floor (lastFMaxP), 95);
    if (hi < lo) return;

    const int newPitch = juce::jlimit (lo, hi, dragAnchorPitch + delta);
    if (newPitch == dragNewPitch) return;

    if (auto* n = audioProcessor.mutableChartNoteForEdit (dragNoteIndex))
    {
        n->pitch     = newPitch;   // live preview (message thread, same as paint)
        dragNewPitch = newPitch;
        repaint();
    }
}

void RBVocalSingAudioProcessorEditor::mouseUp (const juce::MouseEvent&)
{
    if (! dragActive) return;

    if (dragNewPitch != dragAnchorPitch)
    {
        if (auto* n = audioProcessor.mutableChartNoteForEdit (dragNoteIndex))
        {
            auto& api = audioProcessor.reaperApi;
            if (api.canEdit() && n->sourceTake != nullptr)
            {
                api.Undo_BeginBlock2 (nullptr);
                int pitch = dragNewPitch;  // MIDI_SetNote needs a mutable address; NULL = unchanged
                api.MIDI_SetNote (n->sourceTake, n->sourceNoteIndex,
                                  nullptr, nullptr, nullptr, nullptr, nullptr,
                                  &pitch, nullptr, nullptr);
                api.MarkTrackItemsDirty (n->sourceTrack, n->sourceItem);
                api.Undo_EndBlock2 (nullptr, "Edit vocal note pitch", -1);
                lastStateCount = -1;  // force a clean reload/re-sync on the next poll
            }
        }
    }

    dragActive    = false;
    dragNoteIndex = -1;
}

bool RBVocalSingAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    if (! audioProcessor.reaperApi.isLoaded()) return false;

    if (key == juce::KeyPress::leftKey)
    {
        if (audioProcessor.reaperApi.SetEditCurPos)
            audioProcessor.reaperApi.SetEditCurPos (std::max (0.0, cachedPlayTime - 5.0), true, true);
        return true;
    }
    if (key == juce::KeyPress::rightKey)
    {
        if (audioProcessor.reaperApi.SetEditCurPos)
            audioProcessor.reaperApi.SetEditCurPos (cachedPlayTime + 5.0, true, true);
        return true;
    }
    if (key == juce::KeyPress::downKey)
    {
        if (audioProcessor.reaperApi.CSurf_OnPlayRateChange)
        {
            cachedPlayRate = stepRateDown (cachedPlayRate);
            audioProcessor.reaperApi.CSurf_OnPlayRateChange (cachedPlayRate);
        }
        return true;
    }
    if (key == juce::KeyPress::upKey)
    {
        if (audioProcessor.reaperApi.CSurf_OnPlayRateChange)
        {
            cachedPlayRate = stepRateUp (cachedPlayRate);
            audioProcessor.reaperApi.CSurf_OnPlayRateChange (cachedPlayRate);
        }
        return true;
    }

    return false;
}

// ─── Frame loop (VBlank-driven) ─────────────────────────────────────────────────

void RBVocalSingAudioProcessorEditor::onFrame()
{
    // ── Frame throttle ──────────────────────────────────────────────────────
    const double ticksPerSec = (double) juce::Time::getHighResolutionTicksPerSecond();
    const juce::int64 now = juce::Time::getHighResolutionTicks();

    const double paintBudgetFloor = reaperPriority ? (lastPaintSeconds * kBusyFactor) : 0.0;
    const double effInterval      = juce::jmax (targetFrameInterval, paintBudgetFloor);

    if (effInterval > 0.0)
    {
        const double elapsed = (now - lastFrameTicks) / ticksPerSec;
        if (elapsed < effInterval)
            return;
        lastFrameTicks = now;
    }

    // Perf signals for the status bar.
    {
        const double requested  = (targetFrameInterval > 0.0) ? targetFrameInterval : (1.0 / 60.0);
        const double achievable = juce::jmax (requested,
                                              reaperPriority ? (lastPaintSeconds * kBusyFactor)
                                                             :  lastPaintSeconds);
        framesDropping = achievable > requested * 1.15;
        displayFps     = (achievable > 1.0e-6) ? (1.0 / achievable) : 0.0;
    }

    const bool apiReady = audioProcessor.reaperApi.isLoaded();

    if (apiReady)
    {
        const bool doPoll = (now - lastPollTicks) / ticksPerSec >= 0.05;
        if (doPoll)
        {
            lastPollTicks = now;

            // Suppress chart reload while a pitch drag is in flight — a rebuild would
            // invalidate the dragged note's index mid-drag. mouseUp forces a clean
            // re-sync afterwards by setting lastStateCount = -1.
            if (! dragActive && audioProcessor.reaperApi.GetProjectStateChangeCount)
            {
                const int stateCount = audioProcessor.reaperApi.GetProjectStateChangeCount (nullptr);
                if (stateCount != lastStateCount)
                {
                    lastStateCount = stateCount;
                    audioProcessor.readChartFromReaper();
                }
            }

            const int playState = audioProcessor.reaperApi.GetPlayState
                                      ? audioProcessor.reaperApi.GetPlayState() : 0;
            lastIsPlaying = (playState & 1) != 0;

            if (! lastIsPlaying && audioProcessor.reaperApi.GetCursorPosition)
                cachedPlayTime = audioProcessor.reaperApi.GetCursorPosition();

            if (audioProcessor.reaperApi.Master_GetPlayRate)
                cachedPlayRate = audioProcessor.reaperApi.Master_GetPlayRate (nullptr);
        }

        if (lastIsPlaying)
            cachedPlayTime = audioProcessor.currentPlaybackTime.load (std::memory_order_relaxed);
    }
    else
    {
        cachedPlayTime = audioProcessor.currentPlaybackTime.load (std::memory_order_relaxed);
    }

    const int liveNote    = audioProcessor.currentMidiNote.load (std::memory_order_relaxed);
    const int curLyricMode = (int) lyricMode;
    const bool changed = (cachedPlayTime  != lastPaintTime)
                      || (liveNote        != lastPaintNote)
                      || (lastStateCount  != lastPaintStateCount)
                      || (framesDropping  != lastPaintDropping)
                      || (curLyricMode    != lastPaintLyricMode)
                      || (cachedPlayRate  != lastPaintRate);

    if (changed)
    {
        lastPaintTime       = cachedPlayTime;
        lastPaintNote       = liveNote;
        lastPaintStateCount = lastStateCount;
        lastPaintDropping   = framesDropping;
        lastPaintLyricMode  = curLyricMode;
        lastPaintRate       = cachedPlayRate;

        // Dirty only the dynamic areas — controls panel is static widgets that repaint themselves
        repaint (getLocalBounds().withBottom (getHeight() - kControlsH));
    }
}

// ─── File chooser (fallback for non-Reaper hosts) ─────────────────────────────

void RBVocalSingAudioProcessorEditor::openMidiFile()
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Select PART VOCALS MIDI file",
        juce::File::getSpecialLocation (juce::File::userDesktopDirectory),
        "*.mid,*.midi");

    fileChooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto result = fc.getResult();
            if (result.existsAsFile())
                audioProcessor.loadMidiFile (result);
        });
}

// ─── Layout ───────────────────────────────────────────────────────────────────

void RBVocalSingAudioProcessorEditor::resized()
{
    const int panelW = std::min (getWidth(), kWindowW);
    static constexpr int kRowH      = 28;
    static constexpr int kVocalSegW = 95;   // "Harmonies" is wider; "Solo" matches
    static constexpr int kLyricSegW = 85;   // "Scrolling" is wider; "Static" matches
    static constexpr int kBtnW      = 100;

    const int thirdW = panelW / 3;
    const int rowY   = getHeight() - kControlsH + (kControlsH - kRowH) / 2;

    // ── Left third: Vocal pair centred ───────────────────────────────────────
    {
        const auto slot = juce::Rectangle<int> (0, rowY, thirdW, kRowH);
        const auto area = slot.withSizeKeepingCentre (2 * kVocalSegW, kRowH);
        vocalSoloBtn.setBounds (area.withWidth (kVocalSegW));
        vocalHarmBtn.setBounds (area.withWidth (kVocalSegW).withX (area.getX() + kVocalSegW));
    }

    // ── Middle third: Lyric pair centred ─────────────────────────────────────
    {
        const auto slot = juce::Rectangle<int> (thirdW, rowY, thirdW, kRowH);
        const auto area = slot.withSizeKeepingCentre (2 * kLyricSegW, kRowH);
        lyricScrollBtn.setBounds (area.withWidth (kLyricSegW));
        lyricStaticBtn.setBounds (area.withWidth (kLyricSegW).withX (area.getX() + kLyricSegW));
    }

    // ── Right third: [<< 38px][4px][Settings 100px][4px][>> 38px] centred ───
    {
        static constexpr int kNavW   = 38;
        static constexpr int kNavGap = 4;
        const int groupW  = kNavW + kNavGap + kBtnW + kNavGap + kNavW;
        const auto slot   = juce::Rectangle<int> (2 * thirdW, rowY, thirdW, kRowH);
        const int  groupX = slot.getX() + (slot.getWidth() - groupW) / 2;

        rewindBtn     .setBounds (groupX,                                          rowY, kNavW, kRowH);
        settingsButton.setBounds (groupX + kNavW + kNavGap,                        rowY, kBtnW, kRowH);
        forwardBtn    .setBounds (groupX + kNavW + kNavGap + kBtnW + kNavGap,      rowY, kNavW, kRowH);
    }

    // ── "Allow edits" toggle: top row of the stats band, above the time label ──
    {
        const int statsTop = getHeight() - kControlsH - kMessageH - kStatusH - kStatsH;
        editToggle.setBounds (8, statsTop + 2, 130, kEditRowH - 2);
    }
}

// ─── Paint ────────────────────────────────────────────────────────────────────

void RBVocalSingAudioProcessorEditor::paint (juce::Graphics& g)
{
    const juce::int64 paintStart = juce::Time::getHighResolutionTicks();

    g.setColour (kControlsBg);
    g.fillRect  (juce::Rectangle<int> (0, getHeight() - kControlsH, getWidth(), kControlsH));

    const bool harmonies = harmoniesMode();

    const int  topStripH = harmonies ? kLyricH : 0;

    const auto topArea     = juce::Rectangle<int> (0, 0, getWidth(), topStripH);
    const auto highwayArea = getHighwayArea();
    const auto lyricArea   = juce::Rectangle<int> (0, highwayArea.getBottom(), getWidth(), kLyricH);
    const auto statsArea   = juce::Rectangle<int> (0, lyricArea.getBottom(),   getWidth(), kStatsH);
    const auto statusArea  = juce::Rectangle<int> (0, statsArea.getBottom(),   getWidth(), kStatusH);
    const auto messageArea = juce::Rectangle<int> (0, statusArea.getBottom(),  getWidth(), kMessageH);

    const double t = cachedPlayTime - (latencyOffsetMs / 1000.0);

    if (harmonies)
        drawLyricStrip (g, topArea, t, true);

    drawHighway (g, highwayArea, t);

    drawLyricStrip (g, lyricArea, t, false);

    drawStats (g, statsArea, t);

    drawStatusBar (g, statusArea);

    drawMessageBar (g, messageArea);

    const double paintSec = (juce::Time::getHighResolutionTicks() - paintStart)
                                / (double) juce::Time::getHighResolutionTicksPerSecond();
    lastPaintSeconds = 0.5 * lastPaintSeconds + 0.5 * paintSec;
}
