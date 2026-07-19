#pragma once
#include "PluginProcessor.h"
#include "TakeExport.h"   // rbtake::TrailPoint (= TrailSample) + segmentPitchTrail

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
    // Accumulates per-note sung-within-tolerance spans and the pitch trail each
    // frame. Body in PluginEditorHighway.cpp next to the scoring helpers it shares
    // with the traffic light.
    void updateNoteHits  ();
    // Re-aligns noteHitSpans to a freshly reloaded chart by note identity (see
    // noteHitKeys) instead of clearing — only actually-changed notes lose their gold.
    void remapNoteHitSpans ();
    void drawStatusBar   (juce::Graphics&, juce::Rectangle<int>);   // fps / priority / renderer info
    void drawMessageBar  (juce::Graphics&, juce::Rectangle<int>);   // chart status + frame-skip warning
    // "Save take" button handler: segments pitchTrail and writes the notes as a MIDI
    // item on a new timestamped track below the plugin's track (TAKE_EXPORT.md).
    void saveCurrentTake ();
    void openMidiFile    ();
    // Settings-popup callback for the Lyrics size row: stores + persists the scale,
    // then re-applies window geometry (delta-based resize; see FONT_SCALING.md).
    void applyLyricScale (float newScale);

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

    // Squash guard — scrolling-strip overlap handling (LYRIC_OVERLAP.md).
    // Persisted as int (savedSquashGuard); order is part of the saved state.
    enum class SquashMode { Off, Spacing, Shrink, Hybrid };
    SquashMode squashGuard { SquashMode::Off };

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

    // Export the current take (pitch trail) as MIDI on a new track. Enabled only when
    // REAPER's export API resolved, a take is on screen, and the transport is stopped
    // (stopping freezes the trail — the button always operates on a finished pass).
    juce::TextButton saveTakeBtn { "Save take" };

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
    // force a paint on the first frame. See onFrame(). The pitch is tracked at
    // fractional precision — the arrowhead moves on sub-semitone changes.
    double lastPaintTime       { -1.0e18 };
    float  lastPaintPitch      { -2.0f };
    int    lastPaintStateCount { -2 };
    bool   lastPaintDropping   { false };
    int    lastPaintLyricMode  { -1 };
    double lastPaintRate       { -1.0 };
    bool   lastPaintFlash      { false };

    // Measured duration of the last paint() (seconds), used by onFrame() to cap paint's
    // duty cycle so a heavy (large-window) paint can't starve REAPER's UI thread.
    double lastPaintSeconds { 0.0 };
    // Per-section paint EMAs (ms), written and shown only in RB_PAINT_PROFILE builds
    // (status-bar hw/ly/st/bar diagnostics). Members exist unconditionally so the
    // class layout is identical under both flag values.
    double profHighwayMs { 0.0 };
    double profLyricsMs  { 0.0 };
    double profStatsMs   { 0.0 };
    double profBarsMs    { 0.0 };
    // drawHighway sub-section EMAs (ms): bg = background fill + section math + lane
    // grid + range labels + playhead; bars = note bars + bend connectors; text =
    // inline lyric labels (timed separately inside the note loop — the fills-vs-text
    // split is what decides §d1); ov = range-shift markers + separators + arrowhead.
    double profHwBgMs   { 0.0 };
    double profHwBarsMs { 0.0 };
    double profHwTextMs { 0.0 };
    double profHwOvMs   { 0.0 };
    // Which fixed window-size preset the profile-build 'S' key applied last (cycles
    // default → 2×-width → big, keyPressed()) so grid measurements are reproducible.
    int    profSizePreset { 0 };
    static constexpr double kBusyFactor = 3.0;  // paint ≤ ~1/3 of wall time under load

    // When true, the paint-budget cap is applied (REAPER stays responsive, plugin may skip
    // frames). When false (default), the plugin renders as fast as it can.
    bool reaperPriority { false };

    // Session-only calibration offset (ms) — subtracted from render time in paint().
    int    latencyOffsetMs { 0 };
    // Session-only scroll rate (px/s) — drives both highway and lyric strips together.
    float  pixPerSec       { 200.0f };
    // Lyric-strip scale (1.0 = 100%) — scales strip text and the strip heights
    // themselves (FONT_SCALING.md). Highway inline labels are not covered.
    float  lyricScale      { 1.0f };
    // Session-only playback rate (1.0 = normal). Owned locally since GetPlayRate doesn't
    // load in this REAPER version; stepped by Up/Down keys and passed to CSurf_OnPlayRateChange.
    double cachedPlayRate  { 1.0 };
    // Whether out-of-range singing pitch is wrapped by octave onto the chart range.
    bool  octaveWrap      { true };
    // Traffic-light GREAT window in cents (100 = one semitone); CLOSE is 2× this.
    // Settings stepper ±25/±50/±75/±100.
    int   pitchTolCents   { 50 };
    // Whether the sung-pitch contour draws on the highway. Off skips capture and
    // draw; the pass-lifecycle clears stay unconditional so stale trail can't linger.
    bool  pitchTrailOn    { true };

    // Note-hit tracking: per-note spans of playback time sung within the scoring
    // tolerance, index-aligned with chartNotes. drawHighway fills the covered
    // portions of each bar gold; the per-note coverage ratio is the building block
    // for phrase-level scoring later. Accumulation only happens while the transport
    // is actually playing (REAPER path) — while stopped the tracked time follows the
    // edit cursor, so cursor moves must not count as singing or fire the reset.
    // Highlights persist while stopped so the take can be reviewed; hitResetPending
    // arms on any time discontinuity (seek, loop wrap) *and* on every stopped→playing
    // transition (onFrame poll gate — uniform across REAPER cursor-on-stop prefs),
    // then the next sung pass starts from a clean slate. Chart reloads remap the
    // spans by note identity (remapNoteHitSpans) instead of clearing. (Could become
    // a setting if a keep-across-passes workflow surfaces.)
    std::vector<std::vector<std::pair<double, double>>> noteHitSpans;
    int    noteHitVersion   { -1 };
    double lastHitTrackTime { -1.0 };   // previous frame's play time; <0 = no history
    bool   hitResetPending  { true };   // clear spans when accumulation next resumes

    // Identity snapshot of the chart the spans are aligned to. On reload, a note
    // matching on (startTime, endTime, part) keeps its gold if its pitch is also
    // unchanged — so editing one note drops only that note's spans, and unrelated
    // project changes (which still trigger a chart re-read) drop nothing.
    struct NoteKey { double start, end; VocalPart part; int pitch; };
    std::vector<NoteKey> noteHitKeys;
    // Spans parked during a transient empty reload (still aligned with the preserved
    // noteHitKeys); the next non-empty remap consumes them, so a mid-flush failed
    // chart read doesn't permanently lose the gold. See remapNoteHitSpans().
    std::vector<std::vector<std::pair<double, double>>> parkedHitSpans;

    // Pitch trail: the sung fractional pitch of the current pass, one sample per
    // accumulating frame, time-ascending. Raw (unsnapped, unwrapped) pitch — octave
    // wrapping is applied per-sample at draw so the toggle and section transitions
    // apply live; pitch < 0 is a gap marker that breaks the drawn contour. Cleared on
    // a new pass with the hit spans; survives chart reloads (references no notes).
    using TrailSample = rbtake::TrailPoint;   // identical layout {double time; float pitch}
    std::vector<TrailSample> pitchTrail;

    // Transient message-bar flash (take-export feedback). Drawn by drawMessageBar with
    // priority over chartStatus until flashUntil (seconds, Time::getMillisecondCounterHiRes
    // clock) — chartStatus alone would be overwritten ~50 ms later by the post-export
    // chart reload.
    juce::String flashMessage;
    double       flashUntil { 0.0 };

    // Scrolling-strip lyric pixel widths (getStringWidthFloat is a per-glyph walk),
    // index-aligned with chartNotes and shared by both strips; -1 = not yet measured
    // (lazily filled in drawLyricStrip). Invalidated via getChartVersion() plus the
    // strip font height, since the Lyrics size setting rescales the font.
    std::vector<float> lyricWidthCache;
    int                lyricWidthCacheVersion { -1 };
    float              lyricWidthCacheFontH   { -1.0f };

    // Scrolling-pill scratch for the squash-guard passes (LYRIC_OVERLAP.md): refilled
    // per strip per frame (collect → adjust → draw). A member so its capacity persists
    // across frames — after warm-up paint does no allocation.
    struct ScrollPill { int noteIdx; float anchorX, x, tw, drawW; };
    std::vector<ScrollPill> pillScratch;

    // Static-lyric-mode phrase layout cache (PERFORMANCE_IMPROVEMENT.md c5): the
    // collected syllable list + measured widths for one (strip, role) slot, rebuilt
    // only when the chart, phrase index, or font changes — while inside a phrase,
    // drawing does no collecting or measuring. Colour stays per-frame (depends on t).
    struct PhrasePartLayout
    {
        juce::String text, glue;
        double       start, end;
        VocalPart    part;
        float        textW, glueW;
    };
    struct PhraseLayoutCache
    {
        int   chartVersion { -1 };
        int   phraseIdx    { -1 };
        float fontHeight   { -1.0f };
        std::vector<PhrasePartLayout> parts;
        float totalW { 0.0f };
    };
    PhraseLayoutCache phraseLayouts[4];   // {bottom, top} strip × {current, preview}
    const PhraseLayoutCache& getPhraseLayout (bool harmonyStrip, bool preview,
                                              int phraseIdx, const PhraseInfo& phrase,
                                              const juce::Font& font);

    // Lyric-strip fonts, constructed once per lyricScale value — juce::Font
    // construction does typeface-cache lookups, so building them per frame in paint
    // was wasted work (PERFORMANCE_IMPROVEMENT.md c6). Rebuilt lazily at the top of
    // drawLyricStrip when the Lyrics size setting changes; base sizes 15 / 11 pt.
    juce::Font lyricFontCurrent { 15.0f, juce::Font::bold };  // pills + static current row
    juce::Font lyricFontPreview { 11.0f, juce::Font::bold };  // static preview row
    float      lyricFontsScale  { 1.0f };  // the lyricScale the fonts were built at

    // Perf signals computed in onFrame(), shown by drawStatusBar().
    bool   framesDropping { false };
    double displayFps     { 0.0 };

    static constexpr int   kWindowW   = 600;
    static constexpr int   kControlsH = 40;   // 1 row: three equal thirds
    static constexpr int   kEditRowH  = 22;   // edit-toggle row at the top of the stats band
    static constexpr int   kStatsH    = 70 + kEditRowH;  // edit-toggle row + time/pitch/light row
    static constexpr int   kStatusH   = 22;   // fps / priority / renderer info bar
    static constexpr int   kMessageH  = 22;   // chart status + frame-skip warning bar
    static constexpr int   kLyricBaseH = 46;  // 28 current row + 18 preview row at 100% scale
    // How far back from the left edge to start scanning notes each frame.
    static constexpr double kNoteLookback = 5.0;  // seconds
    // Solo base height; Harmonies adds a second (top) lyric strip. The +kEditRowH keeps
    // the highway height unchanged now that the stats band grew by the edit-toggle row.
    static constexpr int   kWindowBaseH   = 375 + kMessageH + kEditRowH;  // kControlsH + kStatusH + kMessageH + kStatsH + kLyricBaseH + highway, at 100% lyric scale
    // Strip and window heights are computed from lyricScale (FONT_SCALING.md): the
    // strips scale and the default window height follows, keeping the highway height
    // constant. At 100% these equal the old kLyricH / kWindowH / kWindowHHarm values.
    int lyricStripH() const { return juce::roundToInt (kLyricBaseH * lyricScale); }
    int windowH()     const { return kWindowBaseH - kLyricBaseH + lyricStripH(); }
    int windowHHarm() const { return windowH() + lyricStripH(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RBVocalSingAudioProcessorEditor)
};
