#include "PercussionHits.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

// rbperc — the vocal percussion rules: hit windows, what counts as a tap, the overlap
// warning, and what the highway and stats light show. Pure, so these run without an
// editor, a chart or a Graphics context.

using namespace rbperc;

namespace
{
    std::vector<Note> notesAt (std::initializer_list<double> starts)
    {
        std::vector<Note> v;
        for (double s : starts) v.push_back ({ s, s + 0.06 });
        prepare (v);
        return v;
    }

    // Replays a gate history through the same edge rule updateNoteHits() uses and returns
    // the index each tap landed on (-1 = outside every window). `gate` is one entry per hop.
    std::vector<int> tapsOn (const std::vector<Note>& notes, double firstHop, double hopSec,
                             std::initializer_list<bool> gate, bool startOpen = false)
    {
        std::vector<int> landed;
        bool   prev = startOpen;
        double t    = firstHop;
        for (bool open : gate)
        {
            if (isTap (prev, open)) landed.push_back (noteAt (notes, t));
            prev = open;
            t   += hopSec;
        }
        return landed;
    }
}

// ─── Windows ─────────────────────────────────────────────────────────────────

TEST_CASE ("percussion: an isolated note gets the full window either side", "[percussion]")
{
    const auto n = notesAt ({ 10.0 });
    REQUIRE (n[0].window.lo == Catch::Approx (10.0 - kEarlySec));
    REQUIRE (n[0].window.hi == Catch::Approx (10.0 + kLateSec));
}

TEST_CASE ("percussion: close notes split the gap at the midpoint", "[percussion]")
{
    // 137 ms apart: the tightest spacing seen on official charts. Unclipped, the two
    // windows would overlap by 63 ms and one tap could score both.
    const auto n = notesAt ({ 10.0, 10.137 });
    REQUIRE (n[0].window.hi == Catch::Approx (10.0685));
    REQUIRE (n[1].window.lo == Catch::Approx (10.0685));
    REQUIRE (n[0].window.lo == Catch::Approx (10.0 - kEarlySec));
    REQUIRE (n[1].window.hi == Catch::Approx (10.137 + kLateSec));
}

TEST_CASE ("percussion: prepare sorts and folds a note charted twice", "[percussion]")
{
    std::vector<Note> v { { 12.0, 12.05 }, { 10.0, 10.05 }, { 10.0002, 10.05 } };
    prepare (v);
    REQUIRE (v.size() == 2);
    REQUIRE (v[0].startTime == Catch::Approx (10.0));
    REQUIRE (v[1].startTime == Catch::Approx (12.0));
    // A duplicate must not shrink its twin's window to nothing.
    REQUIRE (v[0].window.hi == Catch::Approx (10.0 + kLateSec));
}

TEST_CASE ("percussion: noteAt finds the window holding a time", "[percussion]")
{
    const auto n = notesAt ({ 10.0, 10.137, 20.0 });
    REQUIRE (noteAt (n, 9.95)   == 0);
    REQUIRE (noteAt (n, 10.06)  == 0);
    REQUIRE (noteAt (n, 10.0685) == 1);   // midpoint belongs to the later note
    REQUIRE (noteAt (n, 10.2)   == 1);
    REQUIRE (noteAt (n, 10.0 - kEarlySec - 0.01) == -1);   // before the first window
    REQUIRE (noteAt (n, 15.0)   == -1);   // between windows
    REQUIRE (noteAt (n, 20.0 + kLateSec) == -1);   // window is half-open at the end
    REQUIRE (noteAt ({}, 1.0)   == -1);
}

// ─── Taps ────────────────────────────────────────────────────────────────────

TEST_CASE ("percussion: a tap inside the window lands on the note", "[percussion]")
{
    const auto n = notesAt ({ 10.0 });
    // Hops every ~10.7 ms; the gate opens on the 4th hop (t = 10.002).
    const auto landed = tapsOn (n, 9.97, 0.0107, { false, false, false, true, true, false });
    REQUIRE (landed == std::vector<int> { 0 });
}

TEST_CASE ("percussion: a tap just outside the window misses", "[percussion]")
{
    const auto n = notesAt ({ 10.0 });
    const double early = 10.0 - kEarlySec - 0.02;   // gate opens 10 ms before the window
    const double late  = 10.0 + kLateSec;            // gate opens 10 ms after it closes
    REQUIRE (tapsOn (n, early, 0.01, { false, true }) == std::vector<int> { -1 });
    REQUIRE (tapsOn (n, late,  0.01, { false, true }) == std::vector<int> { -1 });
    REQUIRE (noteAt (n, 10.0 + kLateSec) == -1);                                       // end is half-open
}

TEST_CASE ("percussion: a gate already open across the window is not a tap", "[percussion]")
{
    // Singing straight through the window — or a noise floor above the gate — never
    // closes the gate, so there is no edge and nothing scores.
    const auto n = notesAt ({ 10.0 });
    REQUIRE (tapsOn (n, 10.0 - kEarlySec - 0.05, 0.03, { true, true, true, true, true, true, true, true,
                                      true, true, true, true, true, true, true, true },
                     true).empty());
}

TEST_CASE ("percussion: sustained noise yields one edge at most", "[percussion]")
{
    const auto n = notesAt ({ 10.0 });
    const auto landed = tapsOn (n, 9.95, 0.01, { false, true, true, true, true, true, true });
    REQUIRE (landed.size() == 1);
}

TEST_CASE ("percussion: one tap between two close notes scores only one", "[percussion]")
{
    const auto n = notesAt ({ 10.0, 10.137 });
    // Gate opens once at 10.06 and stays open past the second note.
    const auto landed = tapsOn (n, 10.05, 0.01, { false, true, true, true, true, true, true, true, true });
    REQUIRE (landed == std::vector<int> { 0 });
}

TEST_CASE ("percussion: isTap is the closed-to-open edge only", "[percussion]")
{
    REQUIRE (isTap (false, true));
    REQUIRE_FALSE (isTap (true, true));
    REQUIRE_FALSE (isTap (true, false));
    REQUIRE_FALSE (isTap (false, false));
}

// ─── Overlap warning ─────────────────────────────────────────────────────────

TEST_CASE ("percussion: a sung note touching the window flags the note", "[percussion]")
{
    auto n = notesAt ({ 10.0 });
    markOverlaps (n, { { 9.5, 9.95 } });          // ends inside the early half
    REQUIRE (n[0].overlapsSung);

    markOverlaps (n, { { 10.05, 10.5 } });        // starts inside the late half
    REQUIRE (n[0].overlapsSung);
}

TEST_CASE ("percussion: a sung note clear of the window does not flag", "[percussion]")
{
    // Official charts keep singing at least this far away; the warning must stay silent
    // on them.
    auto n = notesAt ({ 10.0 });
    markOverlaps (n, { { 9.0,  10.0 - kEarlySec - 0.001 },
                       { 10.0 + kLateSec + 0.001, 11.0 } });
    REQUIRE_FALSE (n[0].overlapsSung);
}

TEST_CASE ("percussion: a long earlier note still sounding over the window flags", "[percussion]")
{
    // Sorted by start, a long sustain can begin well before shorter notes that end sooner.
    // The running maximum of end times is what catches it.
    auto n = notesAt ({ 10.0 });
    markOverlaps (n, { { 5.0, 10.5 }, { 6.0, 6.5 }, { 7.0, 7.5 } });
    REQUIRE (n[0].overlapsSung);
}

TEST_CASE ("percussion: a harmony part singing over the lead's percussion flags", "[percussion]")
{
    // HARM1 is clear of the percussion, HARM2 sings across it — the case reported to make
    // percussion misbehave in game. The loader passes every part's notes, merged by start.
    auto n = notesAt ({ 10.0, 12.0 });
    markOverlaps (n, { { 8.0, 9.5 }, { 9.9, 10.4 } /* HARM2 */, { 11.0, 11.5 } });
    REQUIRE (n[0].overlapsSung);
    REQUIRE_FALSE (n[1].overlapsSung);
}

// ─── Fill and stats light ────────────────────────────────────────────────────

TEST_CASE ("percussion: fill grows from the hit while playing, full when stopped", "[percussion]")
{
    const Hit h { Result::Hit, 10.0 };
    REQUIRE (fillProgress (h, 9.99, true) == 0.0f);                       // not reached yet
    REQUIRE (fillProgress (h, 10.0 + kGrowSec * 0.5, true) == Catch::Approx (0.5f));
    REQUIRE (fillProgress (h, 11.0, true) == 1.0f);
    REQUIRE (fillProgress (h, 0.0, false) == 1.0f);                       // stopped, cursor anywhere

    REQUIRE (fillProgress ({ Result::Miss, 0.0 },    11.0, true)  == 0.0f);
    REQUIRE (fillProgress ({ Result::Pending, 0.0 }, 11.0, false) == 0.0f);
}

TEST_CASE ("percussion: light is pending in the window, then shows the result", "[percussion]")
{
    const auto n = notesAt ({ 10.0 });
    std::vector<Hit> hits (1);

    REQUIRE (lightAt (n, hits, 10.0 - kEarlySec - 0.1, true) == Light::None);
    REQUIRE (lightAt (n, hits, 9.95, true) == Light::Pending);

    hits[0] = { Result::Hit, 10.03 };
    REQUIRE (lightAt (n, hits, 10.02, true) == Light::Pending);   // playhead not at the tap yet
    REQUIRE (lightAt (n, hits, 10.05, true) == Light::Hit);
    REQUIRE (lightAt (n, hits, 10.0 + kLateSec + kLingerSec - 0.01, true) == Light::Hit);
    REQUIRE (lightAt (n, hits, 10.0 + kLateSec + kLingerSec + 0.01, true) == Light::None);

    hits[0] = { Result::Miss, 0.0 };
    // Scoring can resolve a miss before the picture reaches the window end (playback mode
    // runs detection a calibration ahead); the light must not say MISS while the window is
    // still on screen as open.
    REQUIRE (lightAt (n, hits, 10.05, true) == Light::Pending);
    REQUIRE (lightAt (n, hits, 10.0 + kLateSec + 0.05, true) == Light::Miss);
}

TEST_CASE ("percussion: light shows the stored result while stopped", "[percussion]")
{
    const auto n = notesAt ({ 10.0 });
    const std::vector<Hit> hits { { Result::Hit, 10.08 } };
    // Cursor parked earlier in the window than the tap: stopped review still says hit.
    REQUIRE (lightAt (n, hits, 9.95, false) == Light::Hit);
}

// ─── Reload remap ────────────────────────────────────────────────────────────

TEST_CASE ("percussion: results survive a reload and follow their notes", "[percussion]")
{
    std::vector<double> oldStarts { 1.0, 2.0, 3.0 };
    std::vector<Hit>    oldHits   { { Result::Hit, 1.01 }, { Result::Miss, 0.0 }, { Result::Hit, 3.02 } };

    // Note at 2.0 deleted, one inserted at 2.5, a ULP-level drift on 3.0.
    const std::vector<double> newStarts { 1.0, 2.5, 3.0 + 1e-9 };
    const auto out = remapByStart (oldStarts, oldHits, newStarts);

    REQUIRE (out.size() == 3);
    REQUIRE (out[0].result == Result::Hit);
    REQUIRE (out[1].result == Result::Pending);   // new note starts fresh
    REQUIRE (out[2].result == Result::Hit);
    REQUIRE (out[2].time == Catch::Approx (3.02));
}
