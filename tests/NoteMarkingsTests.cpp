#include "NoteMarkings.h"
#include <catch2/catch_test_macros.hpp>

// rbmark::placeMarkings — which per-note highway labels get drawn when parts sit close
// enough that their labels would collide. Pure, so these run without an editor, a chart or
// a Graphics context.
//
// The fixtures below are built the way drawHighway builds them: a bar centred on its
// pitch's lane, and a label sitting immediately above that bar. That geometry has one
// consequence worth stating up front, because it drives most of these cases — a label needs
// kLabelH of clear space *above* its own bar, so the room available to a lower part is the
// gap between the two bars, (k - 1) * laneH + 1 for a k-semitone separation. At k = 1 that
// gap is 1 px at any lane height, which is why a harmony a semitone under Main can never be
// labelled, while at k = 2 it depends on how tall a lane is.

using rbmark::Candidate;
using rbmark::Rect;
using rbmark::placeMarkings;

namespace
{

constexpr float kLabelH = 12.0f;   // the highway's label row height
constexpr float kBarInset = 1.0f;  // drawHighway's (noteH - 1) bar height

// One note, as drawHighway would lay it out. Pitch 60 is the reference lane; higher
// pitches draw further up the screen (y decreases), matching pitchY().
Candidate noteAt (int pitch, float x1, float x2, int priority, float laneH,
                  float labelW = 30.0f)
{
    const float cy   = 400.0f - (float) (pitch - 60) * laneH;
    const float barH = laneH - kBarInset;
    const Rect  bar  { x1, cy - barH * 0.5f, x2, cy + barH * 0.5f };
    const Rect  lbl  { x1, bar.y1 - kLabelH, x1 + labelW, bar.y1 };
    return { lbl, bar, priority, pitch };
}

} // namespace

TEST_CASE ("note markings: Main never yields, even to itself", "[markings]")
{
    // Main labels are the ones that existed before this rule did. Two short consecutive
    // syllables whose labels overlap horizontally is the everyday case in a fast passage,
    // and it has always drawn both — Solo mode must come out of this pass untouched.
    std::vector<Candidate> in {
        noteAt (60,  0.0f, 10.0f, 0, 25.0f),
        noteAt (60, 10.0f, 20.0f, 0, 25.0f),   // label starts 10 px in, 30 px wide
    };
    REQUIRE (in[0].label.intersects (in[1].label));

    std::vector<char> acc;
    placeMarkings (in, acc);

    REQUIRE (acc[0] == 1);
    REQUIRE (acc[1] == 1);
}

TEST_CASE ("note markings: a harmony in unison with Main yields", "[markings]")
{
    // The case the user reported first: HARM2 doubling Main at the same pitch puts the two
    // labels in exactly the same place.
    std::vector<Candidate> in {
        noteAt (60, 0.0f, 100.0f, 0, 25.0f),
        noteAt (60, 0.0f, 100.0f, 1, 25.0f),
    };

    std::vector<char> acc;
    placeMarkings (in, acc);

    REQUIRE (acc[0] == 1);
    REQUIRE (acc[1] == 0);
}

TEST_CASE ("note markings: a harmony a semitone under Main yields at any lane height",
           "[markings]")
{
    // Not a lane-height question: the label wants 12 px above its own bar and the gap
    // between two adjacent lanes' bars is 1 px however tall the lanes are. Pinned because
    // it is the most common Harmonies layout there is, and because it is the case that
    // would silently come back if the rule were ever narrowed to a label-vs-label test.
    for (const float laneH : { 15.0f, 40.0f })
    {
        std::vector<Candidate> in {
            noteAt (60, 0.0f, 100.0f, 0, laneH),
            noteAt (59, 0.0f, 100.0f, 1, laneH),
        };

        std::vector<char> acc;
        placeMarkings (in, acc);

        REQUIRE (acc[0] == 1);
        REQUIRE (acc[1] == 0);
    }
}

TEST_CASE ("note markings: two semitones apart depends on the lane height", "[markings]")
{
    // The load-bearing pair, and the reason this rule is geometry rather than a semitone
    // count: the same two notes at the same interval, resolved differently because the
    // chart's pitch range changed how tall a lane is.
    SECTION ("tall lanes: the gap fits a label")
    {
        std::vector<Candidate> in {
            noteAt (60, 0.0f, 100.0f, 0, 15.0f),
            noteAt (58, 0.0f, 100.0f, 1, 15.0f),
        };

        std::vector<char> acc;
        placeMarkings (in, acc);

        REQUIRE (acc[0] == 1);
        REQUIRE (acc[1] == 1);
    }

    SECTION ("short lanes: the same interval no longer fits")
    {
        std::vector<Candidate> in {
            noteAt (60, 0.0f, 100.0f, 0, 10.0f),
            noteAt (58, 0.0f, 100.0f, 1, 10.0f),
        };

        std::vector<char> acc;
        placeMarkings (in, acc);

        REQUIRE (acc[0] == 1);
        REQUIRE (acc[1] == 0);
    }
}

TEST_CASE ("note markings: a label is blocked by a higher part's bar, not just its label",
           "[markings]")
{
    // Because labels sit above bars, the thing a lower part's label runs into is usually
    // the higher part's *bar* — the two labels are a whole lane apart and never touch. A
    // rule that compared labels only would let this one draw straight across the cyan bar.
    std::vector<Candidate> in {
        noteAt (61, 0.0f, 100.0f, 0, 14.0f),
        noteAt (60, 0.0f, 100.0f, 1, 14.0f),
    };
    REQUIRE (in[1].label.intersects (in[0].bar));
    REQUIRE (! in[1].label.intersects (in[0].label));

    std::vector<char> acc;
    placeMarkings (in, acc);

    REQUIRE (acc[0] == 1);
    REQUIRE (acc[1] == 0);
}

TEST_CASE ("note markings: parts that do not overlap in time both draw", "[markings]")
{
    // Same pitch, different moments. Nothing to resolve — the x extents are disjoint.
    std::vector<Candidate> in {
        noteAt (60,   0.0f,  50.0f, 0, 15.0f),
        noteAt (60, 200.0f, 250.0f, 1, 15.0f),
    };

    std::vector<char> acc;
    placeMarkings (in, acc);

    REQUIRE (acc[0] == 1);
    REQUIRE (acc[1] == 1);
}

TEST_CASE ("note markings: HARM3 yields to an accepted HARM2 label", "[markings]")
{
    // Priority runs the whole way down, not just "Main wins".
    std::vector<Candidate> in {
        noteAt (72, 0.0f, 100.0f, 0, 15.0f),   // Main, an octave up — clear of both
        noteAt (60, 0.0f, 100.0f, 1, 15.0f),
        noteAt (60, 0.0f, 100.0f, 2, 15.0f),
    };

    std::vector<char> acc;
    placeMarkings (in, acc);

    REQUIRE (acc[0] == 1);
    REQUIRE (acc[1] == 1);
    REQUIRE (acc[2] == 0);
}

TEST_CASE ("note markings: a yielded label frees its space for the part below", "[markings]")
{
    // HARM2 is suppressed by Main, so it claims nothing — and HARM3, which is clear of
    // Main, still gets to draw. A rule that reserved space for rejected candidates would
    // lose this label for no reason.
    std::vector<Candidate> in {
        noteAt (60, 0.0f, 100.0f, 0, 15.0f),
        noteAt (60, 0.0f, 100.0f, 1, 15.0f),   // unison with Main -> yields
        noteAt (50, 0.0f, 100.0f, 2, 15.0f),   // ten semitones down -> clear
    };

    std::vector<char> acc;
    placeMarkings (in, acc);

    REQUIRE (acc[0] == 1);
    REQUIRE (acc[1] == 0);
    REQUIRE (acc[2] == 1);
}

TEST_CASE ("note markings: an empty obstacle blocks nothing", "[markings]")
{
    // How a talkie is passed in. Its column is a translucent full-height wash that text
    // reads through fine, so drawHighway hands over an empty bar rect. Handing over the
    // column itself would suppress every harmony label sharing that stretch of time, which
    // is the whole width of the talkie — hence both halves of this test.
    auto harm = noteAt (50, 0.0f, 100.0f, 1, 15.0f);

    SECTION ("a full-height column would swallow the label")
    {
        auto talkie = noteAt (60, 0.0f, 100.0f, 0, 15.0f);
        talkie.bar  = { 0.0f, 0.0f, 100.0f, 800.0f };   // spans the highway

        std::vector<Candidate> in { talkie, harm };
        std::vector<char>      acc;
        placeMarkings (in, acc);

        REQUIRE (acc[1] == 0);
    }

    SECTION ("passed empty, it does not")
    {
        auto talkie = noteAt (60, 0.0f, 100.0f, 0, 15.0f);
        talkie.bar  = { 0.0f, 0.0f, 0.0f, 0.0f };

        std::vector<Candidate> in { talkie, harm };
        std::vector<char>      acc;
        placeMarkings (in, acc);

        REQUIRE (acc[0] == 1);
        REQUIRE (acc[1] == 1);
    }
}

TEST_CASE ("note markings: an empty input is handled", "[markings]")
{
    // Chart-less capture, or every marking turned off.
    std::vector<Candidate> in;
    std::vector<char>      acc { 1, 1, 1 };   // deliberately dirty: assign() must clear it

    placeMarkings (in, acc);

    REQUIRE (acc.empty());
}
