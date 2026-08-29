#include "FrameHealth.h"
#include <catch2/catch_test_macros.hpp>

// rbperf::classifyFrames — the frame-health rule. Pure, so these run without an editor.
// Background: the 2026-08-29 scrub-redraw investigation, which found that dragging
// REAPER's playhead starves the plugin's redraws on the CPU renderer while the old
// modelled fps figure cheerfully reported 60.

using rbperf::FrameStats;
using rbperf::FrameVerdict;
using rbperf::classifyFrames;

// A rig running exactly as asked: 60 fps cap, paint comfortably inside the budget, every
// requested frame delivered, VBlank arriving at display rate. Each test below perturbs
// one thing about this.
static FrameStats healthy()
{
    FrameStats s;
    s.capFps        = 60.0;
    s.achievableFps = 60.0;
    s.requestedFps  = 60.0;
    s.deliveredFps  = 58.0;
    s.tickFps       = 60.0;
    return s;
}

TEST_CASE ("frame health: a rig keeping up reports nothing", "[frames]")
{
    REQUIRE (classifyFrames (healthy()) == FrameVerdict::Healthy);
}

TEST_CASE ("frame health: no measurement yet is not a fault", "[frames]")
{
    // The first second after the editor opens, before any rate window has closed. Without
    // this guard the warning would flash on every editor open.
    FrameStats s = healthy();
    s.tickFps      = 0.0;
    s.deliveredFps = 0.0;
    s.requestedFps = 0.0;
    REQUIRE (classifyFrames (s) == FrameVerdict::Healthy);
}

TEST_CASE ("frame health: an idle transport is not starving", "[frames]")
{
    // Idle-repaint gating: nothing is moving, so almost no frames are asked for and almost
    // none are drawn. Delivered/requested is meaningless here, and without the floor this
    // would sit on screen permanently whenever the transport was parked.
    FrameStats s = healthy();
    s.requestedFps = 1.0;
    s.deliveredFps = 1.0;
    REQUIRE (classifyFrames (s) == FrameVerdict::Healthy);

    // The pathological form of the same thing: one frame wanted in the window, none yet
    // drawn. Still not a fault.
    s.requestedFps = 1.0;
    s.deliveredFps = 0.0;
    REQUIRE (classifyFrames (s) == FrameVerdict::Healthy);
}

TEST_CASE ("frame health: a low Frame cap that is being honoured is not a fault", "[frames]")
{
    // The user asked for 20 fps and is getting 20. Nothing here is wrong, and the tick
    // rate legitimately runs far above the cap because onFrame() is throttled, not the
    // VBlank. The tick test must measure against the cap, never the other way round.
    FrameStats s;
    s.capFps        = 20.0;
    s.achievableFps = 20.0;
    s.requestedFps  = 20.0;
    s.deliveredFps  = 19.0;
    s.tickFps       = 60.0;
    REQUIRE (classifyFrames (s) == FrameVerdict::Healthy);
}

TEST_CASE ("frame health: the REAPER-priority budget is a throttle, not a fault", "[frames]")
{
    // REAPER priority deliberately lowers the achievable rate so paint cannot monopolise
    // the host's UI thread. That is the setting working, so it must report as capability
    // ("paint cannot hit your cap"), never as frames going missing.
    FrameStats s;
    s.capFps        = 60.0;
    s.achievableFps = 20.0;   // paint 16 ms x kBusyFactor 3
    s.requestedFps  = 20.0;
    s.deliveredFps  = 20.0;   // every frame it asked for did arrive
    s.tickFps       = 60.0;
    REQUIRE (classifyFrames (s) == FrameVerdict::PaintTooSlow);
}

TEST_CASE ("frame health: a window too big to paint reports paint, not starvation", "[frames]")
{
    // REAPER priority off, a maximised window: paint alone overruns the frame interval.
    // Every frame asked for is delivered - there are just few of them.
    FrameStats s;
    s.capFps        = 60.0;
    s.achievableFps = 11.0;   // ~90 ms per paint
    s.requestedFps  = 11.0;
    s.deliveredFps  = 11.0;
    s.tickFps       = 60.0;
    REQUIRE (classifyFrames (s) == FrameVerdict::PaintTooSlow);
}

TEST_CASE ("frame health: repaints asked for and not delivered read as starved", "[frames]")
{
    // The scrub profile. Stopped transport, so the ~20 Hz poll gate is what moves the
    // cursor and therefore what asks for frames; paint itself is fast. WM_PAINT is
    // starved by REAPER's busy message queue and 3 of those 19 frames reach the screen.
    FrameStats s;
    s.capFps        = 60.0;
    s.achievableFps = 60.0;
    s.requestedFps  = 19.0;
    s.deliveredFps  = 3.0;
    s.tickFps       = 58.0;
    REQUIRE (classifyFrames (s) == FrameVerdict::Starved);
}

TEST_CASE ("frame health: a low tick rate on its own is not a fault", "[frames]")
{
    // Regression for a rule that was deliberately REMOVED (2026-08-29). There used to be a
    // StarvedThread verdict keyed on tickFps < 0.5 * capFps: REAPER dispatching our VBlank
    // callback so rarely that we never even asked for frames.
    //
    // The forced redraw in onFrame() changed what a tick means. Every tick we do get now
    // produces a real frame on screen, so a low tick rate no longer predicts a bad picture -
    // and left in, the rule fired through every playhead drag on a rig whose highway was
    // tracking the drag perfectly well, on the OpenGL renderer too, where its advice
    // ("switch to OpenGL") was nonsense.
    //
    // So: ticks far below the cap, every requested frame delivered, must be Healthy.
    FrameStats s;
    s.capFps        = 60.0;
    s.achievableFps = 60.0;
    s.requestedFps  = 15.0;
    s.deliveredFps  = 15.0;
    s.tickFps       = 4.0;
    REQUIRE (classifyFrames (s) == FrameVerdict::Healthy);
}

TEST_CASE ("frame health: slow paint outranks starvation when both are true", "[frames]")
{
    // A big window being scrubbed. Both faults are present; paint is the one the user can
    // do something about first, and the OpenGL advice would be misleading while the window
    // is too big to render at any rate.
    FrameStats s;
    s.capFps        = 60.0;
    s.achievableFps = 11.0;
    s.requestedFps  = 11.0;
    s.deliveredFps  = 2.0;
    s.tickFps       = 58.0;
    REQUIRE (classifyFrames (s) == FrameVerdict::PaintTooSlow);
}
