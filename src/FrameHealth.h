#pragma once

// Frame health — is the editor actually getting the frames it asks for, and if not, why?
//
// Pure and JUCE-free (header-only, no .cpp) so the rule can be unit-tested without an
// editor, a peer or a Graphics context. onFrame() measures the four rates below over a
// ~1 s window and calls classifyFrames() once per window; drawStatusBar and drawMessageBar
// present the result.
//
// Background (2026-08-29). Dragging REAPER's playhead made the picture update in lurches
// on the CPU renderer, and the plugin's own diagnostics reported nothing wrong: the old
// "fps" figure was *modelled* from how long paint() took rather than measured, so a scrub
// that delivered ~3 frames a second read as a contented 60. Two different faults produce
// "the picture is not keeping up", they want opposite advice, and only measurement at the
// point of delivery can tell them apart:
//
//   PaintTooSlow — our own paint() cannot fit in the frame budget. Every frame we ask for
//                  arrives; there are just not many of them. Shrink the window, lower the
//                  Frame cap, enable REAPER priority.
//   Starved      — paint() is fast and we ask for frames that never arrive. JUCE defers
//                  editor repaints to Windows' WM_PAINT, which is only delivered when the
//                  host's message queue is empty; a busy REAPER UI thread can starve it
//                  indefinitely.
//
// There was briefly a third, StarvedThread, keyed on the tick rate: REAPER dispatching our
// VBlank callback so rarely that we never even asked for the frames. It was **removed once
// the forced redraw shipped** (see onFrame), because that changed what a tick means — every
// tick we do get now produces a real frame on screen, so a low tick rate no longer predicts
// a bad picture. Left in, it fired through every scrub on a rig whose highway was tracking
// the drag perfectly well, and on the OpenGL renderer too, where its advice ("switch to
// OpenGL") was nonsense. Do not reintroduce it without a symptom that the delivery test
// misses.

namespace rbperf
{

// A shortfall against the Frame cap this large counts as "paint cannot keep up". Carried
// over unchanged from the original modelled test so that warning behaves as it always has.
constexpr double kSlowFactor = 1.15;

// Below this share of the frames we asked for, they are not arriving. Deliberately
// generous: the point is to catch a stall of several-to-one, not ordinary jitter.
constexpr double kDeliveryRatio = 0.6;

// The delivery test needs a meaningful denominator. Idle-repaint gating means a parked
// transport legitimately asks for almost nothing, and two frames out of three at that rate
// is measurement quantisation rather than a fault.
constexpr double kMinRequestedFps = 10.0;

struct FrameStats
{
    double capFps        = 60.0;  // the Frame cap (60 when uncapped) — the rate asked for
    double achievableFps = 60.0;  // best rate paint() and the REAPER-priority budget allow
    double requestedFps  = 0.0;   // measured repaint() calls/s — frames the idle gate wanted
    double deliveredFps  = 0.0;   // measured paint() calls/s — frames that reached the screen
    // Measured onFrame() calls/s. No longer a fault signal in its own right (see the
    // StarvedThread note above) — kept because it is the "have we measured anything yet"
    // witness, and because the profile build's drawn/wanted/ticks readout needs it.
    double tickFps       = 0.0;
};

enum class FrameVerdict { Healthy, PaintTooSlow, Starved };

inline FrameVerdict classifyFrames (const FrameStats& s) noexcept
{
    // No completed measurement window yet. A genuinely zero tick rate means the editor is
    // not running at all, in which case it cannot paint a warning about it either.
    if (s.tickFps <= 0.0)
        return FrameVerdict::Healthy;

    // Checked first: when paint is the bottleneck it is also the thing to fix, and the
    // delivery shortfall it causes is a symptom rather than a second fault.
    if (s.achievableFps < s.capFps / kSlowFactor)
        return FrameVerdict::PaintTooSlow;

    if (s.requestedFps < kMinRequestedFps)
        return FrameVerdict::Healthy;

    if (s.deliveredFps < kDeliveryRatio * s.requestedFps)
        return FrameVerdict::Starved;

    return FrameVerdict::Healthy;
}

} // namespace rbperf
