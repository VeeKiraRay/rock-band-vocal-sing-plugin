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
//   PaintTooSlow  — our own paint() cannot fit in the frame budget. Every frame we ask for
//                   arrives; there are just not many of them. Shrink the window, lower the
//                   Frame cap, enable REAPER priority.
//   StarvedPaint  — paint() is fast and we ask for frames that never arrive. JUCE defers
//                   editor repaints to Windows' WM_PAINT, which is only delivered when the
//                   host's message queue is empty; a busy REAPER UI thread can starve it
//                   indefinitely. The OpenGL renderer bypasses that path entirely.
//   StarvedThread — the stage before that: REAPER stops dispatching our VBlank callback, so
//                   we never even ask. Requested and delivered then agree perfectly and the
//                   delivery ratio sees nothing — only the tick rate witnesses it. Same
//                   advice, different sentence, because "N of M frames drawn" would report
//                   "4 of 4" here and read as no fault at all.

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

// onFrame() runs on the display's VBlank whatever the picture is doing, so its rate is the
// one signal here that does not depend on wanting frames. Falling this far under the Frame
// cap means the host is not dispatching to us at all — a different stage of the same
// problem, and one that nothing inside onFrame() could fix.
constexpr double kTickStarveRatio = 0.5;

struct FrameStats
{
    double capFps        = 60.0;  // the Frame cap (60 when uncapped) — the rate asked for
    double achievableFps = 60.0;  // best rate paint() and the REAPER-priority budget allow
    double requestedFps  = 0.0;   // measured repaint() calls/s — frames the idle gate wanted
    double deliveredFps  = 0.0;   // measured paint() calls/s — frames that reached the screen
    double tickFps       = 0.0;   // measured onFrame() calls/s — how often we run at all
};

enum class FrameVerdict { Healthy, PaintTooSlow, StarvedPaint, StarvedThread };

// Both starved states want the same fix (the OpenGL renderer) and differ only in which
// stage lost the frame, so the message bar branches on this rather than on the enum.
inline bool isStarved (FrameVerdict v) noexcept
{
    return v == FrameVerdict::StarvedPaint || v == FrameVerdict::StarvedThread;
}

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

    if (s.tickFps < kTickStarveRatio * s.capFps)
        return FrameVerdict::StarvedThread;

    if (s.requestedFps < kMinRequestedFps)
        return FrameVerdict::Healthy;

    if (s.deliveredFps < kDeliveryRatio * s.requestedFps)
        return FrameVerdict::StarvedPaint;

    return FrameVerdict::Healthy;
}

} // namespace rbperf
