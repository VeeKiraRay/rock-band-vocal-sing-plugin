#include "HighwayRange.h"
#include <catch2/catch_test_macros.hpp>

// rbhighway::visibleRange — the highway's pitch axis rules. Pure, so these run without
// an editor or a Graphics context. Background: _upcoming/CHART_REVIEW_FINDINGS.md §2.

using rbhighway::visibleRange;
using rbhighway::kVocalLow;
using rbhighway::kVocalHigh;
using rbhighway::kMinSpan;

TEST_CASE ("highway range: ordinary content is padded and otherwise untouched",
           "[highway]")
{
    // The no-op case, and the one that must stay byte-identical to pre-clamp behaviour:
    // both bounds are comfortably inside the band and the span already exceeds kMinSpan.
    const auto r = visibleRange (45, 64);
    REQUIRE (r.lo == 40.0f);
    REQUIRE (r.hi == 69.0f);
}

TEST_CASE ("highway range: a stray out-of-band note cannot stretch the axis", "[highway]")
{
    // Regression for the C-1 floor. Even after the pitch-1 fix removed the marker that
    // caused it, any junk note below the vocal range would reopen the same hole.
    const auto low = visibleRange (2, 64);
    REQUIRE (low.lo == (float) kVocalLow);
    REQUIRE (low.hi == 69.0f);            // the sound end is still padded normally

    const auto high = visibleRange (45, 95);
    REQUIRE (high.lo == 40.0f);
    REQUIRE (high.hi == (float) kVocalHigh);
}

TEST_CASE ("highway range: padding never escapes the band at the extremes", "[highway]")
{
    // Content legitimately at the edge of the RB range: the pad would push past it.
    const auto r = visibleRange (kVocalLow, kVocalHigh);
    REQUIRE (r.lo == (float) kVocalLow);
    REQUIRE (r.hi == (float) kVocalHigh);
}

TEST_CASE ("highway range: a flat chart is widened to the minimum span", "[highway]")
{
    // Charting from scratch against a stem: every placeholder note the same pitch would
    // otherwise collapse the axis to 2*kPitchPad+1 semitones and fold the pitch trail.
    const auto r = visibleRange (48, 48);
    REQUIRE (r.hi - r.lo + 1.0f == (float) kMinSpan);
    REQUIRE (r.lo <= 48.0f);
    REQUIRE (r.hi >= 48.0f);
}

TEST_CASE ("highway range: widening a flat chart at the band edge stays inside the band",
           "[highway]")
{
    // The interesting interaction: the min-span guard runs after the clamp, so it must
    // re-seat rather than push back out of the band it was just clamped into.
    const auto lowEdge = visibleRange (kVocalLow, kVocalLow);
    REQUIRE (lowEdge.lo >= (float) kVocalLow);
    REQUIRE (lowEdge.hi <= (float) kVocalHigh);
    REQUIRE (lowEdge.hi - lowEdge.lo + 1.0f == (float) kMinSpan);

    const auto highEdge = visibleRange (kVocalHigh, kVocalHigh);
    REQUIRE (highEdge.lo >= (float) kVocalLow);
    REQUIRE (highEdge.hi <= (float) kVocalHigh);
    REQUIRE (highEdge.hi - highEdge.lo + 1.0f == (float) kMinSpan);
}

TEST_CASE ("highway range: content entirely outside the band falls back to the band",
           "[highway]")
{
    // Must not produce an inverted axis: drawHighway divides by (hi - lo + 1), so a
    // negative span would mirror every note off-screen instead of merely hiding it.
    const auto r = visibleRange (10, 20);
    REQUIRE (r.lo == (float) kVocalLow);
    REQUIRE (r.hi == (float) kVocalHigh);
    REQUIRE (r.hi > r.lo);
}

TEST_CASE ("highway range: the axis is never inverted for any input pair", "[highway]")
{
    // Cheap exhaustive sweep — the ordering invariant drawHighway depends on.
    for (int lo = 0; lo <= 127; lo += 7)
        for (int hi = lo; hi <= 127; hi += 7)
        {
            const auto r = visibleRange (lo, hi);
            REQUIRE (r.hi > r.lo);
            REQUIRE (r.lo >= (float) kVocalLow);
            REQUIRE (r.hi <= (float) kVocalHigh);
        }
}
