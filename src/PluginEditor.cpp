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

// ─── Renderer setting ─────────────────────────────────────────────────────────
// The renderer lives in two encodings: the Settings combo item IDs (1/2/3) and the
// processor's persisted savedRenderer (0 = CPU, 2 = GL-persist). These helpers are
// the only place the mapping exists. Invariant: GLSession is NEVER persisted —
// savedRenderer's legal domain is {0, 2}, so a session-only GL choice deliberately
// reverts to CPU on the next editor open (see CLAUDE.md, settings persistence).

enum class Renderer { CPU, GLSession, GLPersist };

static int rendererToComboId (Renderer r)
{
    switch (r)
    {
        case Renderer::GLSession: return 2;
        case Renderer::GLPersist: return 3;
        default:                  return 1;  // CPU
    }
}

static Renderer rendererFromComboId (int id)
{
    return id == 2 ? Renderer::GLSession
         : id == 3 ? Renderer::GLPersist
                   : Renderer::CPU;
}

// Which of the three modes the editor is actually in right now.
static Renderer rendererFromState (bool glAttached, int savedRenderer)
{
    if (! glAttached)       return Renderer::CPU;
    if (savedRenderer == 2) return Renderer::GLPersist;
    return Renderer::GLSession;
}

// ─── Settings Popup ───────────────────────────────────────────────────────────
// Launched as a CalloutBox when the user clicks "⚙ Settings". Owns all the
// "configure-once" controls that were formerly in the always-visible panel.

// Layout constants shared by SettingsPanel::resized() and its height calculation,
// so the panel height can't drift from the row layout (CODE_REVIEW.md 2.4). File
// scope (not class members) because MSVC won't evaluate an in-class-defined
// constexpr function inside a static data member initializer.
namespace settingsLayout
{
    constexpr int kPad    = 14;
    constexpr int kRowH   = 28;
    constexpr int kRowGap = 6;
    constexpr int kSecGap = 14;
    constexpr int kHdrH   = 18;
    constexpr int kHdrGap = 6;
    constexpr int kLblGap = 8;

    // Height of one section: header + gap + N control rows (each row includes its
    // trailing row gap, as laid out by layoutRow).
    constexpr int sectionH (int rows) { return kHdrH + kHdrGap + rows * (kRowH + kRowGap); }
}

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
        float                       initLyricScale     { 1.0f };
        std::function<void(float)>  setLyricScale;
        int                         initSquashGuard    { 0 };   // 0=Off 1=Spacing 2=Auto-shrink 3=Hybrid
        std::function<void(int)>    setSquashGuard;
        bool                        initWrap           { true };
        std::function<void(bool)>   setWrap;
        int                         initPitchTol       { 50 };
        std::function<void(int)>    setPitchTol;
        bool                        initPitchTrail     { true };
        std::function<void(bool)>   setPitchTrail;
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
        static const juce::String kTipLyricSize = "Size of the lyric-strip text. The strips (and the default window height) grow or shrink to match.";
        static const juce::String kTipSquash    = "What scrolling lyrics do when packed words would overlap: push them apart (Spacing), squeeze the text to fit (Auto-shrink), or squeeze a little then push the rest (Hybrid).";
        static const juce::String kTipWrap     = "Wraps your detected pitch by octave to match the chart range when you sing an octave too high or low.";
        static const juce::String kTipPitchTol = "How close to the note you must sing to score GREAT, in cents (100 = one semitone). Twice this distance still counts as CLOSE.";
        static const juce::String kTipTrail    = "Draws the pitch you actually sang as a faint contour on the highway (no snapping), so scoops and drift are visible. Clears when the next pass starts.";
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

        setupLabel (lyricSizeLabel, "Lyrics Size", kTipLyricSize);
        lyricSizeSlider.setSliderStyle (juce::Slider::IncDecButtons);
        lyricSizeSlider.setRange (50, 200, 10);
        lyricSizeSlider.setValue (juce::roundToInt (this->cbs.initLyricScale * 100.0f),
                                  juce::dontSendNotification);
        lyricSizeSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 65, 28);
        lyricSizeSlider.setTextValueSuffix (" %");
        lyricSizeSlider.setTooltip (kTipLyricSize);
        lyricSizeSlider.onValueChange = [this]
        {
            this->cbs.setLyricScale ((float) lyricSizeSlider.getValue() / 100.0f);
        };
        addAndMakeVisible (lyricSizeSlider);

        setupLabel (squashLabel, "Squash Guard", kTipSquash);
        squashCombo.addItem ("Off",         1);
        squashCombo.addItem ("Spacing",     2);
        squashCombo.addItem ("Auto-shrink", 3);
        squashCombo.addItem ("Hybrid",      4);
        squashCombo.setSelectedId (this->cbs.initSquashGuard + 1, juce::dontSendNotification);
        styleCombo (squashCombo);
        squashCombo.setTooltip (kTipSquash);
        squashCombo.onChange = [this]
        {
            this->cbs.setSquashGuard (squashCombo.getSelectedId() - 1);
        };
        addAndMakeVisible (squashCombo);

        // ── Singing ───────────────────────────────────────────────────────────
        setupLabel (wrapLabel, "Octave Wrapping", kTipWrap);
        setupToggle (wrapToggle, this->cbs.initWrap, kTipWrap);
        wrapToggle.onClick = [this]
        {
            this->cbs.setWrap (wrapToggle.getToggleState());
        };

        setupLabel (trailLabel, "Pitch Trail", kTipTrail);
        setupToggle (trailToggle, this->cbs.initPitchTrail, kTipTrail);
        trailToggle.onClick = [this]
        {
            this->cbs.setPitchTrail (trailToggle.getToggleState());
        };

        setupLabel (pitchTolLabel, "Pitch Tolerance", kTipPitchTol);
        pitchTolSlider.setSliderStyle (juce::Slider::IncDecButtons);
        pitchTolSlider.setRange (25, 100, 25);
        pitchTolSlider.setValue (this->cbs.initPitchTol, juce::dontSendNotification);
        pitchTolSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 65, 28);
        pitchTolSlider.setTextValueSuffix (" cents");
        pitchTolSlider.setTooltip (kTipPitchTol);
        pitchTolSlider.onValueChange = [this]
        {
            this->cbs.setPitchTol ((int) pitchTolSlider.getValue());
        };
        addAndMakeVisible (pitchTolSlider);

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

        // The NON-REAPER section: a gap, a header, and one gapless button row.
        static constexpr int kExtraH = settingsLayout::kSecGap + settingsLayout::kHdrH
                                     + settingsLayout::kHdrGap + settingsLayout::kRowH;
        setSize (kW, kH + (this->cbs.showLoadMidi ? kExtraH : 0));
    }

    void resized() override
    {
        using namespace settingsLayout;

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
        layoutRow (lyricSizeLabel, lyricSizeSlider);
        layoutRow (squashLabel, squashCombo, 130);

        area.removeFromTop (kSecGap);
        layoutHdr (singingHeader);
        layoutRow (wrapLabel, wrapToggle, 24);
        layoutRow (trailLabel, trailToggle, 24);
        layoutRow (pitchTolLabel, pitchTolSlider);
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
    // PERFORMANCE (3 rows) + HIGHWAY (4) + SINGING (4), two inter-section gaps,
    // padding top and bottom. The optional NON-REAPER section adds kExtraH (ctor).
    static constexpr int kH = 2 * settingsLayout::kPad
                            + settingsLayout::sectionH (3)
                            + settingsLayout::kSecGap + settingsLayout::sectionH (4)
                            + settingsLayout::kSecGap + settingsLayout::sectionH (4);

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
    juce::Label         lyricSizeLabel;
    juce::Slider        lyricSizeSlider;
    juce::Label         squashLabel;
    juce::ComboBox      squashCombo;

    juce::Label         wrapLabel;
    juce::ToggleButton  wrapToggle;
    juce::Label         trailLabel;
    juce::ToggleButton  trailToggle;
    juce::Label         pitchTolLabel;
    juce::Slider        pitchTolSlider;
    juce::Label         gateLabel;
    juce::Slider        gateSlider;

    juce::Label         nonReaperHeader;
    juce::TextButton    loadMidiBtn    { "Load MIDI File" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsPanel)
};

// Wraps the SettingsPanel in a vertical-scroll viewport sized to the space the editor
// can offer: when the panel fits, this renders identically to the bare panel (no
// scrollbar, same size); when it doesn't (small window, or the panel outgrew the
// default height as settings rows were added), it scrolls instead of clipping.
class SettingsScroller : public juce::Component
{
public:
    SettingsScroller (std::unique_ptr<SettingsPanel> panel, int maxHeight)
    {
        const int panelW = panel->getWidth();
        const int panelH = panel->getHeight();
        viewport.setViewedComponent (panel.release(), true);   // viewport takes ownership
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        const bool scrolling = panelH > maxHeight;
        setSize (panelW + (scrolling ? viewport.getScrollBarThickness() : 0),
                 juce::jmin (panelH, maxHeight));
    }

    void resized() override { viewport.setBounds (getLocalBounds()); }

private:
    juce::Viewport viewport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsScroller)
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
        setSize (kWindowW, harmoniesMode() ? windowHHarm() : windowH());
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
        setSize (kWindowW, harmoniesMode() ? windowHHarm() : windowH());
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
        cbs.initRenderer = rendererToComboId (rendererFromState (glContext.isAttached(),
                                                                 audioProcessor.savedRenderer));
        cbs.setRenderer = [safe] (int id)
        {
            if (! safe) return;
            switch (rendererFromComboId (id))
            {
                case Renderer::CPU:
                    safe->glContext.detach();
                    safe->audioProcessor.savedRenderer = 0;
                    break;

                case Renderer::GLSession:  // savedRenderer intentionally unchanged (never persisted)
                    // Defer attach so the CalloutBox onChange finishes before the component
                    // hierarchy change caused by attachTo() triggers a modal dismiss.
                    juce::MessageManager::callAsync ([safe] { if (safe) safe->glContext.attachTo (*safe); });
                    break;

                case Renderer::GLPersist:
                    safe->audioProcessor.savedRenderer = 2;  // write now so it's correct if editor closes
                    juce::MessageManager::callAsync ([safe] { if (safe) safe->glContext.attachTo (*safe); });
                    break;
            }
        };
        cbs.initCalib          = latencyOffsetMs;
        cbs.setCalib           = [safe] (int v)    { if (safe) { safe->latencyOffsetMs = v; safe->audioProcessor.savedLatencyOffsetMs = v; safe->repaint(); } };
        cbs.initSpeed          = pixPerSec;
        cbs.setSpeed           = [safe] (float v)  { if (safe) { safe->pixPerSec = v; safe->audioProcessor.savedPixPerSec = v; safe->repaint(); } };
        cbs.initLyricScale     = lyricScale;
        cbs.setLyricScale      = [safe] (float v)  { if (safe) safe->applyLyricScale (v); };
        cbs.initSquashGuard    = (int) squashGuard;
        cbs.setSquashGuard     = [safe] (int v)    { if (safe) { safe->squashGuard = (SquashMode) juce::jlimit (0, 3, v); safe->audioProcessor.savedSquashGuard = v; safe->repaint(); } };
        cbs.initWrap           = octaveWrap;
        cbs.setWrap            = [safe] (bool v)   { if (safe) { safe->octaveWrap = v; safe->audioProcessor.savedOctaveWrap = v; } };
        cbs.initPitchTol       = pitchTolCents;
        cbs.setPitchTol        = [safe] (int v)    { if (safe) { safe->pitchTolCents = juce::jlimit (25, 100, v); safe->audioProcessor.savedPitchTolCents = safe->pitchTolCents; } };
        cbs.initPitchTrail     = pitchTrailOn;
        cbs.setPitchTrail      = [safe] (bool v)   { if (safe) { safe->pitchTrailOn = v; safe->audioProcessor.savedPitchTrail = v; safe->repaint(); } };
        cbs.showLoadMidi       = !audioProcessor.reaperApi.isLoaded();
        cbs.loadMidi           = [safe]()           { if (safe) safe->openMidiFile(); };

        auto panel = std::make_unique<SettingsPanel> (audioProcessor, std::move (cbs));

        // The popup must stay editor-parented (`this`) so it inherits the host's
        // editor scale transform. A nullptr/desktop parent was tried twice
        // (2026-07-18) to stop the panel clipping at small window heights and
        // reverted both times — see the CallOutBox gotcha in CLAUDE.md. Instead the
        // panel scrolls inside SettingsScroller when taller than the window (minus
        // headroom for the CallOutBox border + arrow).
        auto content = std::make_unique<SettingsScroller> (std::move (panel),
                                                           juce::jmax (100, getHeight() - 50));
        juce::CallOutBox::launchAsynchronously (
            std::move (content),
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

    // ── Save take button (pitch trail → MIDI export) ──────────────────────────
    // Starts disabled: the trail is empty until a pass is sung. The onFrame() poll
    // gate re-evaluates enablement (export API resolved + frozen non-empty trail).
    saveTakeBtn.setTooltip ("Save the current sung take (the pitch trail) as MIDI notes "
                            "on a new track below this one. Stop playback first - "
                            "stopping freezes the take.");
    saveTakeBtn.setEnabled (false);
    saveTakeBtn.onClick = [this]() { saveCurrentTake(); };
    addAndMakeVisible (saveTakeBtn);

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
    pitchTolCents       = juce::jlimit (25, 100, audioProcessor.savedPitchTolCents.load());
    pitchTrailOn        = audioProcessor.savedPitchTrail;
    latencyOffsetMs     = audioProcessor.savedLatencyOffsetMs;
    pixPerSec           = audioProcessor.savedPixPerSec;
    lyricScale          = audioProcessor.savedLyricScale;
    squashGuard         = (SquashMode) juce::jlimit (0, 3, audioProcessor.savedSquashGuard.load());

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
    setSize (kWindowW, harmoniesMode() ? windowHHarm() : windowH());
    setResizable (true, true);
    setResizeLimits (kWindowW, windowH(), 10000, 10000);

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

// ─── Lyrics size setting ──────────────────────────────────────────────────────

void RBVocalSingAudioProcessorEditor::applyLyricScale (float newScale)
{
    if (newScale == lyricScale) return;

    const int oldStripH = lyricStripH();
    lyricScale = newScale;
    audioProcessor.savedLyricScale = newScale;

    // Delta-based resize: grow/shrink by the strip-height change × visible strips so
    // a user-resized window keeps its custom height (unlike the vocal-mode toggles,
    // which reset to the default). Capture the target height before setResizeLimits —
    // it clamps the current bounds to the new minimum, which would double-count the
    // delta when scaling up from the default height.
    const int nStrips = harmoniesMode() ? 2 : 1;
    const int targetH = getHeight() + (lyricStripH() - oldStripH) * nStrips;
    setResizeLimits (kWindowW, windowH(), 10000, 10000);
    setSize (getWidth(), targetH);
    resized();
    repaint();
}

// ─── Highway area helper ──────────────────────────────────────────────────────

juce::Rectangle<int> RBVocalSingAudioProcessorEditor::getHighwayArea() const
{
    const int topStripH = harmoniesMode() ? lyricStripH() : 0;
    return getLocalBounds()
               .withTop    (topStripH)
               .withBottom (getHeight() - kControlsH - kMessageH - kStatusH - kStatsH - lyricStripH());
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

    // Map through the same calibrated time the highway is drawn with (and hitTestNote
    // uses), so a click lands on the note the user sees, not latencyOffsetMs away.
    const float  phX      = (float) highway.getX() + (float) highway.getWidth() * 0.3f;
    const double seekTime = (cachedPlayTime - latencyOffsetMs / 1000.0)
                          + (e.x - phX) / (double) pixPerSec;

    audioProcessor.reaperApi.SetEditCurPos (std::max (0.0, seekTime), true, true);
}

void RBVocalSingAudioProcessorEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragActive || lastNoteH <= 0.0f) return;

    // Up = higher pitch. Snap the vertical travel to whole semitones.
    const int delta = (int) std::lround ((double) (dragAnchorY - e.y) / (double) lastNoteH);

    // Clamp to the visible lanes intersected with a safe singable band so a drag can never
    // land on a reserved marker pitch (kRangeShiftPitch, kPhraseMarkerPitch/kOverdrivePitch,
    // >= kFirstReservedPitch) and silently vanish on the next reload.
    const int lo = std::max ((int) std::ceil  (lastFMinP), kRangeShiftPitch + 1);
    const int hi = std::min ((int) std::floor (lastFMaxP), kFirstReservedPitch - 1);
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

// ─── Save take (pitch trail → MIDI on a new track) ────────────────────────────
// TAKE_EXPORT.md. Message thread (button handler) — REAPER calls are safe here.
// The trail is frozen: enablement requires a stopped transport, and updateNoteHits()
// stops appending the moment playback stops.
// Trail samples are raw project time, so the export deliberately ignores Calibration:
// the notes line up with the audio waveform, and a chart built from them is neutral
// about whatever monitoring latency this rig (or the target console) happens to have.

void RBVocalSingAudioProcessorEditor::saveCurrentTake()
{
    auto& api = audioProcessor.reaperApi;
    if (lastIsPlaying || ! api.isLoaded() || ! api.canExportTake())
        return;   // enablement prevents this; defensive against a stale click

    const auto flash = [this] (const juce::String& msg)
    {
        flashMessage = msg;
        flashUntil   = juce::Time::getMillisecondCounterHiRes() * 0.001 + 4.0;
        repaint();
    };

    const auto notes = rbtake::segmentPitchTrail (pitchTrail);
    if (notes.empty())
    {
        flash ("No take to save - sing a pass first.");
        return;
    }

    // Insert below the plugin's own track, asked from REAPER *now* (a handover-time
    // capture can be null — the FX isn't attached to its track yet during
    // instantiation). IP_TRACKNUMBER is 1-based, which is exactly the 0-based index
    // of the slot after the track; 0 / -1 (not found / master) falls back to
    // appending at the end of the project.
    int insertIdx = api.CountTracks (nullptr);
    if (audioProcessor.reaperHost != nullptr)
        if (void* selfTrack = audioProcessor.reaperHost->getReaperParent (1))
        {
            const int trackNum = (int) api.GetMediaTrackInfo_Value (selfTrack, "IP_TRACKNUMBER");
            if (trackNum > 0)
                insertIdx = trackNum;
        }

    const juce::String trackName = "RB Vocal Sing "
        + juce::Time::getCurrentTime().formatted ("%Y%m%d %H%M");

    api.Undo_BeginBlock2 (nullptr);

    api.InsertTrackAtIndex (insertIdx, false);   // false = bare track, no default FX
    if (api.TrackList_AdjustWindows)
        api.TrackList_AdjustWindows (false);

    void* newTrack = api.GetTrack (nullptr, insertIdx);
    if (newTrack == nullptr)
    {
        api.Undo_EndBlock2 (nullptr, "Save RB vocal take as MIDI", -1);
        flash ("Take export failed - could not create track.");
        return;
    }

    // GetSetMediaTrackInfo_String reads the new value through the (mutable) buffer.
    char nameBuf[64] = {};
    trackName.copyToUTF8 (nameBuf, sizeof (nameBuf));
    api.GetSetMediaTrackInfo_String (newTrack, "P_NAME", nameBuf, true);

    void* item = api.CreateNewMIDIItemInProj (newTrack, notes.front().startTime,
                                              notes.back().endTime, nullptr);  // seconds
    void* take = item != nullptr ? api.GetActiveTake (item) : nullptr;
    if (take == nullptr)
    {
        api.Undo_EndBlock2 (nullptr, "Save RB vocal take as MIDI", -1);
        flash ("Take export failed - could not create MIDI item.");
        lastStateCount = -1;   // the bare track was still inserted → clean reload
        return;
    }

    const bool noSort = true;
    for (const auto& n : notes)
        api.MIDI_InsertNote (take, false, false,
                             api.MIDI_GetPPQPosFromProjTime (take, n.startTime),
                             api.MIDI_GetPPQPosFromProjTime (take, n.endTime),
                             0, n.pitch, 96, &noSort);
    api.MIDI_Sort (take);
    api.MarkTrackItemsDirty (newTrack, item);

    api.Undo_EndBlock2 (nullptr, "Save RB vocal take as MIDI", -1);
    if (api.UpdateArrange)
        api.UpdateArrange();

    // No track/item/take pointers survive this handler — an undo of the export must
    // leave nothing stale. The forced reload re-syncs the chart view.
    lastStateCount = -1;
    flash ("Saved take: " + juce::String ((int) notes.size()) + " notes - " + trackName);
}

bool RBVocalSingAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
#if RB_PAINT_PROFILE
    // Profile builds: 'S' cycles fixed window-size presets so measurement-grid rows use
    // identical geometry across sessions (manual resizing isn't reproducible). Sits
    // before the REAPER-API guard — resizing needs no API.
    if (key.getTextCharacter() == 's' || key.getTextCharacter() == 'S')
    {
        const int defaultH = harmoniesMode() ? windowHHarm() : windowH();
        profSizePreset = (profSizePreset + 1) % 3;
        if      (profSizePreset == 0) setSize (kWindowW,     defaultH);
        else if (profSizePreset == 1) setSize (kWindowW * 2, defaultH);
        else                          setSize (3200,         1800);
        return true;
    }
#endif

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

    // Detector hop rate: differentiate the processor's monotonic counter over ~1 s.
    // Unsigned wraparound subtracts correctly, so no overflow handling is needed.
#if RB_PITCH_PROFILE
    {
        const double since = (now - lastHopRateTicks) / ticksPerSec;
        if (lastHopRateTicks == 0)
        {
            lastHopRateTicks = now;
            lastHopCountSnap = audioProcessor.profDetectCount.load (std::memory_order_relaxed);
        }
        else if (since >= 1.0)
        {
            const uint32_t nowCount = audioProcessor.profDetectCount.load (std::memory_order_relaxed);
            displayHopsPerSec = (double) (uint32_t) (nowCount - lastHopCountSnap) / since;
            lastHopCountSnap  = nowCount;
            lastHopRateTicks  = now;
        }
    }
#endif

    const bool apiReady = audioProcessor.reaperApi.isLoaded();

    if (apiReady)
    {
        const bool doPoll = (now - lastPollTicks) / ticksPerSec >= 0.05;
        if (doPoll)
        {
            lastPollTicks = now;

            // The REAPER handover can complete after the editor is constructed, so the
            // constructor's one-time "Allow edits" enablement may have seen a not-yet-
            // loaded API. Re-evaluate here (two pointer checks) so the toggle wakes up
            // without a close/reopen. (CODE_REVIEW.md 1.3)
            const bool canEditNow = audioProcessor.reaperApi.canEdit();
            if (canEditNow != editToggle.isEnabled())
            {
                editToggle.setEnabled (canEditNow);
                if (! canEditNow)
                {
                    editToggle.setToggleState (false, juce::dontSendNotification);
                    editMode = false;
                }
            }

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

            // Playing = bit 0 set and NOT paused (bit 1). REAPER can report paused
            // playback with both bits set; a paused transport must never count as
            // playing here — audio isn't running, so no sung time can accumulate and
            // the deferred highlight reset must not fire on paused cursor moves.
            const bool nowPlaying = (playState & 1) != 0 && (playState & 2) == 0;

            // Every playback start is a new sung pass: clear the note-hit highlights.
            // Explicit alongside updateNoteHits()'s jump detection so the reset
            // doesn't depend on REAPER's cursor-on-stop preference (stop-in-place
            // would otherwise continue the old take).
            if (nowPlaying && ! lastIsPlaying)
                hitResetPending = true;

            lastIsPlaying = nowPlaying;

            // "Save take" wakes up the same way as the edit toggle, but additionally
            // needs a frozen non-empty trail: stopped transport (stopping is what
            // freezes the trail) with a take still on screen.
            const bool canSaveNow = audioProcessor.reaperApi.canExportTake()
                                 && ! pitchTrail.empty() && ! lastIsPlaying;
            if (canSaveNow != saveTakeBtn.isEnabled())
                saveTakeBtn.setEnabled (canSaveNow);

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

    // Accumulate note-hit coverage and the pitch trail against this frame's final
    // play time.
    updateNoteHits();

    const float livePitch    = audioProcessor.currentMidiFloat.load (std::memory_order_relaxed);
    const int   curLyricMode = (int) lyricMode;
    // Flash visibility participates in the idle gate so the message-bar flash both
    // appears and expires while the transport sits idle (nothing else would repaint).
    const bool  flashActive  = juce::Time::getMillisecondCounterHiRes() * 0.001 < flashUntil;
    const bool changed = (cachedPlayTime  != lastPaintTime)
                      || (livePitch       != lastPaintPitch)
                      || (lastStateCount  != lastPaintStateCount)
                      || (framesDropping  != lastPaintDropping)
                      || (curLyricMode    != lastPaintLyricMode)
                      || (cachedPlayRate  != lastPaintRate)
                      || (flashActive     != lastPaintFlash);

    if (changed)
    {
        lastPaintTime       = cachedPlayTime;
        lastPaintPitch      = livePitch;
        lastPaintStateCount = lastStateCount;
        lastPaintDropping   = framesDropping;
        lastPaintLyricMode  = curLyricMode;
        lastPaintRate       = cachedPlayRate;
        lastPaintFlash      = flashActive;

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
        // Chain off getHighwayArea() (highway → lyric strip → stats band) instead of
        // re-deriving the bottom-band sum (CODE_REVIEW.md 2.6).
        const int statsTop = getHighwayArea().getBottom() + lyricStripH();
        editToggle.setBounds (8, statsTop + 2, 130, kEditRowH - 2);
        saveTakeBtn.setBounds (146, statsTop + 2, 90, kEditRowH - 2);
    }
}

// ─── Paint ────────────────────────────────────────────────────────────────────

void RBVocalSingAudioProcessorEditor::paint (juce::Graphics& g)
{
    const juce::int64 paintStart = juce::Time::getHighResolutionTicks();

    g.setColour (kControlsBg);
    g.fillRect  (juce::Rectangle<int> (0, getHeight() - kControlsH, getWidth(), kControlsH));

    // Every band chains off getHighwayArea() so paint() can't drift from the layout
    // used by resized() and mouse hit-testing (CODE_REVIEW.md 2.6). The top strip is
    // whatever sits above the highway — empty in Solo mode.
    const auto highwayArea = getHighwayArea();
    const auto topArea     = getLocalBounds().withBottom (highwayArea.getY());
    const auto lyricArea   = juce::Rectangle<int> (0, highwayArea.getBottom(), getWidth(), lyricStripH());
    const auto statsArea   = juce::Rectangle<int> (0, lyricArea.getBottom(),   getWidth(), kStatsH);
    const auto statusArea  = juce::Rectangle<int> (0, statsArea.getBottom(),   getWidth(), kStatusH);
    const auto messageArea = juce::Rectangle<int> (0, statusArea.getBottom(),  getWidth(), kMessageH);

    const double t = cachedPlayTime - (latencyOffsetMs / 1000.0);

    // Per-section timing (RB_PAINT_PROFILE builds only): accumulate raw ms per section
    // this frame, folded into the prof* EMAs after the draws. The whole-paint EMA at
    // the bottom (lastPaintSeconds — the duty-cycle cap's input) stays untouched.
#if RB_PAINT_PROFILE
    auto timed = [] (double& accumMs, auto&& drawFn)
    {
        const juce::int64 s = juce::Time::getHighResolutionTicks();
        drawFn();
        accumMs += (juce::Time::getHighResolutionTicks() - s) * 1000.0
                       / (double) juce::Time::getHighResolutionTicksPerSecond();
    };
#else
    auto timed = [] (double&, auto&& drawFn) { drawFn(); };
#endif
    double msHw = 0.0, msLy = 0.0, msSt = 0.0, msBar = 0.0;

    if (! topArea.isEmpty())
        timed (msLy, [&] { drawLyricStrip (g, topArea, t, true); });

    timed (msHw,  [&] { drawHighway (g, highwayArea, t); });

    timed (msLy,  [&] { drawLyricStrip (g, lyricArea, t, false); });

    timed (msSt,  [&] { drawStats (g, statsArea, t); });

    timed (msBar, [&] { drawStatusBar (g, statusArea); });

    timed (msBar, [&] { drawMessageBar (g, messageArea); });

#if RB_PAINT_PROFILE
    profHighwayMs = 0.5 * profHighwayMs + 0.5 * msHw;
    profLyricsMs  = 0.5 * profLyricsMs  + 0.5 * msLy;
    profStatsMs   = 0.5 * profStatsMs   + 0.5 * msSt;
    profBarsMs    = 0.5 * profBarsMs    + 0.5 * msBar;
#endif

    const double paintSec = (juce::Time::getHighResolutionTicks() - paintStart)
                                / (double) juce::Time::getHighResolutionTicksPerSecond();
    lastPaintSeconds = 0.5 * lastPaintSeconds + 0.5 * paintSec;
}
