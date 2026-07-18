# vocal-rb-sing — Project Reference

A JUCE 7 VST3 plugin that runs inside REAPER and helps author/test Rock Band vocal charts.
It reads mic input for pitch detection and displays a scrolling note highway synced to the
REAPER transport, letting the user sing along and see how well they match the chart.

**Status:** feature-complete PoC plus post-PoC features (Harmonies mode, Settings popup with
persisted settings, transport navigation, write-back note-pitch editing). Builds and runs
inside REAPER. User-facing documentation lives in **README.md** (installation, setup,
usage) — keep it in sync when behaviour changes.

## Reference documents (read before starting related tasks)

Local-only working docs (both directories are gitignored): `_upcoming/` holds planned
work, `_old/` holds completed phases kept for their design rationale.

| Document | Contents |
| --- | --- |
| [_old/CODE_REVIEW.md](_old/CODE_REVIEW.md) | 2026-07 full review — §1–§3 all fixed (2026-07-17 patch series); §4 recorded as non-action, §5 informational |
| [_old/PERFORMANCE_IMPROVEMENT.md](_old/PERFORMANCE_IMPROVEMENT.md) | 2026-07 paint phase — closed: instrumentation + §c wins shipped (PERF-1…10); Release measurement showed Debug-build profiling was the real problem (~9× slower); §d not needed |
| [_old/PERFORMANCE_VERIFICATION.md](_old/PERFORMANCE_VERIFICATION.md) | Manual REAPER checklist per PERF patch + a standing record of one unreproduced lyric-duplication glitch |
| [_upcoming/PITCH_DETECTION_IMPROVEMENT.md](_upcoming/PITCH_DETECTION_IMPROVEMENT.md) | Pitch-engine assessment + karaoke-style assist features (cents scoring, snap, hit highlighting) |
| [_upcoming/FONT_SCALING.md](_upcoming/FONT_SCALING.md) | **Implemented 2026-07-18** (5-patch series) — Lyrics Size setting (50–200%, scales text + strip heights). Doc kept for design rationale; the stretch goal (global UI text scale) is still open. Candidate to move to `_old/` |
| [_upcoming/LYRIC_OVERLAP.md](_upcoming/LYRIC_OVERLAP.md) | **Implemented 2026-07-18** (4-patch series) — Squash Guard dropdown (Off / Spacing / Auto-shrink / Hybrid). Doc kept for design rationale + implementation decisions. Candidate to move to `_old/` |
| [_old/TEST_PLAN.md](_old/TEST_PLAN.md) | Implemented test setup: pluginval smoke script + Catch2 unit-test target (design rationale; see the Testing section below for commands) |

Suggested order: PITCH_DETECTION next (TEST_PLAN, CODE_REVIEW, PERFORMANCE,
FONT_SCALING and LYRIC_OVERLAP are done).

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
`reaperApi.canEdit()` additionally checks the write-side pointers (note editing).

### Chart data

`std::vector<NoteInfo>` owned by `PluginProcessor`, **message-thread only**, sorted by
`startTime`. `NoteInfo`: start/end (seconds, absolute project time), pitch, lyric (UTF-8),
`isTalkie` (`#`/`^` suffix) / `isHidden` (`$` suffix) flags, `part`
(`Main`/`Harm2`/`Harm3`), `thickScale`, and a source back-reference
(`sourceTake`/`sourceNoteIndex`/`sourceTrack`/`sourceItem`, REAPER path only) used by the
note-pitch editor's `MIDI_SetNote` write-back.

- Lyrics: type-5 MIDI text events matched to the nearest note-on within 50 ms. The
  talkie/hidden flags classify the lyric's whole trailing suffix run (`LyricText.h`),
  so `"word#$"` is both.
- Marker pitches (named constants in `PluginProcessor.h`): `kRangeShiftPitch` 0 =
  Range Shift section boundary, `kPhraseMarkerPitch` 105 = phrase, `kOverdrivePitch`
  116 = overdrive, ≥ `kFirstReservedPitch` 96 = reserved (all excluded from `chartNotes`).
- Pitch range (`chartMinPitch`/`chartMaxPitch`) = loaded note min/max ± 2 semitones.
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

- **Pitch detection** (`PitchDetection.cpp`): MPM fixed as the algorithm (`pitchMethod = 2`
  set in the editor constructor; the runtime selector UI was removed — Autocorrelation and
  YIN remain as code-level fallbacks). 2048-sample accumulation, RMS gate
  (`gateThreshold`), 6-cycle hold decay. See PITCH_DETECTION_IMPROVEMENT.md before
  changing.
- **Chart loading**: REAPER path (auto-reload on `GetProjectStateChangeCount` change) and
  `.mid` fallback, per the host-support table above.
- **Highway** (`drawHighway`): horizontal scroll (`pixPerSec`, default 200 px/s), playhead
  at 30 % from left, per-part z-order Harm3 → Harm2 → Main (Main cyan / Harm2 amber /
  Harm3 brown), `+`-lyric bend connectors, talkie notes as translucent columns, inline
  lyric labels Main-only, Range-Shift section boundaries with direction arrows, phrase
  separator lines, white (or amber-when-octave-wrapped) live-pitch arrowhead. Pitch range
  animates per Range-Shift section (smoothstep over the marker length); the per-frame
  geometry is cached (`lastHighwayArea`/`lastFMinP`/…) so mouse hit-testing matches the
  screen exactly.
- **Stats panel** (`drawStats`): time `MM:SS.ms / end`, live note name + MIDI + Hz,
  traffic light vs the closest active note of any part (exact = GREAT, ±1–2 semitones =
  CLOSE, else MISS; talkie = presence detection; silence during an active note = MISS).
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
  renders byte-identically to pre-feature (`_upcoming/LYRIC_OVERLAP.md`).
- **Status + message bars**: `fps | REAPER priority | renderer | calib | rate` diagnostics;
  chart status string, overridden by an amber can't-keep-up warning when frames drop.
- **Controls**: bottom bar (Vocal Mode, Lyric Mode, `<<`/Settings/`>>`) + `SettingsPanel`
  CallOutBox (Frame cap 20–60/uncapped default 60; REAPER priority off; Renderer
  CPU/GL-session/GL-persist; Calibration −200..+500 ms; Note Speed 100–400 px/s; Lyrics
  Size 50–200% — applyLyricScale does a delta-based window resize so a user-resized
  window keeps its height; Squash Guard Off/Spacing/Auto-shrink/Hybrid default Off;
  Octave Wrapping on; Mic gate; Load MIDI File when no REAPER API) + **Allow edits**
  toggle in the stats band.
- **Settings persistence**: `get`/`setStateInformation` serialise vocalMode, gate,
  frameInterval, reaperPriority, octaveWrap, latencyMs, pixPerSec, lyricScale,
  squashGuard, lyricMode, renderer.
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

## Backlog

- **Tests done** — see the Testing section above.
- **CODE_REVIEW fixes done** (2026-07-17, 17-patch series) — §1–§3 all fixed
  (`_old/CODE_REVIEW.md`; §4 recorded as non-action, §5 informational). The one
  behaviour decision: muted MIDI notes stay charted (1.8, documented in README;
  revisit on user feedback).
- **PERFORMANCE phase done** (2026-07-17, PERF-1…PERF-10 series) — profiling
  instrumentation + windowing/render-cache wins shipped; phase closed after the
  Release-build measurement (perf lesson 7 above; `_old/PERFORMANCE_IMPROVEMENT.md`).
- **FONT_SCALING done** (2026-07-18, 5-patch series) — Lyrics Size setting shipped;
  only the stretch goal (global UI text scale for stats/status/settings text) remains
  open in `_upcoming/FONT_SCALING.md`.
- **Squash guard done** (2026-07-18, 4-patch series) — Off / Spacing / Auto-shrink /
  Hybrid dropdown shipped, default Off; Auto-shrink uses proportional glyph scaling
  (`_upcoming/LYRIC_OVERLAP.md` records the design + decisions).
- **Settings popup clips at the default window height** — the panel (434 px since the
  Lyrics Size + Squash Guard rows) exceeds the default Solo window (419 px); chosen
  fix (on hold, planned): launch the CallOutBox with a `nullptr` parent +
  `localAreaToGlobal` so it floats on the desktop instead of clipping to the editor.
- **Cents-level accuracy + assist features** — fractional pitch, cents traffic light,
  visual snap, note-hit highlighting; see PITCH_DETECTION_IMPROVEMENT.md.
- **Phrase-level scoring feedback** — did the user hit all notes in a phrase? (builds on
  note-hit tracking above).
- **Note timing/length editing** — natural next step after pitch editing, but timing
  edits reorder MIDI so `sourceNoteIndex` can go stale mid-edit; needs re-resolution
  after each commit.
- **Track identity by GUID** — name matching breaks on rename; `GetTrackGUID` is stable.
- **Play/stop/record indicator** — `GetPlayState` bitmask (1=play 2=pause 4=record) is
  already polled; show it in the UI.
- **Out of scope:** per-track lane separation (harmony parts share one pitch axis by
  design).

---

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

(from the repo root)

Output: `build/RBVocalSing_artefacts/Debug/VST3/Rock Band Vocal Sing - Reaper.vst3`
(bundle name from `PRODUCT_NAME` in `CMakeLists.txt`).

Install: copy the `.vst3` into `C:\Program Files\Common Files\VST3` and re-scan in REAPER.

---

## Testing

Catch2 unit tests (`RBVocalSingTests` target, `tests/*.cpp`) exercise the processor's
public API directly — pitch detection, chart loading (Solo + Harmonies), and settings
persistence. Piece A is a pluginval black-box smoke script. See TEST_PLAN.md for the
full design rationale.

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

## Known gotchas

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
