#include "EditorVisuals.h"
#include "HighwayRange.h"   // kVocalLow / kVocalHigh — the band these names label
#include <catch2/catch_test_macros.hpp>

// noteNameFor / pitchMarkFor — the octave numbering every note name in the plugin goes
// through. Middle C is C3 (the Yamaha/Roland convention), because these names are read
// beside REAPER's MIDI editor and have to agree with it.
//
// Worth pinning despite being one constant: the plugin shipped for a while naming the
// vocal band C2-C6 under scientific pitch notation, and nothing caught it until the pitch
// was put on screen next to the chart. An off-by-one octave is invisible in isolation —
// every name still looks like a plausible note — so it only shows up against a reference,
// which is exactly what these cases are.

TEST_CASE ("note naming: the RB vocal band reads C1-C5", "[naming]")
{
    // The two numbers that matter, since rbhighway clamps the axis to this band.
    REQUIRE (noteNameFor (rbhighway::kVocalLow)  == "C1");   // 36
    REQUIRE (noteNameFor (rbhighway::kVocalHigh) == "C5");   // 84
}

TEST_CASE ("note naming: middle C is C3", "[naming]")
{
    // The one-line statement of the convention. If this says C4, every other name in the
    // plugin is an octave out and the band above reads C2-C6.
    REQUIRE (noteNameFor (60) == "C3");
}

TEST_CASE ("note naming: letters and accidentals", "[naming]")
{
    REQUIRE (noteNameFor (61) == "C#3");
    REQUIRE (noteNameFor (71) == "B3");    // last of the octave
    REQUIRE (noteNameFor (72) == "C4");    // rolls over
}

TEST_CASE ("note naming: the extremes stay in range and stay sane", "[naming]")
{
    // MIDI 0 is below anything singable, but the pitch axis and the note markings both
    // format whatever they are handed, and a marker pitch of 0 or 1 is a real thing in RB
    // charts (kRangeShiftPitch, kLyricShiftPitch).
    REQUIRE (noteNameFor (0)   == "C-2");
    REQUIRE (noteNameFor (127) == "G8");

    // Out-of-range input is clamped rather than indexing off the end of the table.
    REQUIRE (noteNameFor (-5)  == noteNameFor (0));
    REQUIRE (noteNameFor (999) == noteNameFor (127));
}

TEST_CASE ("note naming: the highway's marking form brackets the same name", "[naming]")
{
    // The markings and the pitch axis must never disagree about a note's name — they are
    // read against each other on the same screen.
    REQUIRE (pitchMarkFor (60) == "[C3]");
    REQUIRE (pitchMarkFor (rbhighway::kVocalLow) == "[" + noteNameFor (rbhighway::kVocalLow) + "]");
}
