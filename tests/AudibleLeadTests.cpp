#include "AudibleLead.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

// rblead::LeadFilter — the filter behind the measured audible lead. Pure, so these run
// without REAPER. Typical values from the 2026-11-12 finding: ~0.2 s with anticipative FX
// processing on, 0.02-0.05 s with it off.

using rblead::LeadFilter;
using Catch::Approx;

namespace
{
void pushN (LeadFilter& f, double v, int n) { for (int i = 0; i < n; ++i) f.push (v); }
}

TEST_CASE ("audible lead: nothing is reported until it has settled", "[lead]")
{
    LeadFilter f;
    REQUIRE_FALSE (f.hasValue());
    // The first samples after a (re)start are ignored, then a few are needed.
    pushN (f, 0.2, rblead::kSettleSamples + rblead::kMinSamples - 1);
    REQUIRE_FALSE (f.hasValue());
    f.push (0.2);
    REQUIRE (f.hasValue());
    REQUIRE (f.value() == Approx (0.2));
}

TEST_CASE ("audible lead: settle samples are really ignored", "[lead]")
{
    // A wild first reading right after playback starts must not reach the median.
    LeadFilter f;
    f.push (0.9);
    f.push (0.9);
    pushN (f, 0.2, rblead::kMinSamples);
    REQUIRE (f.value() == Approx (0.2));
}

TEST_CASE ("audible lead: a seek transient does not move it", "[lead]")
{
    LeadFilter f;
    pushN (f, 0.2, rblead::kSettleSamples + rblead::kWindow);
    f.push (0.6);   // the two positions caught either side of a short seek
    f.push (0.55);
    REQUIRE (f.value() == Approx (0.2));
}

TEST_CASE ("audible lead: implausible samples are discarded outright", "[lead]")
{
    LeadFilter f;
    pushN (f, 0.04, rblead::kSettleSamples + rblead::kMinSamples);
    f.push (5.0);    // a long seek
    f.push (-3.0);   // a loop wrap
    REQUIRE (f.value() == Approx (0.04));
    REQUIRE (f.lastSample() == Approx (0.04));
}

TEST_CASE ("audible lead: follows a real change within the window", "[lead]")
{
    // Anticipative FX switched off mid-session: 0.2 -> 0.04.
    LeadFilter f;
    pushN (f, 0.2,  rblead::kSettleSamples + rblead::kWindow);
    pushN (f, 0.04, rblead::kWindow / 2 + 1);
    REQUIRE (f.value() == Approx (0.04));
}

TEST_CASE ("audible lead: reset starts again from nothing", "[lead]")
{
    LeadFilter f;
    pushN (f, 0.2, rblead::kSettleSamples + rblead::kWindow);
    f.reset();
    REQUIRE_FALSE (f.hasValue());
}

TEST_CASE ("audible lead: median of an even count averages the middle pair", "[lead]")
{
    LeadFilter f;
    pushN (f, 0.0, rblead::kSettleSamples);
    f.push (0.10);
    f.push (0.20);
    f.push (0.30);
    f.push (0.40);
    REQUIRE (f.value() == Approx (0.25));
}

TEST_CASE ("audible lead: a large buffer with anticipative FX is still a lead", "[lead]")
{
    // 2048 blocks + anticipative FX measured 675 ms; a 4096 buffer can pass a second.
    LeadFilter f;
    pushN (f, 1.3, rblead::kSettleSamples + rblead::kMinSamples);
    REQUIRE (f.hasValue());
    REQUIRE (f.value() == Approx (1.3));
}

TEST_CASE ("audible lead: settle skips the first samples but keeps the value", "[lead]")
{
    // A new pass must start compensated: the old value stays while the new pass settles.
    LeadFilter f;
    pushN (f, 0.27, rblead::kSettleSamples + rblead::kWindow);
    f.settle();
    REQUIRE (f.hasValue());
    f.push (0.9);   // unsettled first readings of the new pass
    f.push (0.9);
    REQUIRE (f.value() == Approx (0.27));
}

TEST_CASE ("audible lead: sessions calibrated before compensation keep their behaviour", "[lead]")
{
    using rblead::autoLeadForSession;
    // No key saved yet: on only if Output calib was never set.
    REQUIRE (autoLeadForSession (false, false, 0));
    REQUIRE_FALSE (autoLeadForSession (false, false, 100));
    REQUIRE_FALSE (autoLeadForSession (false, false, -40));
    // Once saved, the user's choice wins either way.
    REQUIRE (autoLeadForSession (true, true, 100));
    REQUIRE_FALSE (autoLeadForSession (true, false, 0));
}
