# vocal-rb-sing — Project Reference

A JUCE 7 VST3 plugin that runs inside REAPER and helps author/test Rock Band vocal charts.
It reads mic input for pitch detection and displays a scrolling note highway synced to the
REAPER transport, letting the user sing along and see how well they match the chart.

**Status:** feature-complete PoC plus post-PoC features (Harmonies mode, Settings popup with
persisted settings, transport navigation, write-back note-pitch editing, save-take MIDI
export). Builds and runs inside REAPER. User-facing documentation lives in **README.md** (installation, setup,
usage) — keep it in sync when behaviour changes.

## Local working context

This file holds stable project knowledge only. Open work, the planned/completed-phase
document index, and other WIP notes live in **CLAUDE.local.md** (gitignored, not part of
a fresh clone) — check it before starting related tasks.

---

## Host support (REAPER vs other DAWs)

Chart source is chosen automatically by whether REAPER's extension API was handed over
(`reaperApi.isLoaded()`). Both paths build the same chart data — only the source differs.

|                         | Inside REAPER (primary)                                              | Other DAWs (fallback)                                       |
| ----------------------- | -------------------------------------------------------------------- | ----------------------------------------------------------- |
| Chart source            | Live project tracks via the REAPER API (`readChartFromReaper`)       | A `.mid` file the user picks (`loadMidiFile`)               |
| Track selection         | Auto: PART VOCALS (Solo) or HARM1/2/3 (Harmonies), by track name     | Same names, matched against `.mid` track-name meta events   |
| Live edits              | Auto-reloads on any project change (edit/undo/tempo)                 | Static — re-pick the file (or toggle Vocal Mode) to refresh |
| "Load MIDI File" button | Hidden                                                               | Shown (in the Settings popup)                               |
| Transport time          | REAPER play/edit cursor (API)                                        | Host playhead via `getPlayHead()`                           |
| Mic pitch detection     | identical                                                            | identical                                                   |

The file-load path is **not dead code** — it is the genuine non-REAPER fallback and is kept
functionally in sync with the REAPER path (Solo + Harmonies, dedup, overlap thickness).
Open question pending user feedback: if nobody runs outside REAPER, the fallback (~130
lines + `juce_audio_formats`) could be deleted.

---

## File layout

```
rock-band-vocal-sing-plugin/          (repo root)
  CMakeLists.txt                  — JUCE 7.0.12 via CPM, VST3 only
  cmake/CPM.cmake                 — package manager
  README.md + assets/             — user-facing docs (screenshots, demo gif)
  CLAUDE.md                       — developer reference (this file)
  _upcoming/ + _old/              — planned / completed working docs (gitignored, local only)
  src/
    PluginProcessor.h             — RBVocalSingAudioProcessor decl + chart data types
    PluginProcessor.cpp           — lifecycle, processBlock, REAPER API handover,
                                    DEF_CLASS_IID (must stay in this one TU)
    PitchDetection.cpp            — detectPitch + Autocorrelation/YIN/MPM (processor members)
    ChartLoader.cpp               — readChartFromReaper / loadMidiFile / parseChartNotes +
                                    MIDI-text decode, pitch sections, harmony post-passes
    TakeExport.h / .cpp           — rbtake::segmentPitchTrail: pure pitch-trail → MIDI-note
                                    segmentation (JUCE-free; unit-tested directly)
    HighwayRange.h                — rbhighway::visibleRange: pure pitch-axis rules (pad,
                                    clamp to the 36–84 vocal band, minimum span). Header-only,
                                    JUCE-free, unit-tested directly
    PluginEditor.h                — RBVocalSingAudioProcessorEditor decl
    PluginEditor.cpp              — editor lifecycle, VBlank frame loop, layout, paint dispatch,
                                    SettingsPanel popup, transport keys, note-pitch edit drag
    PluginEditorHighway.cpp       — drawHighway + drawStats + drawStatusBar + drawMessageBar
    PluginEditorLyrics.cpp        — drawLyricStrip (scrolling + static)
    EditorVisuals.h               — shared paint colours + lyric-suffix helpers (inline)
    reaper_api.h                  — IReaperHostApplication + typed ReaperApi struct
    extern/reaper_vst3_interfaces.h — Cockos interface (copied from JUCE examples)
  build/                          — cmake output (gitignored)
```

The processor and editor are each one class implemented across several `.cpp` files
(multi-TU split for readability). When adding a member function, put its body in the file
that owns that concern; declaration stays in the header.

---

## Input routing (pitch detection source)

The plugin pitch-detects whatever audio REAPER delivers into its input bus (channel 0 in
`processBlock`). The chart is read separately via the REAPER API — independent of which
track hosts the plugin. Key user-facing facts (full setup walkthrough is in README.md):

- Record arm is not enough — the track needs **input monitoring** for mic audio to reach
  `processBlock`.
- FX order matters on synth tracks (before synth = track input; after = synth output).
- On a track with recorded audio items, the plugin pitch-detects the rendered playback —
  useful for offline review of a recorded take.

---

## Architecture

### Threading model

| Thread                            | Does what                                                                                                                                                              |
| --------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Audio (`processBlock`)            | Pitch detection; writes `currentFrequency` / `currentMidiNote` / `currentPlaybackTime` atomics. `currentPlaybackTime` (from `getPlayHead()`) is the editor's playback hot path, not just a fallback. |
| Message (VBlank `onFrame`, paint) | All REAPER API calls (gated to ~20 Hz), chart loading, `cachedPlayTime`, repaint.                                                                                       |

REAPER API functions must **never** be called from `processBlock`.

### Key data flow

```
Reaper host
  └─ setIHostApplication()  →  reaperApi.load()    [one-time handover]

Editor onFrame() — VBlankAttachment, ≈ display refresh
  ├─ [≤20 Hz gate] GetProjectStateChangeCount()  →  if changed → readChartFromReaper()
  ├─ [≤20 Hz gate] GetPlayState()                →  playing vs stopped
  ├─ [≤20 Hz gate, stopped] GetCursorPosition()  →  cachedPlayTime
  ├─ [every frame, playing] currentPlaybackTime atomic → cachedPlayTime
  └─ repaint() (idle-gated)                      →  paint() reads cachedPlayTime + chartNotes
```

### REAPER API access

`PluginProcessor` holds a `ReaperApi` struct (`src/reaper_api.h`) of typed function
pointers loaded from `IReaperHostApplication::getReaperApi()`; the inner
`Vst3Ext : VST3ClientExtensions` overrides `setIHostApplication` to do the load.
`reaperApi.canEdit()` additionally checks the write-side pointers (note editing), and
`canExportTake()` the take-export set on top of that. The processor also **retains the
host interface** (`reaperHost`, add-ref'd at handover, released in the destructor) so
the plugin's own `MediaTrack*` can be queried at point of use via `getReaperParent(1)`
— a handover-time capture comes back null because the FX isn't attached to its track
yet during instantiation (found 2026-07-20).

### Chart data

`std::vector<NoteInfo>` owned by `PluginProcessor`, **message-thread only**, sorted by
`startTime`. `NoteInfo`: start/end (seconds, absolute project time), pitch, lyric (UTF-8),
`isTalkie` (`#`/`^` suffix) / `isHidden` (`$` suffix) flags, `part`
(`Main`/`Harm2`/`Harm3`), `thickScale`, and a source back-reference
(`sourceTake`/`sourceNoteIndex`/`sourceTrack`/`sourceItem`, REAPER path only) used by the
note-pitch editor's `MIDI_SetNote` write-back.

- Lyrics: MIDI text events of **type 1 or type 5** matched to the nearest note-on within
  50 ms. Both types are required — type 5 is the MIDI "lyric" event this project's own
  charts use, while real RB exports (RBN/Magma convention) write lyrics as type-1 "text"
  events; accepting only type 5 loaded imported charts with every note and no lyrics.
  Because animation/venue cues (`[idle]`, `[mellow]`, `[play]`) share those event types,
  **bracketed text is skipped** (`isBracketedMarkerText` in `LyricText.h`) — the bracket is
  the only thing separating a cue from a lyric. The talkie/hidden flags classify the
  lyric's whole trailing suffix run (`LyricText.h`), so `"word#$"` is both.
- Marker pitches (named constants in `PluginProcessor.h`): `kRangeShiftPitch` 0 =
  Range Shift section boundary, `kPhraseMarkerPitch` 105 = phrase, `kOverdrivePitch`
  116 = overdrive, ≥ `kFirstReservedPitch` 96 = reserved (all excluded from `chartNotes`).
  `kLyricShiftPitch` 1 = RB3 lyric shift is filtered too, but unlike the others it is
  *dropped* rather than collected — nothing consumes it yet. It has a named constant anyway
  because charting it dragged the highway floor to C-1
  (`_upcoming/CHART_REVIEW_FINDINGS.md` §2), and because the pitch-edit drag floor is
  expressed as `kLyricShiftPitch + 1`.
- Pitch range (`chartMinPitch`/`chartMaxPitch`) = loaded note min/max ± 2 semitones,
  clamped into the singable band (`rbhighway::kVocalLow/High` = 36–84). This pair is the
  **octave-wrap scoring range only** — the highway draws the section min/max through
  `rbhighway::visibleRange` instead, so changing these numbers moves scoring, not the
  picture. The minimum-span rule is deliberately not applied here (widening the wrap range
  would send a sung pitch to a different octave than the one drawn).
- `songEndTime` from an `[end]` text event on an EVENTS track, else the latest note end.
- Muted MIDI notes are **deliberately not treated specially** — they draw and score like
  any other note (CODE_REVIEW 1.8 decision, 2026-07-17: not a common Rock Band authoring
  case; revisit if a user asks — options were skip, or render dimmed). Documented in README.
- Both loaders share `clearChart()` + `finalizeChart()` (`ChartLoader.cpp`) for
  everything after "find source + parse" — the REAPER/file paths stay in sync structurally.
- `finalizeChart()` also fills the per-note render cache (`displayLyric`,
  `syllable`/`glue`, `effectiveEnd` = end of the same-part raw-`"+"` chain) and the 12
  pitch-class octave-wrap candidate lists, and bumps `chartVersion` — the editor's
  lyric-width and phrase-layout caches key off `getChartVersion()` for invalidation
  (2026-07 PERF series). Paint never allocates strings or rescans the note list.

### Vocal mode (Solo / Harmonies)

`vocalMode` atomic on the processor; the editor's Vocal Mode buttons set it and call
`reloadChart()` (re-reads REAPER if connected, else the remembered `lastMidiFile`).

- **Solo:** first track containing `PART VOCALS`, all notes `Main`.
- **Harmonies:** `HARM1`/`HARM2`/`HARM3` combined into one list. Range Shift sections come
  from HARM1 only; phrase markers are collected per part (HARM1 → `chartPhrases`,
  HARM2 → `chartHarm2Phrases`, HARM3 → none).
- Post-passes (both in `ChartLoader.cpp`): `applyHarmonyOverlapThickness` — a lower part
  in sustained unison with a higher-priority part gets a thicker bar (Harm2 ×1.3,
  Harm3 ×1.6); `dedupHarmonyLyrics` — a HARM3 lyric duplicating HARM2's within 30 ms is
  marked `isHidden`.

---

## Implemented features (summary)

Details live in the code; this is the map of what exists and its non-obvious decisions.

- **Pitch detection** (`PitchDetection.cpp` + `NsdfPeak.cpp` + the front end in
  `processBlock`): MPM fixed as the algorithm (processor default `pitchMethod = 2`;
  Autocorrelation and YIN remain code-level fallbacks). Pipeline: high-pass at the
  band floor + 2× 6 kHz low-pass → integer decimation to a fixed **~24 kHz** internal
  rate (`detectRate` — all window/lag maths is host-rate-independent by construction,
  so high sample rates need no buffer scaling) → 768-sample window (~32 ms) analysed
  every 256-sample hop (**~94 estimates/s**) → RMS gate (`gateThreshold`) → FFT NSDF
  (`juce::dsp::FFT`, 2048-point) → `rbpitch::findNsdfPeak` (zero-lag-lobe skip, key
  maximum, boundary-lag rejection, clarity gate at NSDF peak < 0.80 = unvoiced,
  parabolic interpolation) → 3-tap median → 24-hop (~260 ms) hold decay.
  - Each `HopSample` is timestamped at its window's **centre**, not at the sample that
    triggered it — see the 2026-08-08 backlog entry in CLAUDE.local.md for the measured
    latency figures and for the mid-phrase asymmetry that timestamping deliberately does
    *not* correct.
  - The searched band is **60–1200 Hz** (`rbpitch::kMinFreqHz`/`kMaxFreqHz`), sized
    around the RB vocal range MIDI 36–84 with ~6% headroom — boundary rejection
    discards a peak landing *on* a bound, so a tight bound rejects real content.
  - `NsdfPeak.h/.cpp` is pure and JUCE-free so the peak-picking decisions are unit
    tested on hand-built NSDF tables (`tests/NsdfPeakTests.cpp`), including hidden
    `[.sweep]` accuracy sweeps across analysis rates and window lengths.
  - **Octave guard** (`octaveGuard` 0=Off 1=Warn 2=Warn+Correct, default Off): when on,
    `findNsdfPeak` additionally looks for a peak near `bestTau / 2` (±6% window, required
    to sit inside the walked region and to be a genuine local maximum, parabolically
    interpolated like the main peak) and reports `octaveRatio` / `octaveTau`. A rival at
    or above `kOctaveRivalRatio` (0.85) that persists `kOctaveRunHops` (6) consecutive
    voiced hops is believed; in Correct mode `detectPitchMPM` then returns the rival lag,
    **before** the median filter, so scoring, the trail and the take export all agree.
    Off passes `octaveRivalRatio = 0`, which skips the search — the detector is then
    bit-for-bit its pre-guard self. Rationale and the false-positive findings:
    `_upcoming/OCTAVE_CONFIDENCE.md`.
  - Design rationale: `_old/ENGINE_V2.md` (original v2),
    `_upcoming/PITCH_DETECTION_FINDINGS.md` (2026-08 range fix + 24 kHz move, with
    the measurements behind both).
- **Chart loading**: REAPER path (auto-reload on `GetProjectStateChangeCount` change) and
  `.mid` fallback, per the host-support table above.
- **Highway** (`drawHighway`): horizontal scroll (`pixPerSec`, default 200 px/s), playhead
  at 30 % from left, per-part z-order Harm3 → Harm2 → Main (Main cyan / Harm2 amber /
  Harm3 brown), `+`-lyric bend connectors, talkie notes as translucent columns, inline
  lyric labels Main-only, Range-Shift section boundaries with direction arrows, phrase
  separator lines, white (or amber-when-octave-wrapped) live-pitch arrowhead — drawn at
  the fractional pitch (glides, no semitone stepping) and snapped onto the closest
  active note's lane when within the Pitch Tolerance (b2 visual snap; the idle-repaint
  gate tracks `currentMidiFloat` so sub-semitone movement repaints, and the heard sample's
  `octaveRatio` for the same reason — doubt moves independently of pitch), octave-guard
  doubt marking when the guard is on (`kOctaveDoubtColour` red arrowhead — outranking the
  octave-wrapped amber — plus red trail segments built as a second `juce::Path` in the
  same walk, sharing the boundary vertex so the contour stays unbroken; the stats chip
  shows `OCT? nn` confirmed / dim `oct nn` for a rival seen but not believed, in a column
  taken only while the guard is on so Off keeps the pre-feature layout), and note-hit
  highlighting — `updateNoteHits()` (called from `onFrame()`) accumulates per-note
  sung-within-tolerance spans (`noteHitSpans`; accumulation only while actually
  playing on the REAPER path, since the stopped-state time follows the edit cursor;
  persist while stopped for review, cleared when the next pass starts via
  `hitResetPending` — armed by any time discontinuity *and* by every stopped→playing
  transition so the reset is independent of REAPER's cursor-on-stop preference,
  fired on the next accumulating frame; while playing the gold is clipped at the
  playhead line, since in playback mode the scoring window runs a calibration ahead of
  the listener's ears and a note would otherwise light up while still approaching the
  line (inert with a live mic, lifted when stopped — same rule as the trail);
  chart reloads remap spans by note identity
  via `remapNoteHitSpans()` — tolerance-matched, since REAPER take rewrites drift
  the PPQ→time recompute by ULPs, and robust to transient empty reads — so only
  actually-edited notes lose their gold) and the
  note loop fills them gold (`kNoteHitColour`; a white overlay was too faint on
  cyan — see EditorVisuals.h) (b3; the coverage ratio is the future phrase-scoring
  input), and a pitch trail (b4) — `updateNoteHits()` also appends the sung
  fractional pitch per frame (`pitchTrail`, gap markers break silences, same
  new-pass lifecycle as the hit spans) and drawHighway strokes it as a faint white
  contour (`kPitchTrailColour`) behind the arrowhead, raw/unsnapped, octave-wrapped
  per-sample at draw. Trail samples are stored on the raw project timeline and shifted by
  `detectionOffsetSec()` at draw (see the three-timelines gotcha); while playing the
  contour stops at the sample being heard (`heardDetection()`), so it never runs ahead of
  the arrowhead — the clip lifts on stop, since a cursor snap-back would otherwise hide the
  whole pass. Pitch range
  animates per Range-Shift section (smoothstep over the marker length); the per-frame
  geometry is cached (`lastHighwayArea`/`lastFMinP`/…) so mouse hit-testing matches the
  screen exactly. The per-section target range comes from `rbhighway::visibleRange`
  (`HighwayRange.h`) — pad ± 5, clamp into 36–84, minimum 12-semitone span.
- **Stats panel** (`drawStats`): time `MM:SS.ms / end`, live note name + MIDI + Hz,
  traffic light vs the closest active note of any part, scored in cents from
  `currentMidiFloat` (within ±`pitchTolCents` = GREAT, within 2× = CLOSE, else MISS;
  talkie = presence detection; silence during an active note = MISS). Octave wrapping
  wraps the rounded note and carries the sub-semitone offset across.
- **Lyric strips** (`drawLyricStrip`): bottom = Main, top = HARM2+HARM3 (Harmonies only).
  Scrolling mode (word pills, sub-pixel GlyphArrangement positioning) or Static mode
  (current phrase + dim next-phrase preview; bottom strip uses HARM1's phrase grid, top
  uses HARM2's). The Lyrics Size setting (`lyricScale`, 50–200%) scales strip text,
  pill metrics and the strip heights — strip/window heights are computed
  (`lyricStripH()`/`windowH()`/`windowHHarm()` in PluginEditor.h replaced the old
  `kLyricH`/`kWindowH`/`kWindowHHarm` constexprs); fonts are rebuilt lazily on scale
  change and the lyric-width cache keys on font height. At 100% rendering is identical
  to pre-feature. **Squash guard** (`squashGuard`, `SquashMode` enum, default Off)
  de-overlaps dense scrolling pills: the scrolling branch is collect → adjust → draw
  passes over a persistent scratch vector (`pillScratch`); Spacing = greedy push-right
  (continuous in t, pills glide back onto their anchors), Auto-shrink = proportional
  glyph scaling via AffineTransform (floor 0.5, then fitted-squash to 0.25 + ellipsis;
  no per-frame Font construction), Hybrid = cap at 0.8×width then push the rest. Off
  renders byte-identically to pre-feature (`_old/LYRIC_OVERLAP.md`).
- **Status + message bars**: `fps | REAPER priority | renderer | calib | rate` diagnostics
  (the calib field reads `out/in ms`, appending ` live` / ` playback` when the *output*
  offset is non-zero — at 0 ms the mode cannot change anything, so it is left off — and
  ` (stopped)`, since the output offset is not applied while stopped);
  chart status string, overridden by an amber can't-keep-up warning when frames drop.
- **Controls**: bottom bar (Vocal Mode, Lyric Mode, `<<`/Settings/`>>`) + `SettingsPanel`
  CallOutBox (Frame cap 20–60/uncapped default 60; REAPER priority off; Renderer
  CPU/GL-session/GL-persist; Output calib −200..+500 ms step 20 (chart) and Input calib
  −50..+250 ms step 5 (detections — finer, since the analysis chain alone is 5–30 ms);
  **Live-mic used** off — whether the *output* calibration also shifts detected pitch, see
  the calibration gotcha; Note Speed 100–400 px/s; Lyrics
  Size 50–200% — applyLyricScale does a delta-based window resize so a user-resized
  window keeps its height; Squash Guard Off/Spacing/Auto-shrink/Hybrid default Off;
  Octave Wrapping on; Pitch Trail on; Pitch Tolerance ±25/50/75/100 cents default
  ±50; Octave Guard Off/Warn only/Warn + correct default Off; Mic gate; Load MIDI File
  when no REAPER API) + **Allow edits** toggle in the stats band.
- **Settings persistence**: `get`/`setStateInformation` serialise vocalMode, gate,
  octaveGuard, frameInterval, reaperPriority, octaveWrap, pitchTolCents, pitchTrail,
  latencyMs, inputLatencyMs, liveMic, pixPerSec, lyricScale, squashGuard, lyricMode,
  renderer. `octaveGuard` follows the `gateThreshold`/`vocalMode` pattern — no `saved*`
  mirror, because the audio thread reads it — and is clamped to 0..2 on read.
  Exception by design: the `OpenGL - session only` renderer choice is never persisted
  (only `OpenGL - persist` writes `savedRenderer = 2`).
- **Transport**: click-to-seek on the highway, Left/Right arrows & `<<`/`>>` = ∓5 s
  (`SetEditCurPos`), Up/Down = play-rate step 0.1× (0.1–4×, `CSurf_OnPlayRateChange`).
  All no-ops outside REAPER.
- **Note pitch editing** (Allow edits, requires `reaperApi.canEdit()`): drag a bar
  vertically, snapped to semitones; preview mutates the in-memory `NoteInfo` (chart
  reloads are suppressed while dragging); `mouseUp` commits via `MIDI_SetNote` inside an
  undo block + `MarkTrackItemsDirty`, then forces a clean reload (`lastStateCount = -1`).
  Drags clamp to visible lanes ∩ pitch 1..95 so a note can never land on a reserved
  marker pitch. Pitch only — timing/lyrics untouched, so `sourceNoteIndex` stays valid.
- **Save take as MIDI** (`saveCurrentTake()` in PluginEditor.cpp + `src/TakeExport.h/.cpp`,
  requires `reaperApi.canExportTake()`): the **Save take** button next to Allow edits
  (enabled by the poll gate only when stopped with a non-empty trail — stopping is what
  freezes the trail) segments `pitchTrail` via `rbtake::segmentPitchTrail` (pure,
  unit-tested: 120 ms min note, 80 ms same-pitch gap merge, 20 ms spacing when notes
  butt, 0.65 st split hysteresis, clamp 1..95) and writes one MIDI item (vel 96, ch 0,
  PPQ via `MIDI_GetPPQPosFromProjTime`) on a new track `RB Vocal Sing YYYYMMDD HHMM`
  directly below the plugin's track (`reaperHost->getReaperParent(1)` at click time;
  fallback = end of project), all inside one undo block, then forces a clean reload.
  Feedback via a ~4 s amber message-bar flash (`flashMessage`/`flashUntil`) that
  outranks chartStatus *and* the frame-drop warning, and participates in the
  idle-repaint gate so it appears/expires while the transport is idle. Notes are written
  at **raw project time regardless of Calibration** (2026-08-03) — the chart has to line
  up with the audio waveform and stay neutral about the monitoring latency of this rig or
  the target console. Design + findings: `_old/TAKE_EXPORT.md`.

---

## Performance lessons (hard-won — do not relearn)

Full history was in this file's old backlog; the forward-looking plan is now
PERFORMANCE_IMPROVEMENT.md. The conclusions that must survive:

1. **Polling is decoupled from rendering.** The old 30 fps "message-pump ceiling" was
   actually per-tick REAPER API calls; VBlank repaints + a ~20 Hz API gate fixed it.
   Keep any new REAPER call inside the poll gate in `onFrame()`.
2. **Paint self-limits.** Idle-repaint gating (no repaint when nothing changed) + the
   paint-budget duty-cycle cap (`effInterval = max(target, lastPaintSeconds × kBusyFactor)`
   behind the "REAPER priority" toggle) keep the host responsive at large window sizes.
   Don't remove these; retune them if the renderer changes.
3. **DO NOT cache solid fills into a `juce::Image`.** Tried (highway background/grid) and
   reverted: on JUCE's CPU renderer `drawImageAt` is read+write per pixel while
   `fillRect`/`drawHorizontalLine` are write-only — the cache was slower. Image caching
   only pays where per-call CPU cost is high (text/glyphs may qualify — see
   PERFORMANCE_IMPROVEMENT.md §d1 before trying).
4. **JUCE 8 minimal port does not work** (attempted June 2025, reverted). D2D-by-default
   invalidates all the GDI-tuned throttle constants, and JUCE 8's GDI fallback is also
   slower; GL toggle crashed on top of D2D. A future upgrade must rebuild the paint
   pipeline D2D-natively and re-profile — treat as a project, not an upgrade.
5. **OpenGL toggle (shipped, experimental)** uses `attachTo` with component painting, so
   JUCE's GL thread holds the MessageManagerLock around `paint()` — existing chart reads
   stay thread-safe as-is. The *full* decoupled `renderOpenGL()` pivot removes the MM
   lock and therefore **requires an immutable chart snapshot** — skipping that sync would
   crash.
6. **Scrolling lyric blur** was mostly a frame-rate problem (halved by 60 fps). Word
   pills + sub-pixel GlyphArrangement are kept; 80 px/s scroll and 8× outline text were
   tried and reverted (broke highway sync / cost too much).
7. **Profile (and run) Release builds.** Debug's software rasterizer is ~9× slower per
   pixel — a 2315×1280 playing frame measured 32.7 ms in Debug vs **3.6 ms** in Release
   (2026-07-17), and every "paint problem" measured in Debug was mostly that. The
   `RB_PAINT_PROFILE` CMake option (OFF by default, works in both configs) adds
   per-section paint EMAs + drawHighway sub-timers to the status bar and an 'S'-key
   window-size preset cycle; sub-timers showed the background fill dominates and note
   bars/label text are ≤ 2 ms even in Debug — so §d1 (text image cache) and the
   Harmonies scan merge are ruled out (`_old/PERFORMANCE_IMPROVEMENT.md`).

---

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

(from the repo root)

Output: `build/RBVocalSing_artefacts/Debug/VST3/Rock Band Vocal Sing - Reaper.vst3`
(bundle name from `PRODUCT_NAME` in `CMakeLists.txt`).

Install: `scripts\deploy_plugin.bat` (optionally `-Config Debug`, `-NoRestart`) —
builds Release, closes REAPER gracefully (the user answers any save prompt; REAPER
locks the installed DLL), then an elevated stage (UAC prompt) removes **every** copy
of the bundle under `C:\Program Files\Common Files\VST3` and installs the fresh one,
and REAPER restarts unelevated. The remove-all step exists because a stray nested
copy (`VST3\Release\VST3\…`) once shadowed the real install with the same plugin ID
and a stale build was tested against for a day (2026-07-19). Manual alternative:
copy the `.vst3` into `C:\Program Files\Common Files\VST3` and re-scan in REAPER.

---

## Testing

Catch2 unit tests (`RBVocalSingTests` target, `tests/*.cpp`) exercise the processor's
public API directly — pitch detection, chart loading (Solo + Harmonies), settings
persistence — plus the take-export segmenter (pure functions in `src/TakeExport.h`,
tested without any plugin scaffolding) and the highway pitch-axis rules
(`src/HighwayRange.h`, likewise pure — `tests/HighwayRangeTests.cpp`). Piece A is a pluginval black-box smoke script. See TEST_PLAN.md for the
full design rationale.

`tests/DetectionLatencyTests.cpp` is a *measurement* test rather than a pass/fail one:
it drives `processBlock` with synthesized onsets, legato pitch steps and gate closes,
then prints how late each was reported (via Catch2 `WARN`, so the numbers surface on a
passing run). Its bounds are expressed in hops, not milliseconds, because every figure
comes out identical in hops across host rates. Read the numbers before changing anything
in the detection front end — they are how the window-centre anchor and the phrase-edge
vs mid-phrase asymmetry were established.

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --target RBVocalSingTests
& "build/RBVocalSingTests_artefacts/Debug/RBVocalSingTests.exe"   # all pass

cmake --build build --config Debug                                 # plugin still builds
scripts/run_pluginval.ps1                                          # exits 0
```

`scripts\run_tests.bat` chains the first three commands (configure + build + run) in
one step for fast local iteration — plain batch, not PowerShell, so it needs no
`-ExecutionPolicy` flag. A GitHub Actions workflow (`.github/workflows/tests.yml`) runs
the same build+test on every push/PR (separate from `release.yml`, which only builds on
tagged releases).

Any deliberate chart-loader behaviour change must update the fixtures in
`tests/ChartLoaderTests.cpp` in the same commit.

No `[!mayfail]` cases remain. The convention, should it be needed again: tag a test
`[!mayfail]` to assert the fixed/correct behaviour for a known bug — it's allowed to
fail until the bug is fixed elsewhere, then starts passing with no test-file changes;
remove the tag in the same commit as the fix to lock it into CI. (The three historical
cases — CODE_REVIEW 1.2 high-sample-rate pitch, 2.1 mixed suffix flags, 1.4 songEndTime
fallback — were fixed and un-flagged in the 2026-07-17 patch series.)

---

## Dev tools (`_dev_tools/`, gitignored)

Standalone local scripts for inspecting things the plugin can't show from inside itself.
Not part of the build, not shipped, no dependencies beyond a stdlib Python 3.

```
python _dev_tools/midi_vocal_dump.py <file.mid> [--lyrics]
```

**`midi_vocal_dump.py`** — reads a `.mid` the way `ChartLoader.cpp` does and reports why a
chart would load wrong: text events counted **by meta type** (the type-1 vs type-5
distinction the loader turns on) split into lyric-looking vs bracketed, the note histogram
separating singable notes from marker pitches (flagging pitch 1), the pitch range the
highway would render, and the 50 ms lyric match replayed both as the loader behaves today
and as it would with type-1 accepted. Use it whenever an imported chart misbehaves — it
turns "lyrics don't show" into a count. It is also the acceptance check for the two
chart-loader bugs recorded in CLAUDE.local.md (`_upcoming/CHART_REVIEW_FINDINGS.md` §1.5).

Drop `.mid` files into `_dev_tools/` or `_upcoming/` to inspect them; both are gitignored,
so nothing an author sends for review can be committed by accident.

---

## Known gotchas

- **Three timelines exist — decide which one any new feature is on.** Raw project time
  (`cachedPlayTime`, `currentPlaybackTime`, `HopSample::time`, chart `NoteInfo` times);
  the *drawing* timeline `t = cachedPlayTime - drawingOffsetSec()`; and the
  *detection* timeline `cachedPlayTime - detectionOffsetSec()`. The two offsets are
  independent settings and are equal only by coincidence (see the next gotcha); never
  substitute one for the other, and never inline `latencyOffsetMs` in place of either —
  both helpers carry conditions the raw field does not. The rule: **drawing `t`** for chart
  geometry; **detection time** for anything that pairs detected pitch with chart notes;
  **raw** for anything written into the project or reported as a project position
  (`SetEditCurPos`, `MIDI_SetNote`, the take export, the stats time readout). Audio-thread
  data arrives raw and must be shifted where it is consumed on the message thread. Check a
  new feature against the existing consumers: chart geometry in
  `drawHighway`/`drawLyricStrip` = drawing `t`; `updateNoteHits()` scoring = detection
  time; the arrowhead, its visual-snap scan and the whole stats band (pitch, note name, Hz,
  traffic light) = drawing `t`, with the *pitch* itself fetched through `heardDetection()`
  so it is the sample belonging at `t` rather than the newest analysed; `hitTestNote` =
  drawing `t`; click-to-seek = drawing `t` as input producing a raw project time (screen x
  maps to that note's own time, satisfying both halves of the rule); `<<`/`>>` and
  arrow-key seeks = raw (relative ±5 s on the real transport); `pitchTrail` = stored raw,
  shifted by `detectionOffsetSec()` in `drawHighway`; take export and the stats time
  readout = raw. Bit us 2026-08-03: the trail stored raw hop times but was drawn against
  `t`, so at +200 ms the contour drew 200 ms right of the playhead and every scoring
  sub-interval fell outside `[t0, t]` — and the first fix over-corrected by storing
  calibrated times, which leaked the offset into the exported MIDI.
- **Calibration is two numbers, because it compensates two different physical delays.**
  Split 2026-08-08 (`latencyOffsetMs` + `inputOffsetMs`); before that one slider did both
  jobs and could not satisfy them at once.
  - **Output calib** (`latencyOffsetMs`) — how late the song reaches the listener's ears.
    Shifts the **chart**, via `drawingOffsetSec()`.
  - **Input calib** (`inputOffsetMs`) — how late a detection is *filed*: mic/interface
    input latency plus the analysis chain. Shifts **detections**, via
    `detectionOffsetSec()`. Non-obvious and the reason the split was needed: the analysis
    chain alone contributes 5–30 ms that has no counterpart on the output side, so forcing
    the two equal means either the chart drifts off the song or the trail drifts off the
    chart. Users hit this by dragging the *chart* early to meet a late trail.
  - **Live-mic used** (`liveMicInput`, default off) still cancels the *human* round trip:
    a singer hears late and therefore sings late, so the output offset is added on top of
    the input offset for detection. A machine source — vocal stem, ReaSynth, a recorded
    take — reaches `processBlock` at true project time and never made that trip. Not
    auto-detected on purpose: a mic and a stem are the same float buffer, and
    `I_RECARM`/`I_RECMON` false-positive on the still-armed track you just recorded on.
  - `detectionOffsetSec()` = `inputOffsetMs + (liveMic ? latencyOffsetMs : 0)`, and
    `drawingOffsetSec()` = `latencyOffsetMs` **while playing, 0 while stopped** — both in
    PluginEditor.h, each the single point of truth for its timeline. Both offsets at 0
    reproduces pre-split behaviour, which is what a pre-split saved session loads as
    (`inputLatencyMs` absent → 0).
  - The take export is immune to both (raw project time in every mode).
- **The output calibration must not apply while the transport is stopped.** While stopped,
  `cachedPlayTime` is the edit cursor — a true project position with no audio in flight, so
  there is no monitoring latency to cancel. Applying it anyway drew the chart
  `latencyOffsetMs` away from the cursor, which made the natural verification move (park
  the cursor on a note start, check the note is on the playhead line) report a sync error
  that did not exist; a user found the plugin this way at −60 ms, where a 1/32 note was
  displaced by roughly its own length (found 2026-08-08). `drawingOffsetSec()` gates on
  `lastIsPlaying`, and only on the REAPER path — the file fallback's host playhead only
  advances during playback, so it counts as always playing. Two consequences worth knowing:
  the chart visibly shifts by the calibration on every play/stop transition (so
  `lastIsPlaying` is in the idle-repaint gate — it is a *paint input* now, and a
  stop-in-place changes nothing else the gate watches), and trail-vs-chart alignment is
  unaffected in both states because both are mapped through the same `t`.
- **The octave rival is one-directional evidence, and weaker than it looks.** A peak near
  `bestTau / 2` is worth something because a signal periodic at τ is *not* periodic at τ/2
  — but only when it is cleanly periodic and fundamental-dominant. Two common cases break
  that and produce a rival with the pitch entirely correct: a pitch glide (the 768-sample
  window spans two notes, so consecutive periods differ and nsdf(τ) is damped) and a vowel
  whose first formant sits near H2 (the waveform genuinely has a strong quasi-period at
  τ/2). Hence `kOctaveRunHops` — the rival must outlast a transition, which contaminates
  about `kPitchWindow / kPitchHop` = 3 hops. Do **not** "complete the symmetry" by adding a
  2τ check for octave-high errors: a peak at 2τ always exists for a periodic signal, so it
  carries no information at all. Found the hard way — Warn mode lit up glides and
  mid-sustain moments on a real vocal before the persistence filter existed (2026-08-27).
- `DEF_CLASS_IID(IReaperHostApplication)` must appear in exactly **one** .cpp file.
  It is in `PluginProcessor.cpp`. Do not add it to any header.
- `#define NOMINMAX` must precede any Windows headers to avoid `min`/`max` macro conflicts
  with `std::min` / `std::max`.
- `setIHostApplication` may never be called (other DAWs). Always guard with
  `reaperApi.isLoaded()` before calling any function pointer.
- All REAPER API calls are message-thread only. Never call them from `processBlock`.
- `MIDI_GetProjTimeFromPPQPos` converts PPQ to absolute project seconds correctly across
  tempo changes. Never compute time from PPQ with a fixed tempo.
- `msglen` in `MIDI_GetTextSysexEvt` is in/out: set it to buffer capacity before the call,
  then it contains the actual byte count after. Always null-terminate manually.
- `CStringA` in `reaper_vst3_interfaces.h` is `typedef const char8* CStringA` from the
  bundled VST3 SDK — plain `const char*`, no ATL required.
- Never call height-only `g.setFont (12.0f)` in paint code — it keeps the typeface
  *style* of whatever font is currently in the Graphics context, so text silently
  flips bold/plain depending on which section drew first (bit us 2026-07-18: the top
  lyric strip's bold font leaked into the highway labels in Scrolling mode only).
  Always pass a full `juce::Font (size, style)`.
- Keep **user-visible string literals ASCII-only**. MSVC compiles this project's source
  in the system ANSI codepage (no `/utf-8` flag) while JUCE decodes `const char*`
  literals as UTF-8 — so an em dash or `→` in a literal renders as mojibake (bit us
  2026-07-20: the take-export flash message). Comments can use any Unicode; strings the
  user sees cannot.
- The Settings CallOutBox must stay **editor-parented** (`this`). A `nullptr`/desktop
  parent was tried twice (2026-07-18) to stop the panel clipping at small window
  heights, and reverted both times: (1) a desktop window doesn't inherit the host's
  editor scale transform, so its text renders visibly smaller; (2) compensating with
  `setTransform` — even the residual `editorScale / display->scale` with pre-divided
  target/fit rects — still rendered wrong on a mixed-DPI two-monitor setup (worked on
  the 100% monitor, glitched on the high-DPI one). Tall panels scroll inside
  `SettingsScroller` (a Viewport wrapper) instead.
