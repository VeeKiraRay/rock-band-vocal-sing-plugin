#include "TakeExport.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>

// Unit tests for the take-export segmenter (segmentPitchTrail). Synthetic trails
// mimic what updateNoteHits() captures: time-ascending {time, fractional pitch}
// samples, pitch < 0 marking gaps, spacing ~16 ms at 60 fps (~50 ms at the 20 fps
// frame cap, irregular under drops).

using rbtake::TrailPoint;
using rbtake::ExportedNote;
using rbtake::segmentPitchTrail;
using Catch::Approx;

namespace
{
    // Append samples t0, t0+dt, ... up to and including t1 (index-based so no
    // floating-point drift), with pitch taken from pitchAt(t).
    template <typename Fn>
    void addContour (std::vector<TrailPoint>& tr, double t0, double t1, double dt, Fn pitchAt)
    {
        for (int i = 0;; ++i)
        {
            const double t = t0 + i * dt;
            if (t > t1 + 1e-9)
                break;
            tr.push_back ({ t, (float) pitchAt (t) });
        }
    }

    void addTone (std::vector<TrailPoint>& tr, double t0, double t1, double dt, float pitch)
    {
        addContour (tr, t0, t1, dt, [pitch] (double) { return pitch; });
    }
}

TEST_CASE ("takeexport: empty and gap-only trails produce no notes", "[takeexport]")
{
    REQUIRE (segmentPitchTrail ({}).empty());

    std::vector<TrailPoint> gapsOnly { { 0.0, -1.0f }, { 0.5, -1.0f }, { 1.0, -1.0f } };
    REQUIRE (segmentPitchTrail (gapsOnly).empty());
}

TEST_CASE ("takeexport: steady tone becomes one note spanning its samples", "[takeexport]")
{
    std::vector<TrailPoint> tr;
    addTone (tr, 0.0, 1.0, 0.016, 69.0f);   // last sample lands at 0.992

    const auto notes = segmentPitchTrail (tr);
    REQUIRE (notes.size() == 1);
    REQUIRE (notes[0].pitch == 69);
    REQUIRE (notes[0].startTime == Approx (0.0));
    REQUIRE (notes[0].endTime   == Approx (0.992));
}

TEST_CASE ("takeexport: vibrato within hysteresis stays one note", "[takeexport]")
{
    std::vector<TrailPoint> tr;
    addContour (tr, 0.0, 1.0, 0.016,
                [] (double t) { return 69.0 + 0.4 * std::sin (2.0 * 3.14159265 * 6.0 * t); });

    const auto notes = segmentPitchTrail (tr);
    REQUIRE (notes.size() == 1);
    REQUIRE (notes[0].pitch == 69);
}

TEST_CASE ("takeexport: chatter around a rounding boundary does not split", "[takeexport]")
{
    // 68.4/68.6 alternating straddles the 68/69 lround boundary; the first sample
    // locks 68 and 0.6 <= splitHysteresis keeps every sample in the segment.
    std::vector<TrailPoint> tr;
    bool high = true;                            // toggles before returning → 68.4 first
    addContour (tr, 0.0, 1.0, 0.016,
                [&high] (double) { high = ! high; return high ? 68.6 : 68.4; });

    const auto notes = segmentPitchTrail (tr);
    REQUIRE (notes.size() == 1);
    REQUIRE (notes[0].pitch == 68);
}

TEST_CASE ("takeexport: a clean semitone step splits into two spaced notes", "[takeexport]")
{
    std::vector<TrailPoint> tr;
    addTone (tr, 0.0,   0.484, 0.016, 69.0f);
    addTone (tr, 0.500, 1.000, 0.016, 71.0f);   // still one run (16 ms delta at the join)

    const auto notes = segmentPitchTrail (tr);
    REQUIRE (notes.size() == 2);
    REQUIRE (notes[0].pitch == 69);
    REQUIRE (notes[1].pitch == 71);
    REQUIRE (notes[1].startTime == Approx (0.5));
    // The segments were contiguous at 0.5; the spacing pass trims the earlier end.
    REQUIRE (notes[0].endTime == Approx (0.5 - 0.020));
}

TEST_CASE ("takeexport: a short unvoiced gap inside a held note is bridged", "[takeexport]")
{
    std::vector<TrailPoint> tr;
    addTone (tr, 0.0, 0.288, 0.016, 69.0f);
    tr.push_back ({ 0.304, -1.0f });            // 32 ms unvoiced blip (consonant)
    addTone (tr, 0.320, 0.608, 0.016, 69.0f);

    const auto notes = segmentPitchTrail (tr);
    REQUIRE (notes.size() == 1);
    REQUIRE (notes[0].pitch == 69);
    REQUIRE (notes[0].startTime == Approx (0.0));
    REQUIRE (notes[0].endTime   == Approx (0.608));
}

TEST_CASE ("takeexport: a short spike is dropped and the run re-merged", "[takeexport]")
{
    std::vector<TrailPoint> tr;
    addTone (tr, 0.0,   0.480, 0.016, 69.0f);
    addTone (tr, 0.496, 0.528, 0.016, 81.0f);   // 48 ms octave-error spike
    addTone (tr, 0.544, 1.000, 0.016, 69.0f);

    const auto notes = segmentPitchTrail (tr);
    REQUIRE (notes.size() == 1);
    REQUIRE (notes[0].pitch == 69);
    REQUIRE (notes[0].startTime == Approx (0.0));
    REQUIRE (notes[0].endTime   == Approx (0.992));
}

TEST_CASE ("takeexport: an isolated segment under the minimum length is dropped", "[takeexport]")
{
    std::vector<TrailPoint> tr;
    addTone (tr, 0.0, 0.080, 0.016, 69.0f);     // 80 ms < 120 ms minimum

    REQUIRE (segmentPitchTrail (tr).empty());
}

TEST_CASE ("takeexport: irregular sample spacing yields the same notes", "[takeexport]")
{
    // Same contour (69 until t=1.0, then 71) sampled regularly and with mixed
    // 16/50/90 ms deltas; both grids contain the boundary sample at exactly 1.0.
    // Segment boundaries are decided by sample times, not counts, so the note
    // structure must match.
    const auto pitchAt = [] (double t) { return t < 1.0 ? 69.0 : 71.0; };

    std::vector<TrailPoint> regular;
    addContour (regular, 0.0, 2.0, 0.020, pitchAt);

    std::vector<TrailPoint> irregular;
    {
        const double deltas[] = { 0.016, 0.050, 0.090 };
        int i = 0;
        for (double t = 0.0; t < 1.0; t += deltas[i++ % 3])
            irregular.push_back ({ t, (float) pitchAt (t) });
        for (double t = 1.0; t <= 2.0 + 1e-9; t += deltas[i++ % 3])
            irregular.push_back ({ t, (float) pitchAt (t) });
    }

    const auto a = segmentPitchTrail (regular);
    const auto b = segmentPitchTrail (irregular);
    REQUIRE (a.size() == 2);
    REQUIRE (b.size() == 2);
    for (const auto* notes : { &a, &b })
    {
        REQUIRE ((*notes)[0].pitch == 69);
        REQUIRE ((*notes)[1].pitch == 71);
        REQUIRE ((*notes)[0].startTime == Approx (0.0));
        REQUIRE ((*notes)[0].endTime   == Approx (1.0 - 0.020));   // spacing trim
        REQUIRE ((*notes)[1].startTime == Approx (1.0));
        REQUIRE ((*notes)[1].endTime >= 1.9);
    }
}

TEST_CASE ("takeexport: pitches clamp into the safe band 1..95", "[takeexport]")
{
    std::vector<TrailPoint> high, low;
    addTone (high, 0.0, 1.0, 0.016, 100.3f);
    addTone (low,  0.0, 1.0, 0.016, 0.2f);

    const auto hn = segmentPitchTrail (high);
    const auto ln = segmentPitchTrail (low);
    REQUIRE (hn.size() == 1);
    REQUIRE (hn[0].pitch == 95);
    REQUIRE (ln.size() == 1);
    REQUIRE (ln[0].pitch == 1);
}

TEST_CASE ("takeexport: absolute project time is preserved", "[takeexport]")
{
    std::vector<TrailPoint> base, offset;
    for (auto* tr : { &base, &offset })
    {
        const double t0 = (tr == &offset) ? 120.0 : 0.0;
        addTone (*tr, t0 + 0.0,   t0 + 0.484, 0.016, 69.0f);
        addTone (*tr, t0 + 0.500, t0 + 1.000, 0.016, 71.0f);
    }

    const auto a = segmentPitchTrail (base);
    const auto b = segmentPitchTrail (offset);
    REQUIRE (a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
    {
        REQUIRE (b[i].startTime == Approx (a[i].startTime + 120.0));
        REQUIRE (b[i].endTime   == Approx (a[i].endTime   + 120.0));
        REQUIRE (b[i].pitch == a[i].pitch);
    }
}
