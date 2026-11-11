#pragma once
#include <cstddef>
#include <vector>

// Note markings — which of the highway's per-note labels actually get drawn.
//
// Pure and JUCE-free (header-only, no .cpp) so the unit tests can exercise the yield rule
// without a Graphics context or a loaded chart. drawHighway measures the text (which needs
// JUCE), builds one Candidate per note that wants a label, and delegates the decision here.
//
// THE PROBLEM THIS SOLVES: a label is 12 px tall, while a lane is highwayHeight / (span+1)
// — roughly 29 px on the 12-semitone minimum span, ~15 px on a 24-semitone one. So a label
// covers one to two lanes. In Harmonies mode HARM2 routinely sits one semitone from Main,
// which puts one part's label straight on top of the other part's bar and label. Yielding
// only on an exact pitch match does not cover that; the clearance has to be tested as
// geometry, because how many semitones "too close" means depends on the lane height.

namespace rbmark
{

// Half-open on both axes, matching juce::Rectangle: an edge touching an edge is not an
// overlap. A rect with x2 <= x1 or y2 <= y1 is empty and intersects nothing — which is how
// a caller says "this note occupies no space a label has to avoid" (a talkie's translucent
// full-height column is readable through, so it is passed empty rather than as its bar).
struct Rect
{
    float x1, y1, x2, y2;

    constexpr bool isEmpty() const { return x2 <= x1 || y2 <= y1; }

    constexpr bool intersects (const Rect& o) const
    {
        return ! isEmpty() && ! o.isEmpty()
            && x1 < o.x2 && o.x1 < x2
            && y1 < o.y2 && o.y1 < y2;
    }
};

struct Candidate
{
    Rect label;      // where the text would go
    Rect bar;        // the note's own bar, as an obstacle for lower-priority labels
    int  priority;   // 0 = Main, 1 = HARM2, 2 = HARM3
    int  noteIndex;  // index into chartNotes, carried through for the draw pass
};

// Decides which candidates are drawn. `in` must arrive ordered by priority (0 first), then
// by time within a priority — a stable sort on priority gives that, since the note list is
// already time-ordered.
//
// The policy:
//   - Priority 0 (Main) is accepted unconditionally. Its labels are the ones that existed
//     before this rule did, so they must keep drawing exactly as they always have, overlap
//     with each other included. It also means Solo mode is untouched by this pass.
//   - Anything else is accepted only when its label clears every accepted label and every
//     bar belonging to a strictly higher priority.
//   - A yielded label claims nothing, so HARM3 can take space HARM2 gave up.
//
// Written through an out-param rather than returned so the caller can hand in a persistent
// scratch vector and keep paint allocation-free. O(n^2) in the number of candidates, which
// is the count of *visible* notes across all parts — tens, not thousands.
inline void placeMarkings (const std::vector<Candidate>& in, std::vector<char>& acceptedOut)
{
    acceptedOut.assign (in.size(), 0);

    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const Candidate& c = in[i];

        if (c.priority == 0)
        {
            acceptedOut[i] = 1;
            continue;
        }

        bool clear = true;
        for (std::size_t j = 0; j < i && clear; ++j)
        {
            const Candidate& o = in[j];
            if (acceptedOut[j] && c.label.intersects (o.label)) clear = false;
            if (o.priority < c.priority && c.label.intersects (o.bar)) clear = false;
        }

        acceptedOut[i] = clear ? 1 : 0;
    }
}

} // namespace rbmark
