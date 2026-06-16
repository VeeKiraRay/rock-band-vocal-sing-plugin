# vocal-rb-sing — Project Reference

A JUCE 7 VST3 plugin that runs inside Reaper and helps author/test Rock Band vocal charts.
It reads mic input for pitch detection and displays a scrolling note highway synced to the
Reaper transport, letting the user sing along and see how well they match the chart.

---

## Status

**PoC complete, plus several post-PoC features.** All three original phases are implemented and
the plugin builds and runs inside Reaper. Since the PoC it has gained Harmonies mode, a Settings
popup with persisted UI settings, transport navigation (click-to-seek, arrow-key seek/play-rate),
and write-back note-pitch editing (the **Allow edits** toggle). See the backlog section for what
is still to do.

---

## Host support (REAPER vs other DAWs)

The plugin loads its chart from one of two sources, chosen automatically by whether REAPER's
extension API was handed over (`reaperApi.isLoaded()`). The two paths build the same chart
data (`chartNotes` + phrases + sections) — only the source differs.

|                         | Inside REAPER (primary)                                             | Other DAWs / standalone (fallback)                          |
| ----------------------- | ------------------------------------------------------------------- | ----------------------------------------------------------- |
| Chart source            | Live project tracks read via the REAPER API (`readChartFromReaper`) | A `.mid` file the user picks (`loadMidiFile`)               |
| Track selection         | Auto: PART VOCALS (Solo) or HARM1/2/3 (Harmonies), by track name    | Same names, matched against `.mid` track-name meta events   |
| Live edits              | Auto-reloads on any project change (edit/undo/tempo)                | Static — re-pick the file (or toggle Vocal Mode) to refresh |
| "Load MIDI File" button | **Hidden** (not needed)                                             | **Shown** (in the Settings popup)                           |
| Transport time          | REAPER play/edit cursor (API)                                       | Host playhead via `getPlayHead()`                           |
| Mic pitch detection     | identical                                                           | identical                                                   |

**Why the file-load path exists.** It is _not_ dead code. Historically it was also used
inside REAPER (before auto-track-detection existed) — that REAPER-manual use is now obsolete,
but the path remains as the genuine fallback for non-REAPER hosts and is kept in sync with
the REAPER path (it handles Solo + Harmonies, dedup, and overlap thickness identically).

**Open question (gather feedback):** is there a real use case for running outside REAPER? The
non-REAPER mode works but is degraded (static chart, no live-edit follow). If nobody needs
it, the plugin could become REAPER-only — deleting the button, `openMidiFile`/`loadMidiFile`,
the `juce_audio_formats` dependency, and the `apiReady` branching (~130 lines). Left in place
for now pending feedback.

> README note: this table is the basis for a user-facing "Using inside REAPER vs other DAWs"
> section in the (not-yet-started) README.md.

---

## File layout

```
rock-band-vst/vocal-rb-sing/
  CMakeLists.txt                  — JUCE 7.0.12 via CPM, VST3 only
  cmake/CPM.cmake                 — package manager (copied from drum-rb-play)
  src/
    PluginProcessor.h             — RBVocalSingAudioProcessor decl + chart data types
    PluginProcessor.cpp           — core: lifecycle, processBlock, Reaper API handover,
                                    DEF_CLASS_IID (must stay in this one TU)
    PitchDetection.cpp            — detectPitch + Autocorrelation/YIN/MPM (processor members)
    ChartLoader.cpp               — readChartFromReaper / loadMidiFile / parseChartNotes +
                                    MIDI-text decode, pitch sections, harmony post-passes
    PluginEditor.h                — RBVocalSingAudioProcessorEditor decl
    PluginEditor.cpp              — editor lifecycle, VBlank frame loop, layout, paint dispatch,
                                    SettingsPanel popup, transport keys, note-pitch edit drag
    PluginEditorHighway.cpp       — drawHighway + drawStats + drawStatusBar + drawMessageBar
    PluginEditorLyrics.cpp        — drawLyricStrip (scrolling + static)
    EditorVisuals.h               — shared paint colours + lyric-suffix helpers (inline;
                                    included by the three editor .cpp files)
    reaper_api.h                  — IReaperHostApplication interface + ReaperApi struct
    extern/
      reaper_vst3_interfaces.h    — Cockos IReaperHostApplication (copied from JUCE examples)
  build/                          — cmake build output (gitignored)
  development_plan.md             — original three-phase plan (historical)
```

The processor (`RBVocalSingAudioProcessor`) and editor
(`RBVocalSingAudioProcessorEditor`) are each one class declared in their header but
implemented across several `.cpp` files (standard JUCE multi-TU split, done purely for
readability). When adding a member function, put its body in the file that owns that
concern and keep the declaration in the header.

---

## Input routing (pitch detection source)

The plugin pitch-detects whatever audio REAPER delivers into its input bus (`buffer` channel 0
in `processBlock`). The chart (scrolling highway) is read separately via the REAPER API from
PART VOCALS / HARM tracks — it is independent of which track the plugin lives on.

Practical consequences for users:

- **Record arm is not enough.** The track also needs _input monitor_ enabled (the speaker icon
  in REAPER). Record arm alone does not route mic audio to the plugin's processBlock; monitoring
  does. Record status (whether ⏺ is running) is irrelevant to pitch detection.
- **Plugin on a MIDI / synth track (e.g. with ReaSynth).** The FX chain order determines the
  input: placed _before_ the synth the plugin receives the track's audio input (mic, or silence
  if not monitoring); placed _after_ it receives the synth's audio output and pitch-detects that
  instead.
- **Plugin on an audio track with recorded items.** The plugin pitch-detects those rendered
  samples. Useful for offline review: play back a recorded vocal take and see how it matched
  the chart without singing live.

> **README TODO:** when a user-facing README is written, these three scenarios should become a
> "How to set up the plugin" section with step-by-step instructions for the primary use case
> (live mic monitoring) and call-outs for the other two.

---

## Architecture

### Threading model

| Thread                            | Does what                                                                                                                                                                                                                                |
| --------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Audio (processBlock)              | Reads mic samples, runs pitch detection, writes `currentFrequency` / `currentMidiNote` / `currentPlaybackTime` atomics. Writes `currentPlaybackTime` from `getPlayHead()` — this is the editor's playback hot path, not just a fallback. |
| Message (VBlank `onFrame`, paint) | Calls Reaper API (gated to ~20 Hz). Reads Reaper state count + play state + stopped-cursor. Reads note cache. Writes `cachedPlayTime`. All chart loading happens here.                                                                   |

Reaper API functions must **never** be called from `processBlock`. The editor's VBlank-driven
`onFrame()` handles everything Reaper-related, gating API calls to ~20 Hz so the render rate
(≈ display refresh) can run free. See the "Frame rate — RESOLVED" backlog entry.

### Key data flow

```
Reaper host
  └─ setIHostApplication()  →  reaperApi.load()    [one-time handover]

Editor onFrame() — VBlankAttachment, ≈ display refresh
  ├─ [≤20 Hz gate] GetProjectStateChangeCount()  →  if changed → readChartFromReaper()
  ├─ [≤20 Hz gate] GetPlayState()                →  playing vs stopped
  ├─ [≤20 Hz gate, stopped] GetCursorPosition()  →  cachedPlayTime
  ├─ [every frame, playing] currentPlaybackTime atomic → cachedPlayTime
  └─ repaint()                                    →  paint() reads cachedPlayTime + chartNotes
```

### Reaper API access

`PluginProcessor` holds a `ReaperApi` struct (`src/reaper_api.h`) with typed function
pointers loaded from `IReaperHostApplication::getReaperApi()`. The inner struct
`Vst3Ext : VST3ClientExtensions` overrides `setIHostApplication` to do the load.

`DEF_CLASS_IID(IReaperHostApplication)` is in `PluginProcessor.cpp` — exactly one TU.

`CStringA` in `reaper_vst3_interfaces.h` is `typedef const char8* CStringA` from the
bundled VST3 SDK (`pluginterfaces/base/ftypes.h`), i.e., plain `const char*`. No ATL
required.

### Chart data

`std::vector<NoteInfo>` owned by `PluginProcessor`, UI-thread only. Sorted by `startTime`.
`NoteInfo` holds: `startTime`, `endTime` (seconds, absolute project time), `pitch` (0–127),
`lyric` (UTF-8 string, may be empty), `isTalkie`/`isHidden` flags, `part` (`VocalPart`:
`Main` / `Harm2` / `Harm3`), `thickScale` (1.0 / 1.1 / 1.2), plus a source back-reference
(`sourceTake`, `sourceNoteIndex`, `sourceTrack`, `sourceItem`) captured during
`readChartFromReaper` so the note pitch editor can write the edit back via `MIDI_SetNote`.

Lyrics are type-5 MIDI text events matched to their nearest note-on by timestamp (50 ms
tolerance).

Notes with `pitch >= 96` are skipped (Rock Band overdrive / phrase markers).

Pitch range (`chartMinPitch` / `chartMaxPitch`) is auto-detected from loaded notes ± 2
semitones of padding.

### Vocal mode (Solo / Harmonies)

`vocalMode` atomic on the processor (set by the editor's **Vocal Mode** combo, drives a
re-read).

- **Solo:** one track containing `PART VOCALS`, all notes tagged `part = Main`.
- **Harmonies:** three tracks `HARM1`/`HARM2`/`HARM3` combined into one `chartNotes` list,
  tagged `Main`/`Harm2`/`Harm3`. Range Shift sections (pitch 0) come from **HARM1 only**
  (the `collectSections` flag on `parseChartNotes`); the vertical pitch range spans all three
  parts so nothing clips. Phrase markers (pitch 105) are collected **per part** into a
  nullable `outPhrases` list — HARM1 → `chartPhrases`, HARM2 → `chartHarm2Phrases`, HARM3 →
  none (HARM2/HARM3 can be phrased differently from the lead).

`reloadChart()` re-reads the current source for the current mode (Reaper if connected, else
the remembered `lastMidiFile`); the editor calls it on a Vocal Mode switch.

Two harmony post-passes run after building the combined list (both in `PluginProcessor.cpp`):

- `applyHarmonyOverlapThickness` — a lower part sitting on the same sustained pitch as a
  higher-priority part gets a thicker bar (Harm2 ×1.3, Harm3 ×1.6) so overlaps stay visible.
  Diagonal slides (different pitch) don't trigger it.
- `dedupHarmonyLyrics` — a HARM3 lyric identical to a HARM2 lyric at the same time
  (|Δ| < 30 ms) is marked `isHidden` (hidden in the strip; the note bar still draws).

---

## What is implemented

### Pitch detection (`PluginProcessor.cpp`)

- Three algorithms exist in code (Autocorrelation, YIN, MPM) but the **runtime selector was
  removed** — `pitchMethod` is fixed to MPM (`2`) in the editor constructor, MPM being the best
  all-round choice. The other two are kept as code-level fallbacks in case the selector is ever
  revisited; there is no longer an Algorithm UI control.
- Buffer accumulates `kPitchBufSize = 2048` samples before each detection pass
- RMS gate (`gateThreshold`) silences pitch output below threshold
- Hold decay (`kPitchHoldCycles = 6`) prevents dropout flicker
- Results written to `currentFrequency` (Hz) and `currentMidiNote` (0–127 or –1)

### Reaper MIDI reading (`readChartFromReaper`)

- Solo: finds the first track whose name contains `"PART VOCALS"`. Harmonies: finds
  `HARM1`/`HARM2`/`HARM3` and parses each (per-track `parseTrack` lambda) into one list.
- Walks all media items on each track; skips non-MIDI takes
- Reads notes with `MIDI_GetNote`, converts PPQ to seconds via `MIDI_GetProjTimeFromPPQPos`
- Reads lyrics with `MIDI_GetTextSysexEvt` (type 5)
- Auto-reloads whenever `GetProjectStateChangeCount` changes (edit, undo, tempo change, etc.)

### File-based MIDI fallback (`loadMidiFile`)

- Used when `reaperApi.isLoaded()` is false (other DAWs)
- Parses `.mid` with `juce::MidiFile`; per-track `parseSeq` lambda, same Solo/Harmonies
  track search by track name meta event
- "Load MIDI File" button lives in the **Settings popup** and is only shown there when the
  Reaper API is unavailable

### Highway visualizer (`drawHighway`)

- Horizontal scroll, 200 px/second, playhead fixed at 30% from left
- Notes drawn one part at a time (so bend-connector tracking is per-part) in z-order
  Harm3 → Harm2 → Main. Colours: Main cyan, Harm2 amber, Harm3 brown (`colourForPart`).
  Bar height = `(noteH - 1) * thickScale`.
- Lane grid lines every 2 semitones
- Inline lyric labels on bars are **Main-only** (HARM2/HARM3 lyrics live in the top strip)
- White arrowhead at playhead showing live detected pitch
- Diagnostic text when notes are empty (includes track list if the lead track not found)

### Stats panel (`drawStats`)

- Current time: `MM:SS.ms`
- Live pitch: note name + octave (e.g., `A4`), MIDI number, Hz
- Traffic light: compares live MIDI note to the **closest active note of any part**
  - Green = exact semitone match
  - Yellow = ± 1–2 semitones
  - Red = > 2 semitones off
  - Grey = no chart note active or no pitch detected

### Lyric strips (`drawLyricStrip`, `harmonyStrip` arg)

- Bottom strip: Main (HARM1/Solo) lyrics, below the highway
- Top strip (Harmonies only): shared HARM2+HARM3 lyrics above the highway
- Both share the same font and gold/white/grey colour scheme (per-part colour coding lives
  on the highway note bars, not in the strips)
- Both honour the Lyric Mode toggle (Scrolling / Static). Static uses the bottom strip's
  HARM1 phrase grid and the top strip's HARM2 phrase grid (`chartHarm2Phrases`).

### Status bar (`drawStatusBar`) + message bar (`drawMessageBar`)

Two thin strips sit between the stats panel and the controls row; both are inside the
per-frame repaint region so they update live.

- **Status bar** (`kStatusH = 22`) — always shows faintly: `N fps | REAPER priority: on/off
| renderer: CPU/GPU | calib: X ms`. Diagnostic info for power users; not a warning.
- **Message bar** (`kMessageH = 22`) — normally shows the chart status string
  (`audioProcessor.chartStatus`: "Reaper connected — reading PART VOCALS…", "Loaded N
  notes from …", etc.). When frames are dropping, overrides the status with an amber ⚠
  warning: `"[!] Can't keep up (~N fps) — shrink window, lower Frame cap, or enable REAPER
priority"`. Driven by `framesDropping` / `displayFps`, computed in `onFrame()` from
  `lastPaintSeconds` vs the requested rate and the priority policy.

### Controls

The controls split into two places: a thin **bottom control bar** with the things reached for
constantly, and a **Settings popup** (a `juce::CallOutBox` launched by the **Settings** button,
implemented by the `SettingsPanel` class in `PluginEditor.cpp`) holding everything else. There is
also an **Allow edits** toggle in the stats band (top row, above the time label).

**Bottom control bar** (laid out in thirds by `resized()`):

- Vocal Mode segmented pair: Solo / Harmonies (re-reads the chart; window grows by one strip in
  Harmonies)
- Lyric Mode segmented pair: Scrolling / Static
- Transport: `<<` / `>>` buttons (rewind / forward 5 s) flanking the **Settings** button

**Settings popup** (`SettingsPanel`):

- Frame cap combo: 20/30/50/60 fps / Uncapped (default 60)
- REAPER priority checkbox (default **off**): when on, caps paint's duty cycle so REAPER's
  UI thread stays responsive (plugin auto-skips frames under load); when off, the plugin
  renders as fast as the Frame cap allows even if REAPER lags. The status bar warns either
  way when frames are being skipped.
- Renderer combo (default **CPU (software)**): three items — `CPU (software)` (item 1),
  `OpenGL - session only` (item 2), `OpenGL - persist` (item 3). Items 2/3 attach a
  `juce::OpenGLContext` to the editor (paint rasterized on the GPU, JUCE render thread); item 1
  detaches. Only item 3 writes `savedRenderer = 2` so the GPU choice is restored on reload; item 2
  is deliberately **not** persisted (reverts to CPU next load). Experimental — see the "GPU
  rendering" backlog entry. Status bar shows `renderer: GPU`/`CPU`.
- Calibration stepper (±20 ms, −200..+500 ms, default 0): shifts the render time passed to
  every draw routine by subtracting the offset (`t = cachedPlayTime - latencyOffsetMs/1000`),
  so highway, lyric strips, and traffic-light scoring all shift together. Positive = notes
  appear later (compensates for audio output latency).
- Note Speed stepper (25 px/s steps, 100–400 px/s, default 200): sets `pixPerSec`, the shared
  scroll rate used by both the note highway and the lyric strips. Higher = faster scroll, notes
  spread further apart (fewer on screen); lower = slower, words pack tighter horizontally.
  Highway and lyrics always share this rate — decoupling them would break visual sync.
- Octave Wrapping checkbox (default **on**): when the detected pitch falls outside the chart's
  range, wraps it by octaves so an octave-off performance still scores against the right note
  (`octaveWrap`, applied in `drawHighway`/scoring).
- Mic gate slider: 0.001–0.05 RMS threshold
- Load MIDI File button: shown only when the Reaper API is unavailable (other DAWs)

**Settings persistence.** Unlike the original design, the UI settings are **no longer
session-only** — `getStateInformation`/`setStateInformation` serialise `vocalMode`,
`gateThreshold`, `frameInterval`, `reaperPriority`, `octaveWrap`, `latencyMs` (calibration),
`pixPerSec` (note speed), `lyricMode`, and `renderer` into the plugin state, restored on reopen.
The lone exception is the `OpenGL - session only` renderer item, which intentionally does not
persist.

### Transport navigation + keyboard (`keyPressed`)

The editor takes keyboard focus (`setWantsKeyboardFocus(true)`) and drives REAPER's transport.
All require `reaperApi.isLoaded()` (no-ops in other hosts):

- **Left / Right arrow** — move the edit cursor ∓5 s via `SetEditCurPos` (same as the `<<` / `>>`
  buttons).
- **Up / Down arrow** — step REAPER's play rate up/down in 0.1× increments (clamped 0.1×–4×) via
  `CSurf_OnPlayRateChange`. `cachedPlayRate` is owned locally and shown on the highway/status.
- **Click anywhere on the highway** — `mouseDown` inverts the pixel↔time mapping and seeks the
  playhead with `SetEditCurPos`. In edit mode a click that lands on a note bar starts a pitch
  drag instead (see below); clicks on empty highway still seek.

### Note pitch editing (Allow edits)

Click-and-drag pitch editing, gated behind the **Allow edits** toggle (enabled only when
`reaperApi.isLoaded() && reaperApi.canEdit()` — i.e. the write-side API pointers resolved).
Read/visualize stays the default; editing is opt-in so accidental drags while singing are
impossible.

- `NoteInfo` carries a back-reference to its source (`sourceTake`, `sourceNoteIndex`,
  `sourceTrack`, `sourceItem`), captured in `readChartFromReaper` at build time.
- `mouseDown` hit-tests the note under the cursor (`hitTestNote`); `mouseDrag` snaps vertical
  travel to whole semitones and previews live on the message thread; `mouseUp` commits via
  `MIDI_SetNote`, wrapped in `Undo_BeginBlock2`/`Undo_EndBlock2` ("Edit vocal note pitch") +
  `MarkTrackItemsDirty`, then forces a clean reload (`lastStateCount = -1`).
- Drags are clamped to the visible lanes ∩ a safe singable band (1..95) so a note can never land
  on REAPER's reserved marker pitches (0 range-shift, 105 phrase, 116/≥96 overdrive) and silently
  vanish on reload.
- Pitch only — timing and lyrics are untouched, so the source note index stays valid for the
  session. Works in Harmonies mode for free (each `NoteInfo` knows its own take).

---

## Backlog

### Bug fixes / polish

- **Cents-level accuracy** — traffic light currently compares whole semitones. Should
  measure fractional pitch offset in cents from the chart note and use ± 50 cents for
  "CLOSE" instead of ± 2 semitones.
- **Frame rate — RESOLVED.** The highway now renders at display refresh rate
  (uncapped ≈ 60 fps, smooth) inside Reaper. Driven by `juce::VBlankAttachment` →
  `onFrame()` instead of `startTimerHz`. The earlier "30 Hz is the hard ceiling"
  conclusion was **wrong** — see correction below.

  **The actual root cause (corrected):**
  Not Reaper's message pump. The old `timerCallback` made direct Reaper API calls
  (`GetProjectStateChangeCount`, `GetPlayState`, `GetPlayPosition`/`GetCursorPosition`)
  on **every** repaint tick. Raising the tick rate multiplied Reaper API traffic on the
  message thread, contending with Reaper's own message thread → Reaper-wide lag. The
  cost scaled with _repaint rate_ only because API polling was welded to it, which looked
  exactly like a "message-pump ceiling." It wasn't.

  **The fix (decouple polling from rendering — ported from the `chartchotic` plugin,
  which hits 60 fps in Reaper with the same pure-software JUCE renderer):**
  - `VBlankAttachment` drives repaints at display rate, with a high-res-tick self-throttle
    in `onFrame()` for an optional debug frame cap.
  - Reaper API calls are gated to ~20 Hz (50 ms high-res-tick gate): state-change check,
    chart reload, play-state, and stopped-cursor read only fire inside the gate.
  - The playback hot path reads the cheap `currentPlaybackTime` atomic every frame
    (written from `getPlayHead()` in `processBlock`, faster than display rate) — no Reaper
    round-trip while playing. Net result: API traffic _dropped_ (30/s → ~20/s) while
    frame rate rose to ~60/s.
  - The `rateCombo` ("Frame cap": 20/30/50/60 fps / Uncapped) sets `targetFrameInterval`.
    Default is **60 fps** (see second pass below).

  **Second pass — REAPER UI starvation at large window sizes (RESOLVED):**
  After the decouple, REAPER's _own_ UI lagged at ~2× window and stopped redrawing beyond
  that (edits applied but didn't repaint until the plugin was resized). Cause: the plugin
  paints on REAPER's shared message thread, and it was both too frequent and too expensive
  per call. `chartchotic` "never got as bad" not because of an image cache (our procedural
  fill is actually cheaper per-pixel on the CPU renderer) but because it windows its note
  iteration and we didn't. Three fixes:
  - **Default frame cap 60 fps** (`targetFrameInterval = 1/60`) so high-refresh monitors
    don't drive 120–144 full repaints/sec at the host.
  - **Idle-repaint gating** in `onFrame()`: only `repaint()` when `cachedPlayTime`,
    `currentMidiNote`, or `lastStateCount` changed. While stopped+silent (e.g. editing
    notes) the plugin idles and frees the host thread entirely. During playback time
    changes every frame so it still repaints continuously.
  - **Windowed note loop** in `drawNotesForPart`: `std::lower_bound` to the first note at
    `leftTime = visible-left − kNoteLookback (5 s)` and `break` once past `rightTime` —
    O(visible) per frame instead of O(all notes × parts), independent of song length.
    (Note: this trims iteration only; drawing was already visible-bounded, so it is
    roughly _neutral_ on paint cost — kept because it can't hurt and helps long songs.)
  - **Paint-budget duty-cycle cap** (the actual fix for wide-window _playback_ stalls):
    `paint()` is timed (EMA in `lastPaintSeconds`), and `onFrame()` uses
    `effInterval = max(targetFrameInterval, lastPaintSeconds * kBusyFactor)` (kBusyFactor=3)
    as the throttle. This guarantees paint never eats more than ~1/3 of the wall clock, so
    REAPER's shared UI thread always gets time — at large window sizes the plugin
    auto-degrades its fps instead of freezing the host. Per-frame paint cost scales with
    _visible content_ (wider/taller window → more pixels filled + more lyric/label text
    rasterized), which is the real width-dependent bottleneck; this cap bounds its impact
    on the host regardless of which part of paint is heavy.

  **Easy wins (kept, still apply):**
  - `setOpaque(true)` — skips parent compositing.
  - `repaint(getLocalBounds().withBottom(getHeight() - kControlsH))` — excludes the
    controls panel (~40% of default window area) from every repaint.
  - Removed `g.fillAll(black)` — each sub-area fills itself; the full-component black
    pass was painting every pixel twice.

  **Highway grid cache — attempted and reverted (made things worse), DO NOT retry:**
  Tried caching the highway background + grid lines into a `juce::Image` (RGB,
  highway area size), rebuilt on size/pitch-range changes, blitted with `g.drawImageAt`
  in `drawHighway()`. Result: performance regressed — default size lagged, wider sizes
  worse.

  Root cause: JUCE's CPU software renderer makes `drawImageAt` a read-then-write
  operation (read every pixel from source image, write to framebuffer). By contrast,
  `fillRect` + `drawHorizontalLine` are write-only operations (no source read).
  Write-only is faster than read+write on modern CPUs due to write-combining and
  the absence of cache-miss penalty on the source buffer. Replacing N scanline
  writes with a full image copy made the paint cost higher, not lower.

  **Conclusion:** For JUCE software rendering, direct draw calls (fillRect,
  drawHorizontalLine) beat pixel-buffer caching for solid fills. Caching only pays
  off when primitives have high per-call CPU overhead (e.g., GPU draw calls, complex
  path rasterization). Do not attempt this again.

- **GPU rendering — minimal spike IMPLEMENTED (experimental, opt-in); full decouple is the
  pivot.** The paint-load fixes keep REAPER responsive but at extreme window sizes force a
  trade: with "REAPER priority" on the plugin degrades its own fps. Attaching a
  `juce::OpenGLContext` to the editor can remove that trade.
  - _What ships now (the minimal `attachTo` spike):_ an opt-in **Renderer** combo in the
    Settings popup, CPU (software) by default. Picking `OpenGL - session only` or
    `OpenGL - persist` calls `glContext.attachTo(*this)`; picking `CPU (software)` calls
    `glContext.detach()`. Component painting stays enabled (the default), so JUCE rasterizes the
    existing `paint()` onto the GPU on its own render thread. `continuousRepainting` is left off —
    the existing VBlank `repaint()` + idle-gating still drive frames. Only the `persist` item is
    saved (`savedRenderer = 2`, restored on reopen); `session only` reverts to CPU next load. The
    status bar shows `renderer: GPU` / `CPU`. Detach happens in the editor destructor too.
  - _Why no chart snapshot is needed for this path:_ with an attached context and component
    painting on, JUCE's render thread **acquires the `MessageManagerLock` before calling
    `paint()`** and releases it right after (verified in `juce_OpenGLContext.cpp`
    `renderFrame()` — lock → `paintComponent()` → release; it even sleeps 2 ms after release
    to avoid hogging the message thread). So `paint()` stays serialized with the message
    thread and the existing direct `getChartNotes()` reads remain thread-safe **as-is**.
    Live MIDI editing still auto-reloads + redraws exactly as before.
  - _Why it would help (the real win — only fully realized by the pivot below):_ the cost
    that scales at large sizes is **text/glyph rasterization** (more visible notes → more
    glyphs/frame), which a GPU glyph atlas accelerates; faster paint = MM lock held briefly
    = message thread freed sooner. The _full_ win (GL thread never blocking the message
    thread at all) needs the decouple below.
  - _Pivot if the minimal spike underdelivers — full decoupled renderer (NOT yet done,
    needs the snapshot rework):_ replace component painting with a custom
    `juce::OpenGLRenderer::renderOpenGL()` running free on the GL thread — paint then runs
    **without** the MM lock, fully removing host contention. Because paint no longer holds
    the MM lock, it can no longer read the live `chartNotes` vector: the message thread must
    hand it an **immutable chart snapshot** (atomic `shared_ptr` swap, or a `CriticalSection`
    - extract — `chartchotic`'s `noteStateMapLock` pattern is the reference for _this_, it is
      a software renderer, not a GL example). Skipping that sync is what would crash. This is
      the sizable rework the project deferred; pursue only if the minimal toggle proves
      insufficient on real setups.
  - _Catch — host/driver fragility:_ JUCE+GL+VST3 has known rough edges (Windows
    multi-context / Intel iGPU, REAPER separate-process plugin bridging, resize/DPI context
    recreation). macOS deprecated OpenGL in 10.14 (2018). JUCE 8 (2024) ships a **Direct2D**
    Windows renderer — likely the better long-term GPU path than raw GL, but needs a
    JUCE 7→8 upgrade. The opt-in toggle exists precisely so users can confirm stability on
    their own rigs before GL could ever be considered default.
  - _JUCE 8 upgrade — ATTEMPTED AND REVERTED (June 2025)._ A mechanical upgrade from
    7.0.12 → 8.0.13 was completed (Font API, VBlankAttachment lambda, GlyphArrangement
    string-width calls) and built cleanly, but introduced a significant performance
    regression: frame drops at 60 fps that did not exist on JUCE 7, and OpenGL toggling
    dropped to ~30 fps and crashed Reaper.

    Root cause: **JUCE 8 makes Direct2D the default Windows renderer.** All of the
    paint-budget throttle tuning (`kBusyFactor`, `lastPaintSeconds` EMA, idle-repaint
    gating, windowed note loop) was built and validated against JUCE 7's GDI software
    renderer. D2D has different per-frame overhead characteristics that throw off that
    tuning. Forcing software rendering at runtime (`setCurrentRenderingEngine(0)` in
    `parentHierarchyChanged`) did not fully close the gap — JUCE 8's GDI path also
    has higher overhead than JUCE 7's. The OpenGL crash was likely D2D + GL context
    interacting simultaneously (D2D is active by default, then GL is also attached).

    **Conclusion:** A minimal port of the existing paint code to JUCE 8 does not work.
    To upgrade again, the entire paint pipeline should be rebuilt to lean into Direct2D
    natively — replace the current procedural `paint()` with D2D-idiomatic draw calls,
    re-profile the paint-budget logic against the D2D cost model, and reconsider the
    OpenGL toggle (D2D already provides GPU acceleration, so the toggle may be
    unnecessary). The full decoupled-renderer pivot (custom `renderOpenGL()` on the GL
    thread, with immutable chart snapshot) remains viable as an alternative GPU path but
    requires the same chart-snapshot rework regardless of JUCE version.

  - _Note:_ this does **not** help lyric-text _smoothness_ (that's frame rate + sub-pixel
    positioning, already handled) — only the host-contention/paint-cost ceiling.

- **Scrolling lyric readability** — historically diagnosed at 30 Hz / 200 px/sec, where
  the text jumped ~6.7 px per frame and read as motion blur. At the current ~60 fps the
  jump roughly halves (~3.3 px/frame), so this is materially better now — re-evaluate
  whether it still needs work before investing further. Investigated approaches (from the
  30 Hz era):
  - 80 px/sec scroll — improved blur but broke the visual sync between the highway note
    position and the lyric strip (they scrolled at different speeds). Reverted.
  - Black outline (8× drawText at ±1 px offsets) — no visible improvement, made 60 Hz
    start lagging due to the extra draw calls. Reverted.
  - Background pill behind each word — slight improvement; kept in current code. Active
    syllable uses warm dark amber (`0xff2a2200`), others use cool dark (`0xff141428`).
  - Sub-pixel rendering via `juce::GlyphArrangement::draw` with fractional
    `AffineTransform::translation` — marginal improvement, kept in current code.
  - Larger font (20 px) and black pill — both individually noticeable improvements but
    reverted to keep the baseline clean while planning a proper fix.

  **Root cause:** the per-frame px jump is the core issue; styling helps at the margins
  but doesn't fix it. Higher frame rate (now ≈ 60 fps) directly halves the jump and is the
  biggest lever — already applied.

  **If still not smooth enough — remaining avenues:**
  - Wall-clock interpolation between Reaper polls: in `paint()` advance `t` by the
    wall-clock elapsed since the last position read. Mostly moot now that the playing hot
    path reads `currentPlaybackTime` every frame from the atomic (already near display
    rate), but available if the atomic ever proves too coarse.
  - Larger font (20 px bold) with the black pill was subjectively better; revisit now that
    smoothness is improved so the styling improvement is additive.

- **Track identity by GUID** — track search uses name matching, which breaks if the user
  renames the track. `GetTrackGUID` would give a stable pointer across renames.
- **Transport state** — `GetPlayState()` and `GetCursorPosition()` are read inside the
  ~20 Hz poll gate. When stopped, `cachedPlayTime` tracks the edit cursor so the highway
  updates as the user clicks around to find a start position. When playing, the playback
  position comes from the `currentPlaybackTime` atomic (read every frame), not a per-frame
  `GetPlayPosition()` call. Future: visually indicate stopped/playing/recording state in
  the highway UI (the bitmask is already available — 1=play, 2=pause, 4=record).

### Implemented features

- **Harmonies mode** — Solo (PART VOCALS) and Harmonies (HARM1/2/3) on one highway with
  per-part colours, overlap thickness, lyric dedup, and a second (top) lyric strip. See
  the "Vocal mode" section above.
- **Settings popup** — most controls moved off the main bar into a `CallOutBox` (`SettingsPanel`),
  and the UI settings now persist via `get`/`setStateInformation`. See the "Controls" section.
- **Transport navigation** — click-to-seek on the highway, Left/Right arrow ∓5 s (+ `<<`/`>>`
  buttons), Up/Down arrow play-rate stepping. See "Transport navigation + keyboard".
- **Write-back to REAPER — note pitch editing (DONE).** Both originally-scoped interactions
  shipped: click-highway-to-seek (`SetEditCurPos`) and drag-a-note-to-change-pitch
  (`MIDI_SetNote` under an undo block), gated behind the **Allow edits** toggle. Full details in
  the "Note pitch editing" section above. Implementation notes vs the original plan:
  - The back-reference on `NoteInfo` is `sourceTake` + `sourceNoteIndex` (+ `sourceTrack`,
    `sourceItem` for the dirty mark), captured in `readChartFromReaper`.
  - Auto-reload interplay was handled by **previewing the drag on the in-memory `NoteInfo`** and
    only committing on `mouseUp`, then forcing one clean reload (`lastStateCount = -1`) — rather
    than suppressing the poll mid-drag. Simpler and avoids a stuck-suppressed state.
  - Only pitch is editable. Timing/lyric edits and the **Edit Mode** name from the original plan
    (now shipped as **Allow edits**) are the only deltas.

### Upcoming features (user-identified, to be detailed)

- Phrase-level scoring feedback (did the user hit all notes in a phrase?)
- GPU rendering — opt-in **Renderer** combo (CPU / OpenGL session-only / OpenGL persist)
  **implemented** (experimental); full decoupled-renderer is the documented pivot. See the
  "GPU rendering" bullet under the frame-rate backlog entry above.
- Note **timing/length** editing (drag horizontally / resize) — the back-reference plumbing from
  pitch editing makes this the natural next step, but it reorders MIDI (sorted by time) so the
  source index can stale mid-edit; needs re-resolution after each commit.
- Any other improvements/bug fixes the user has in mind — document here as they're scoped

### Out of scope

- Per-track lane separation (the harmony parts share one pitch axis by design, not separate
  lanes)

---

## Build

```
cd rock-band-vst/vocal-rb-sing
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

Output: `build/RBVocalSing_artefacts/Debug/VST3/Rock Band Vocal Sing - Reaper.vst3`
(the bundle name comes from `PRODUCT_NAME` in `CMakeLists.txt`).

To install, copy `Rock Band Vocal Sing - Reaper.vst3` into the system VST3 folder
(`C:\Program Files\Common Files\VST3` on Windows) and re-scan in REAPER.

---

## Known gotchas

- `DEF_CLASS_IID(IReaperHostApplication)` must appear in exactly **one** .cpp file.
  It is in `PluginProcessor.cpp`. Do not add it to any header.
- `#define NOMINMAX` must precede any Windows headers to avoid `min`/`max` macro conflicts
  with `std::min` / `std::max`.
- `setIHostApplication` may never be called (other DAWs). Always guard with
  `reaperApi.isLoaded()` before calling any function pointer.
- All Reaper API calls are message-thread only. Never call them from `processBlock`.
- `MIDI_GetProjTimeFromPPQPos` converts PPQ to absolute project seconds correctly across
  tempo changes. Never compute time from PPQ with a fixed tempo.
- `msglen` in `MIDI_GetTextSysexEvt` is in/out: set it to buffer capacity before the call,
  then it contains the actual byte count after. Always null-terminate manually.
