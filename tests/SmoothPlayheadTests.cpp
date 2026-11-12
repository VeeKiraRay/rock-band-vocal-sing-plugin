#include "SmoothPlayhead.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>

// rbsmooth::Estimator — the Smooth playhead rule. Pure, so these run without an editor.
// The rig being simulated is the one that found the problem (2026-11-12): 4096-sample
// blocks at 48 kHz, so the host reports a new position every ~85 ms, drawn at 60 fps.

using rbsmooth::Estimator;
using Catch::Approx;

namespace
{
constexpr double kBlock = 4096.0 / 48000.0;   // ~85.3 ms between reported positions
constexpr double kFrame = 1.0 / 60.0;

// The position a host would report at wall time t, playing from 0 at normal speed: the
// start of the block being played, so it only advances once per block.
double reportedAt (double t, double block = kBlock) { return std::floor (t / block) * block; }

// Drive an estimator frame by frame from wall time t0 to t1 and return what it drew.
std::vector<double> run (Estimator& e, double t0, double t1, double block = kBlock)
{
    std::vector<double> drawn;
    for (double t = t0; t < t1; t += kFrame)
        drawn.push_back (e.update (reportedAt (t, block), t, 1.0, true));
    return drawn;
}
} // namespace

TEST_CASE ("smooth playhead: stopped is exactly the reported position", "[smooth]")
{
    Estimator e;
    REQUIRE (e.update (12.345, 1.0, 1.0, false) == 12.345);
    REQUIRE (e.update (20.0,   1.1, 1.0, false) == 20.0);
}

TEST_CASE ("smooth playhead: a large buffer moves the picture on every frame", "[smooth]")
{
    // The point of the feature. Unsmoothed, only about one frame in five would move.
    Estimator e;
    const auto drawn = run (e, 0.0, 3.0);

    int moved = 0, steps = 0;
    for (size_t i = 30; i < drawn.size(); ++i)   // after the block interval is learned
    {
        const double step = drawn[i] - drawn[i - 1];
        REQUIRE (step >= 0.0);                    // never backwards
        REQUIRE (step <= kFrame * rbsmooth::kCatchUp + 1.0e-9);
        ++steps;
        if (step > 0.0) ++moved;
    }
    REQUIRE ((double) moved / steps > 0.9);
}

TEST_CASE ("smooth playhead: stays within one block of the reported position", "[smooth]")
{
    Estimator e;
    for (double t = 0.0; t < 3.0; t += kFrame)
    {
        const double rep = reportedAt (t);
        const double d   = e.update (rep, t, 1.0, true);
        REQUIRE (d >= rep - 1.0e-9);
        REQUIRE (d <= rep + kBlock + 1.0e-9);
    }
}

TEST_CASE ("smooth playhead: a stalled host cannot run the picture ahead", "[smooth]")
{
    // Blocks stop arriving (a stop not yet polled, a host stall): the estimate runs out at
    // one block interval past the last report and waits there.
    Estimator e;
    run (e, 0.0, 2.0);
    const double last = reportedAt (2.0 - kFrame);

    double d = 0.0;
    for (double t = 2.0; t < 3.0; t += kFrame)
        d = e.update (last, t, 1.0, true);

    REQUIRE (d <= last + std::min (e.measuredBlockInterval(), rbsmooth::kMaxLeadSec) + 1.0e-9);
    REQUIRE (d > last);   // it did lead, by up to that much
}

TEST_CASE ("smooth playhead: a forward seek snaps", "[smooth]")
{
    Estimator e;
    run (e, 0.0, 1.0);
    REQUIRE (e.update (30.0, 1.0, 1.0, true) == 30.0);
}

TEST_CASE ("smooth playhead: a loop wrap or backward seek snaps back", "[smooth]")
{
    // Never-backwards applies to corrections, not to the transport jumping back.
    Estimator e;
    run (e, 0.0, 2.0);
    REQUIRE (e.update (0.5, 2.0, 1.0, true) == 0.5);
}

TEST_CASE ("smooth playhead: a block behind the picture is waited out, not shown", "[smooth]")
{
    Estimator e;
    run (e, 0.0, 1.0);
    const double before = e.update (reportedAt (1.0), 1.0, 1.0, true);
    // A report a few ms behind what is on screen: a correction, not a seek.
    const double after  = e.update (before - 0.005, 1.0 + kFrame, 1.0, true);
    REQUIRE (after >= before);
}

TEST_CASE ("smooth playhead: a late block is caught up at bounded speed", "[smooth]")
{
    Estimator e;
    run (e, 0.0, 1.0);
    const double base = reportedAt (1.0 - kFrame);

    // No report for 0.2 s, then one that is 0.2 s on: the picture had stopped at the lead
    // cap, so it is behind, and must close the gap faster than real time but not in a jump.
    double d = 0.0;
    for (double t = 1.0; t < 1.2; t += kFrame)
        d = e.update (base, t, 1.0, true);

    const double next = e.update (base + 0.2, 1.2, 1.0, true);
    REQUIRE (next - d <= kFrame * rbsmooth::kCatchUp + 1.0e-9);
    REQUIRE (next > d);
}

TEST_CASE ("smooth playhead: small blocks are effectively unchanged", "[smooth]")
{
    // 128-sample blocks: a new position every tick, so there is nothing to estimate and the
    // drawn position stays within a frame of the reported one.
    constexpr double small = 128.0 / 48000.0;
    Estimator e;
    for (double t = 0.0; t < 2.0; t += kFrame)
    {
        const double rep = reportedAt (t, small);
        const double d   = e.update (rep, t, 1.0, true);
        REQUIRE (std::abs (d - rep) <= kFrame);
    }
}

TEST_CASE ("smooth playhead: play rate scales the estimate", "[smooth]")
{
    // At half speed the host reports half as much progress per block.
    Estimator e;
    double d = 0.0;
    for (double t = 0.0; t < 2.0; t += kFrame)
        d = e.update (0.5 * reportedAt (t), t, 0.5, true);
    REQUIRE (d == Approx (0.5 * reportedAt (2.0 - kFrame)).margin (0.5 * kBlock + 1.0e-6));
}
