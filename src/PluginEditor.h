#pragma once
#include "PluginProcessor.h"
#include "StemAudioReader.h"
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
    // One sampled detection: {double time; float pitch}. Backs both the pitch trail and
    // the marker history below; declared here because heardDetection() returns one.
    using TrailSample = rbtake::TrailPoint;

    void onFrame         ();   // VBlank-driven: throttled REAPER polling + repaint
    void drawHighway     (juce::Graphics&, juce::Rectangle<int>, double playTime);
    // harmonyStrip=false → bottom strip (Main/HARM1 lyrics); true → top strip (HARM2+HARM3).
    void drawLyricStrip  (juce::Graphics&, juce::Rectangle<int>, double playTime, bool harmonyStrip);
    void drawStats       (juce::Graphics&, juce::Rectangle<int>, double playTime);
    // The detection whose audio the listener is hearing right now: the newest one with a
    // live mic (their voice has already made the round trip), latencyOffsetMs back for
    // playback, whose audio is only now reaching them. Raw {time, pitch}; pitch < 0 =
    // none/unvoiced. Body next to updateNoteHits, which fills the history it reads.
    TrailSample heardDetection() const;
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
    // item on a new timestamped track below the plugin's track (2026-07 take-export work).
    void saveCurrentTake ();
    void openMidiFile    ();
    // Settings-popup callback for the Lyrics size row: stores + persists the scale,
    // then re-applies window geometry (delta-based resize; see 2026-07 lyrics-size work).
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

    // Squash guard — scrolling-strip overlap handling (2026-07 squash-guard work).
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

    // ── Offline stem tools ────────────────────────────────────────────────────
    // Stem mode reads the plugin's own track directly, so the pitch readout describes the
    // *stem under the cursor* rather than the mic. It governs the offline tools only —
    // what the detection settings do is a separate question, answered by "Live-mic used".
    //
    // Persisted through savedStemMode (the audio thread never reads it, so it follows the
    // mirror pattern rather than living on the processor as an atomic).
    juce::ToggleButton stemToggle { "Stem" };
    bool stemMode { false };

    // True when the readout should come from the stem rather than the mic: the mode is on,
    // REAPER is connected with the accessor API, and the transport is stopped. While
    // playing, the stem reaches processBlock like any other audio and the live path is
    // already correct, so this deliberately yields to it.
    bool stemReadoutActive() const;

    // Reads a window ending at the edit cursor, runs it through stemDetector, and files
    // the last hop into detHistory. Called from the poll gate, not every frame.
    void updateStemReadout();

    // Batch sweep: reads the whole stem offline and fills the pitch trail, so Save take
    // can write a reference MIDI with no playback and no singing at all. Chunked across
    // frames rather than run in one go — a 4-minute stem is ~22k detections, and a
    // blocking loop would freeze REAPER for the duration.
    juce::TextButton analyseBtn { "Analyse stem" };
    void startStemSweep();
    void advanceStemSweep();
    void cancelStemSweep();
    void finishStemSweep();

    bool   sweepActive { false };
    double sweepStart  { 0.0 };   // project seconds: where the sweep began
    double sweepPos    { 0.0 };   // project seconds: next sample to read
    double sweepEnd    { 0.0 };   // project seconds: clamped against songEndTime
    rbpitch::Detector        sweepDetector;   // a third instance, independent of both others
    std::vector<TrailSample> sweepTrail;      // swapped into pitchTrail on completion
    std::vector<float>       sweepScratch;

    static constexpr int    kSweepChunk     = 8192;   // samples read per accessor call
    static constexpr double kSweepBudgetSec = 0.020;  // work per frame; the rest is REAPER's

    // Message-bar flash, shared by the take export and the sweep (4 s, outranks
    // chartStatus and the frame-drop warning).
    void flashMsg (const juce::String& msg);

    rbstem::AudioReader stemReader;    // one accessor, held open across frames
    rbpitch::Detector   stemDetector;  // separate instance: never shares the live state
    std::vector<float>  stemScratch;   // read buffer, reused between reads
    double lastStemReadTime { -1.0 };  // cursor position of the last read (skips re-reads)

    // How much audio to analyse per cursor position. The octave guard needs
    // kOctaveRunHops consecutive voiced hops before it believes a rival, so a single
    // window would leave it permanently unable to fire — silently dead in the one mode
    // built to inspect stems for octave errors. 150 ms is ~3600 internal samples: the
    // window fill plus ~11 hops, comfortably past the 6 required.
    static constexpr double kStemReadSec = 0.15;

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
    float  lastPaintHeardPitch { -2.0f };   // see the gate: in stem mode the heard
                                            //   sample moves while the atomic does not
    int    lastPaintStateCount { -2 };
    bool   lastPaintDropping   { false };
    int    lastPaintLyricMode  { -1 };
    double lastPaintRate       { -1.0 };
    bool   lastPaintFlash      { false };
    // Play state is a paint input, not just a poll result: drawingOffsetSec() applies the
    // output calibration only while playing, so the whole chart shifts on the transition.
    // A stop-in-place (cursor doesn't move) changes nothing else the gate watches.
    bool   lastPaintPlaying    { false };
    // Octave-guard chip state. Its own gate inputs because they move independently of
    // pitch — a steady note can cross the doubt threshold without currentMidiFloat
    // changing at all, and the visibility deadline expires with nothing else changing, so
    // without these the chip would not appear (or not clear) while the transport is idle.
    // All constant when the guard is off, so this costs no extra repaints there.
    float  lastPaintOctRatio   { -1.0f };
    bool   lastPaintOctDoubt   { false };
    bool   lastPaintOctVisible { false };
    bool   lastPaintOctDash    { false };

    // Minimum time the octave chip stays on screen after a reading. Rival evidence is
    // per-hop (~94/s) and intermittent, so without a hold the chip changed state faster
    // than a two-digit number can be read. Long enough to bridge a consonant or a few
    // rival-less hops, short enough not to linger after singing stops.
    static constexpr double kOctChipHoldSec = 0.4;
    float  heldOctRatio    { 0.0f };
    bool   heldOctDoubt    { false };
    double heldOctUntil       { 0.0 };   // a reading is still worth showing
    double heldOctVoicedUntil { 0.0 };   // ...and, failing that, "no rival here"
    // Both decided in onFrame(), consumed by drawStats — see the latch there.
    bool   octChipVisible     { false };
    bool   octChipDashVisible { false };

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

    // Output calibration (ms) — how late the song reaches the listener's ears, subtracted
    // from render time in paint() so the note at the playhead line is the one they hear.
    // Applies only while the transport is playing; see drawingOffsetSec().
    int    latencyOffsetMs { 0 };
    // Input calibration (ms) — how late a detection is filed relative to when the sound
    // was actually made: the mic/interface input latency plus the analysis chain (window
    // fill + median, measured in tests/DetectionLatencyTests.cpp at ~5 ms for a phrase-start
    // onset and ~29 ms for a mid-phrase legato step). Independent of latencyOffsetMs
    // because it compensates a different physical delay — sharing one number forced a
    // choice between the chart sitting on the audio and the trail sitting on the chart.
    int    inputOffsetMs  { 0 };
    // Does the *output* offset apply to detected pitch as well? A live singer's voice is
    // timestamped only after the song has travelled out to their ears and back, so that
    // round trip must be shifted back onto the chart on top of the input offset. Playback
    // (a stem, a recorded take) never made that trip and is already at its true project
    // time. See detectionOffsetSec().
    bool   liveMicInput   { false };
    // Session-only scroll rate (px/s) — drives both highway and lyric strips together.
    float  pixPerSec       { 200.0f };
    // Lyric-strip scale (1.0 = 100%) — scales strip text and the strip heights
    // themselves (2026-07 lyrics-size work). Highway inline labels are not covered.
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

    // Pitch trail: the sung fractional pitch of the current pass, one sample per hop,
    // time-ascending. Raw in both axes — unsnapped/unwrapped pitch (octave wrapping is
    // applied per-sample at draw so the toggle and section transitions apply live) and
    // raw project time (drawHighway applies the Calibration offset, so the take export
    // gets true project time); pitch < 0 is a gap marker that breaks the drawn contour.
    // Cleared on a new pass with the hit spans; survives chart reloads (references no notes).
    std::vector<TrailSample> pitchTrail;

    // Recent detections backing heardDetection(), so the playhead-line marker can show
    // the audio being heard rather than the newest analysed. Deliberately independent of
    // the Pitch Trail setting and of the pass lifecycle — a plain rolling window, never
    // cleared. 256 samples is ~2.7 s at the ~94 Hz hop rate, ample for the +500 ms maximum
    // Calibration even with dropped frames.
    static constexpr int kDetHistorySize = 256;
    std::array<TrailSample, kDetHistorySize> detHistory {};
    int detHistoryHead  { 0 };   // next write slot
    int detHistoryCount { 0 };   // entries written, saturating at kDetHistorySize

    // Reusable scratch buffer for draining audioProcessor's per-hop pitch queue
    // in updateNoteHits() — reused every frame to avoid per-frame allocation.
    std::vector<RBVocalSingAudioProcessor::HopSample> hopScratch;
    // Pitch/voicing "in effect" at the end of the last processed hop — carries the
    // held value across frame boundaries so a frame with no new hops (UI faster
    // than the ~16ms hop cadence) still scores against the right ongoing pitch.
    float lastHopMidiF  { -1.0f };
    bool  lastHopVoiced { false };

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

    // Scrolling-pill scratch for the squash-guard passes (2026-07 squash-guard work): refilled
    // per strip per frame (collect → adjust → draw). A member so its capacity persists
    // across frames — after warm-up paint does no allocation.
    struct ScrollPill { int noteIdx; float anchorX, x, tw, drawW; };
    std::vector<ScrollPill> pillScratch;

    // Static-lyric-mode phrase layout cache (2026-07 paint phase, c5): the
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
    // was wasted work (2026-07 paint phase, c6). Rebuilt lazily at the top of
    // drawLyricStrip when the Lyrics size setting changes; base sizes 15 / 11 pt.
    juce::Font lyricFontCurrent { 15.0f, juce::Font::bold };  // pills + static current row
    juce::Font lyricFontPreview { 11.0f, juce::Font::bold };  // static preview row
    float      lyricFontsScale  { 1.0f };  // the lyricScale the fonts were built at

    // Perf signals computed in onFrame(), shown by drawStatusBar().
    bool   framesDropping { false };
    double displayFps     { 0.0 };
    // Achieved detector hop rate (detections/s), differentiated from the processor's
    // monotonic profDetectCount over a ~1 s window in onFrame(). Shown only in
    // RB_PITCH_PROFILE builds; members exist unconditionally (see the paint EMAs).
    double        displayHopsPerSec { 0.0 };
    uint32_t      lastHopCountSnap  { 0 };
    juce::int64   lastHopRateTicks  { 0 };

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
    // Strip and window heights are computed from lyricScale (2026-07 lyrics-size work): the
    // strips scale and the default window height follows, keeping the highway height
    // constant. At 100% these equal the old kLyricH / kWindowH / kWindowHHarm values.
    int lyricStripH() const { return juce::roundToInt (kLyricBaseH * lyricScale); }
    int windowH()     const { return kWindowBaseH - kLyricBaseH + lyricStripH(); }
    int windowHHarm() const { return windowH() + lyricStripH(); }

    // Seconds to subtract from a detection's raw project time to place it on the chart —
    // the single point of truth for the detection timeline. The input offset always
    // applies (the analysis chain files every estimate late regardless of source); the
    // output offset is added only for a live mic, whose voice also made the round trip
    // out to the singer's ears and back (see liveMicInput). Both zero reproduces the
    // pre-split behaviour exactly, which is what an old saved session loads as.
    double detectionOffsetSec() const
    {
        return (inputOffsetMs + (liveMicInput ? latencyOffsetMs : 0)) / 1000.0;
    }

    // Seconds to subtract from cachedPlayTime to get the drawing timeline. Gated on the
    // transport actually playing: while stopped, cachedPlayTime is the edit cursor, a true
    // project position with no audio in flight, so there is no monitoring latency to
    // cancel — applying it anyway drew the chart latencyOffsetMs away from the cursor and
    // made "park the cursor on a note start" report a sync error that wasn't there
    // (found 2026-08-08). Only the REAPER path knows the play state; the file fallback's
    // host playhead only advances during playback, so treat it as always playing.
    // This shifts chart and trail together — both are mapped through the same t — so
    // their alignment to each other is identical in both transport states.
    double drawingOffsetSec() const
    {
        const bool stopped = audioProcessor.reaperApi.isLoaded() && ! lastIsPlaying;
        return stopped ? 0.0 : latencyOffsetMs / 1000.0;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RBVocalSingAudioProcessorEditor)
};
