#include "TakeExport.h"
#include <algorithm>
#include <cmath>

// Segmentation pipeline (2026-07 take-export work). Every pass operates on sample times,
// never sample counts — frame spacing is ~16 ms at 60 fps but ~50 ms at the 20 fps
// frame cap, and irregular under dropped frames.

namespace rbtake
{

std::vector<ExportedNote> segmentPitchTrail (const std::vector<TrailPoint>& trail,
                                             const SegmentationParams& p)
{
    struct Segment { double start, end; int pitch; };
    std::vector<Segment> segs;

    // Pass 1+2: split into voiced runs (at gap markers or an over-large sample delta),
    // then hysteresis semitone segmentation within each run. The first sample locks the
    // segment's semitone; samples stay while within splitHysteresis of it. On exit the
    // segment closes at the boundary sample's time and a new one starts there
    // (segments within a run are contiguous). A run ends at its last voiced sample.
    const size_t n = trail.size();
    size_t i = 0;
    while (i < n)
    {
        if (trail[i].pitch < 0.0f) { ++i; continue; }

        size_t runEnd = i;
        while (runEnd + 1 < n && trail[runEnd + 1].pitch >= 0.0f
               && trail[runEnd + 1].time - trail[runEnd].time <= p.maxSampleSpacing)
            ++runEnd;

        double segStart = trail[i].time;
        int    locked   = (int) std::lround (trail[i].pitch);
        for (size_t k = i + 1; k <= runEnd; ++k)
        {
            if (std::abs (trail[k].pitch - (float) locked) > (float) p.splitHysteresis)
            {
                segs.push_back ({ segStart, trail[k].time, locked });
                segStart = trail[k].time;
                locked   = (int) std::lround (trail[k].pitch);
            }
        }
        segs.push_back ({ segStart, trail[runEnd].time, locked });

        i = runEnd + 1;
    }

    // Pass 3: drop segments shorter than minNoteLen (spikes, chatter overshoots;
    // single-sample runs come out zero-length and die here too).
    segs.erase (std::remove_if (segs.begin(), segs.end(),
                                [&p] (const Segment& s) { return s.end - s.start < p.minNoteLen; }),
                segs.end());

    // Pass 4: merge adjacent same-pitch segments whose gap fits in maxGapMerge —
    // re-joins a held note fragmented by a dropped spike or an unvoiced consonant
    // blip. Merging only lengthens, so no re-drop pass is needed.
    std::vector<Segment> merged;
    for (const auto& s : segs)
    {
        if (! merged.empty() && merged.back().pitch == s.pitch
            && s.start - merged.back().end <= p.maxGapMerge)
            merged.back().end = s.end;
        else
            merged.push_back (s);
    }

    // Pass 5+6: clamp into the safe band, then enforce the slight spacing where
    // consecutive notes butt (trim the earlier note's end; survivors are ≥ minNoteLen
    // so a noteSpacing trim never degenerates them). Natural gaps stay as-detected.
    std::vector<ExportedNote> notes;
    notes.reserve (merged.size());
    for (const auto& s : merged)
        notes.push_back ({ s.start, s.end,
                           std::clamp (s.pitch, p.minPitch, p.maxPitch) });

    for (size_t k = 1; k < notes.size(); ++k)
        if (notes[k].startTime - notes[k - 1].endTime < p.noteSpacing)
            notes[k - 1].endTime = std::max (notes[k - 1].startTime + 0.010,
                                             notes[k].startTime - p.noteSpacing);

    return notes;
}

} // namespace rbtake
