#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Vocal percussion — the rules for when a percussion note counts as hit, and how it shows.
//
// Pure and JUCE-free (header-only, no .cpp) so the unit tests can exercise every decision
// without an editor, a chart or a Graphics context. The chart loader prepares the notes,
// updateNoteHits() feeds it detection samples, and drawHighway / drawStats ask it what to
// show.
//
// A percussion note is hit by tapping the mic, so a hit is "the gate opened inside the
// note's window" — no pitch involved. Only the two extremes exist: hit or miss.

namespace rbperc
{

// Hit window around each note's start. Fixed, not taken from the MIDI length: charts write
// that length as an arbitrary short stub (most often a 32nd, sometimes anything up to a
// quarter), so it says nothing about timing. 100 ms each side. It was widened to 200 ms
// once when taps felt hard to land, but the cause turned out to be taps being dropped
// before the check (see the hop loop in updateNoteHits), not the window; with that fixed,
// 100 ms hits reliably. Where notes are close, the midpoint clipping in prepare() narrows it
// further, so the width only matters for isolated notes.
constexpr double kEarlySec = 0.100;
constexpr double kLateSec  = 0.100;

// How long the stats light keeps showing a result after the window closes. The window is
// short and a tap usually lands late in it, so without this the result would flash for a
// frame and be gone.
constexpr double kLingerSec = 0.350;

// How long after a window closes before an untapped note is called a miss. Hops reach the
// editor late - dated at their window centre, delivered per audio block, drained per UI
// frame - so a tap near the end of a window can arrive a block or two after the window has
// closed on the frame clock. Comfortably above that, and well inside kLingerSec so the
// stats light still shows the miss.
constexpr double kResolveLagSec = 0.150;

// How long the hit fill takes to grow from the centre to the ring.
constexpr double kGrowSec = 0.120;

// Two percussion notes closer than this are one note charted twice (overlapping MIDI items,
// a duplicated paste). Far below any real spacing, far above serialisation drift.
constexpr double kSameNoteSec = 0.001;

// Half-open, so a tap exactly on the midpoint between two notes belongs to the later one.
struct Window
{
    double lo { 0.0 }, hi { 0.0 };
    bool contains (double t) const noexcept { return t >= lo && t < hi; }
};

struct Note
{
    double startTime { 0.0 };
    double endTime   { 0.0 };      // kept for completeness; the window is what scores
    Window window    {};           // filled by prepare()
    bool   overlapsSung { false }; // filled by markOverlaps()
};

// A sung note's extent, for the overlap check. Talkies count: the singer is making sound
// either way, which keeps the gate open just the same.
struct Span { double start, end; };

enum class Result { Pending, Hit, Miss };

// One per note, index-aligned with the note list. `time` is the hit's detection-timeline
// time, meaningful only for Result::Hit.
struct Hit
{
    Result result { Result::Pending };
    double time   { 0.0 };
};

// Sorts, folds duplicates and assigns each note its window. Where two notes sit closer
// than the two half-windows, each window is clipped at the midpoint between them so one tap
// can never score both — real charts space percussion as tightly as ~137 ms, so this is the
// normal case at speed, not a defensive one.
inline void prepare (std::vector<Note>& notes)
{
    std::sort (notes.begin(), notes.end(),
               [] (const Note& a, const Note& b) { return a.startTime < b.startTime; });
    notes.erase (std::unique (notes.begin(), notes.end(),
                              [] (const Note& a, const Note& b)
                              { return std::abs (a.startTime - b.startTime) < kSameNoteSec; }),
                 notes.end());

    for (size_t i = 0; i < notes.size(); ++i)
    {
        const double s = notes[i].startTime;
        double lo = s - kEarlySec;
        double hi = s + kLateSec;
        if (i > 0)                lo = std::max (lo, 0.5 * (notes[i - 1].startTime + s));
        if (i + 1 < notes.size()) hi = std::min (hi, 0.5 * (s + notes[i + 1].startTime));
        notes[i].window = { lo, hi };
    }
}

// Flags every note whose window intersects a sung note. Official charts never put singing
// within 200 ms of percussion (wider than the window, so this holds with margin), so this only ever fires on an authoring problem — which is
// what it is for: the tool shows how the chart plays in game, and percussion under singing
// is reported to misbehave there. `sung` must be sorted by start.
//
// O((N + M) log M): a note overlaps iff some sung span starting before its window ends
// also ends after its window starts, so a running maximum of end times answers it.
inline void markOverlaps (std::vector<Note>& notes, const std::vector<Span>& sung)
{
    std::vector<double> maxEndSoFar (sung.size());
    double running = -1.0e300;
    for (size_t k = 0; k < sung.size(); ++k)
        maxEndSoFar[k] = running = std::max (running, sung[k].end);

    for (auto& n : notes)
    {
        const auto firstAfter = std::lower_bound (sung.begin(), sung.end(), n.window.hi,
            [] (const Span& s, double v) { return s.start < v; });
        const auto count = (size_t) (firstAfter - sung.begin());
        n.overlapsSung = count > 0 && maxEndSoFar[count - 1] > n.window.lo;
    }
}

// Index of the note whose window holds t, or -1. Windows are disjoint and ascending.
inline int noteAt (const std::vector<Note>& notes, double t)
{
    const auto it = std::upper_bound (notes.begin(), notes.end(), t,
        [] (double v, const Note& n) { return v < n.window.lo; });
    if (it == notes.begin()) return -1;
    const auto idx = (size_t) (it - notes.begin()) - 1;
    return notes[idx].window.contains (t) ? (int) idx : -1;
}

// A tap is the gate going from closed to open. Not "open anywhere in the window": a sung
// tail running into the window, or a mic noise floor sitting just above the gate, would
// otherwise score every percussion note.
inline bool isTap (bool prevAboveGate, bool aboveGate) noexcept
{
    return aboveGate && ! prevAboveGate;
}

// How far the hit fill has grown, 0..1, at drawing time t. Driven by drawing time so the
// fill starts as the playhead reaches the tap — which in playback mode is when it is heard,
// not when it was analysed a calibration earlier. Shown full while stopped, so a reviewed
// pass stays visible wherever the cursor was left.
inline float fillProgress (const Hit& h, double t, bool playing) noexcept
{
    if (h.result != Result::Hit) return 0.0f;
    if (! playing)               return 1.0f;
    const double p = (t - h.time) / kGrowSec;
    return (float) std::clamp (p, 0.0, 1.0);
}

enum class Light { None, Pending, Hit, Miss };

// What the stats light says about percussion at drawing time t. Pending ("---") while a
// window is open and untapped — a tap still to come is not a miss — and also in the brief
// gap before a miss is resolved, since scoring runs on the detection timeline and may
// trail the picture by the input calibration.
inline Light lightAt (const std::vector<Note>& notes, const std::vector<Hit>& hits,
                      double t, bool playing)
{
    const auto it = std::upper_bound (notes.begin(), notes.end(), t,
        [] (double v, const Note& n) { return v < n.window.lo; });
    if (it == notes.begin()) return Light::None;

    const auto  idx = (size_t) (it - notes.begin()) - 1;
    const auto& w   = notes[idx].window;
    if (t >= w.hi + kLingerSec) return Light::None;
    if (idx >= hits.size())     return Light::Pending;

    const Hit& h = hits[idx];
    if (h.result == Result::Hit && (! playing || t >= h.time)) return Light::Hit;
    if (h.result == Result::Miss && t >= w.hi)                  return Light::Miss;
    return Light::Pending;
}

// Carries per-note state across a chart reload by matching start times, so an edit
// elsewhere in the project does not wipe the review. Unmatched notes start fresh. Both
// start lists must be sorted.
template <typename T>
std::vector<T> remapByStart (const std::vector<double>& oldStarts, std::vector<T>& oldValues,
                             const std::vector<double>& newStarts)
{
    // Same tolerance as the note-hit remap: REAPER's PPQ-to-time recompute drifts by ULPs
    // whenever it rewrites a take; 0.1 ms is far above that and far below any real edit.
    constexpr double kMatchEps = 1.0e-4;

    std::vector<T> out (newStarts.size());
    size_t j = 0;
    for (size_t i = 0; i < newStarts.size(); ++i)
    {
        while (j < oldStarts.size() && oldStarts[j] < newStarts[i] - kMatchEps) ++j;
        if (j < oldStarts.size() && j < oldValues.size()
            && std::abs (oldStarts[j] - newStarts[i]) <= kMatchEps)
            out[i] = std::move (oldValues[j++]);
    }
    return out;
}

} // namespace rbperc
