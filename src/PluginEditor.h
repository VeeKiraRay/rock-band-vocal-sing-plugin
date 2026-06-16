#pragma once
#include "PluginProcessor.h"

class RBVocalSingAudioProcessorEditor
    : public juce::AudioProcessorEditor
{
public:
    explicit RBVocalSingAudioProcessorEditor (RBVocalSingAudioProcessor&);
    ~RBVocalSingAudioProcessorEditor() override;

    void paint   (juce::Graphics&) override;
    void resized ()                override;

private:
    void onFrame         ();   // VBlank-driven: throttled REAPER polling + repaint
    void drawHighway     (juce::Graphics&, juce::Rectangle<int>, double playTime);
    // harmonyStrip=false → bottom strip (Main/HARM1 lyrics); true → top strip (HARM2+HARM3).
    void drawLyricStrip  (juce::Graphics&, juce::Rectangle<int>, double playTime, bool harmonyStrip);
    void drawStats       (juce::Graphics&, juce::Rectangle<int>, double playTime);
    void drawStatusBar   (juce::Graphics&, juce::Rectangle<int>);   // fps / priority / renderer info
    void drawMessageBar  (juce::Graphics&, juce::Rectangle<int>);   // chart status + frame-skip warning
    void openMidiFile    ();

    void mouseDown  (const juce::MouseEvent&) override;
    void mouseDrag  (const juce::MouseEvent&) override;
    void mouseUp    (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&)   override;
    juce::Rectangle<int> getHighwayArea() const;

    // Returns the chartNotes index of the editable note under a component-space point,
    // or -1 if none. Uses the geometry cached by the last drawHighway() so the hit zone
    // matches what's on screen. See note-editing in CLAUDE.md.
    int hitTestNote (juce::Point<int>) const;

    bool harmoniesMode() const;   // true when the Harmonies mode is selected

    enum class LyricMode { Scrolling, Static };
    LyricMode lyricMode { LyricMode::Scrolling };

    RBVocalSingAudioProcessor& audioProcessor;

    // ── Always-visible controls ───────────────────────────────────────────────
    // Vocal Mode segmented pair (radio group 1)
    juce::TextButton vocalSoloBtn   { "Solo" };
    juce::TextButton vocalHarmBtn   { "Harmonies" };
    // Lyric Mode segmented pair (radio group 2)
    juce::TextButton lyricScrollBtn { "Scrolling" };
    juce::TextButton lyricStaticBtn { "Static" };

    juce::TextButton settingsButton { "Settings" };
    juce::TextButton rewindBtn      { "<<" };
    juce::TextButton forwardBtn     { ">>" };

    // Opt-in note editing. Sits in its own row above the time label in the stats band.
    // When checked, a mouse-press on a note bar drags its pitch; otherwise the press
    // seeks the playhead (unchanged). Disabled (and forced off) unless REAPER is
    // connected and the write-side API resolved. Session-only — never persisted.
    juce::ToggleButton editToggle { "Allow edits" };
    bool editMode { false };

    // ── Note drag-edit state (message thread only) ────────────────────────────
    bool  dragActive      { false };
    int   dragNoteIndex   { -1 };   // index into chartNotes of the note being dragged
    int   dragAnchorPitch { 0 };    // pitch at mouse-down
    int   dragNewPitch    { 0 };    // current previewed pitch
    int   dragAnchorY     { 0 };    // mouse y at mouse-down

    // Geometry of the last highway paint, cached so mouse hit-testing matches the
    // on-screen bars (fMinP/fMaxP/noteH are animated per-frame). Written in drawHighway().
    juce::Rectangle<int> lastHighwayArea;
    float lastFMinP { 0.0f };
    float lastFMaxP { 0.0f };
    float lastNoteH { 0.0f };

    // Drives tooltip display for all controls (main panel + settings popup).
    // Uses a custom LookAndFeel (tooltipLaf) for 15pt font — must outlive tooltipWindow.
    std::unique_ptr<juce::LookAndFeel> tooltipLaf;
    juce::TooltipWindow tooltipWindow { this };

    std::unique_ptr<juce::FileChooser> fileChooser;

    // ── Frame driving ────────────────────────────────────────────────────────
    // Repaints are driven by the display's VBlank, not a Timer. This decouples
    // render rate (≈ display refresh) from REAPER API polling, which is gated to
    // ~20 Hz below. See chartchotic's PluginEditor for the reference pattern.
    juce::VBlankAttachment vblankAttachment;

    // Opt-in GPU rendering. Off by default (software). When enabled via the Settings
    // popup, this context is attached to the editor so JUCE rasterizes paint() on the
    // GPU from its own render thread. Detached again when toggled off. Session-only.
    juce::OpenGLContext glContext;

    // Frame cap. 0 = uncapped (free VBlank). Otherwise the minimum seconds between
    // rendered frames. Default 60 fps so high-refresh monitors don't flood REAPER's
    // shared UI thread with 120–144 repaints/sec.
    double      targetFrameInterval { 1.0 / 60.0 };
    juce::int64 lastFrameTicks      { 0 };

    // REAPER API poll gate (~20 Hz). REAPER calls only fire when this elapses.
    juce::int64 lastPollTicks       { 0 };

    // Cached per-frame state (updated in onFrame on the message thread)
    double cachedPlayTime   { 0.0 };
    int    lastStateCount   { -1 };
    bool   lastIsPlaying    { false };  // last play-state decision (refreshed in poll gate)

    // Idle-repaint gating: skip repaint() when nothing visible would change. Sentinels
    // force a paint on the first frame. See onFrame().
    double lastPaintTime       { -1.0e18 };
    int    lastPaintNote       { -2 };
    int    lastPaintStateCount { -2 };
    bool   lastPaintDropping   { false };
    int    lastPaintLyricMode  { -1 };
    double lastPaintRate       { -1.0 };

    // Measured duration of the last paint() (seconds), used by onFrame() to cap paint's
    // duty cycle so a heavy (large-window) paint can't starve REAPER's UI thread.
    double lastPaintSeconds { 0.0 };
    static constexpr double kBusyFactor = 3.0;  // paint ≤ ~1/3 of wall time under load

    // When true, the paint-budget cap is applied (REAPER stays responsive, plugin may skip
    // frames). When false (default), the plugin renders as fast as it can.
    bool reaperPriority { false };

    // Session-only calibration offset (ms) — subtracted from render time in paint().
    int    latencyOffsetMs { 0 };
    // Session-only scroll rate (px/s) — drives both highway and lyric strips together.
    float  pixPerSec       { 200.0f };
    // Session-only playback rate (1.0 = normal). Owned locally since GetPlayRate doesn't
    // load in this REAPER version; stepped by Up/Down keys and passed to CSurf_OnPlayRateChange.
    double cachedPlayRate  { 1.0 };
    // Whether out-of-range singing pitch is wrapped by octave onto the chart range.
    bool  octaveWrap      { true };

    // Perf signals computed in onFrame(), shown by drawStatusBar().
    bool   framesDropping { false };
    double displayFps     { 0.0 };

    static constexpr int   kWindowW   = 600;
    static constexpr int   kControlsH = 40;   // 1 row: three equal thirds
    static constexpr int   kEditRowH  = 22;   // edit-toggle row at the top of the stats band
    static constexpr int   kStatsH    = 70 + kEditRowH;  // edit-toggle row + time/pitch/light row
    static constexpr int   kStatusH   = 22;   // fps / priority / renderer info bar
    static constexpr int   kMessageH  = 22;   // chart status + frame-skip warning bar
    static constexpr int   kLyricH    = 46;   // 28 current row + 18 preview row
    static constexpr float kPixPerSec = 200.0f;  // default seed for pixPerSec member
    // How far back from the left edge to start scanning notes each frame.
    static constexpr double kNoteLookback = 5.0;  // seconds
    // Solo base height; Harmonies adds a second (top) lyric strip. The +kEditRowH keeps
    // the highway height unchanged now that the stats band grew by the edit-toggle row.
    static constexpr int   kWindowH       = 375 + kMessageH + kEditRowH;  // kControlsH + kStatusH + kMessageH + kStatsH + kLyricH + highway
    static constexpr int   kWindowHHarm   = kWindowH + kLyricH;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RBVocalSingAudioProcessorEditor)
};
