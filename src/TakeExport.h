#pragma once
#include <vector>

// Take export — segment the sung pitch trail into discrete semitone notes.
//
// Pure functions, deliberately JUCE- and processor-free so the unit tests can include
// this header and link TakeExport.cpp without constructing a plugin. The editor's
// pitchTrail is a std::vector<TrailPoint> (via the TrailSample alias in
// PluginEditor.h), so it passes straight into segmentPitchTrail with no copy.
// Design + constant rationale: _upcoming/TAKE_EXPORT.md.

namespace rbtake
{

// One trail sample: absolute project time (s) + raw fractional MIDI pitch.
// pitch < 0 marks a gap (the singer went unvoiced).
// The octave guard's evidence (octaveRatio, 0-1) and its persistence-filtered decision
// (octaveDoubt) for this sample. Segmentation ignores both, but the editor's trail and
// marker history are TrailPoints, so carrying them here is what lets the highway colour a
// doubtful stretch and the stats chip stay pinned to the *heard* sample rather than the
// newest analysed one.
struct TrailPoint   { double time; float pitch;
                      float octaveRatio { 0.0f }; bool octaveDoubt { false }; };

// One exported note: absolute project times (s), integer MIDI pitch.
struct ExportedNote { double startTime, endTime; int pitch; };

// Fixed v1 constants — kept in one struct so a later Settings exposure is cheap.
struct SegmentationParams
{
    double minNoteLen       = 0.120;  // s — drop shorter segments (16th @120 BPM = 125 ms;
                                      //     detection spikes are 16–48 ms)
    double maxGapMerge      = 0.080;  // s — bridge unvoiced blips (consonants) between
                                      //     same-pitch segments
    double noteSpacing      = 0.020;  // s — min gap enforced when consecutive notes butt
    double maxSampleSpacing = 0.250;  // s — larger delta between voiced samples counts as
                                      //     a gap (defensive vs dropped-frame bursts)
    double splitHysteresis  = 0.65;   // semitones — |pitch − locked| must exceed this to
                                      //     start a new note (vibrato ±0.3–0.7 st holds;
                                      //     a real semitone step, 1.0, splits)
    // Safe singable band [1, 95]: same rule as the note-drag editor — nothing at the
    // Range Shift pitch (0) or at/above the first reserved marker pitch (96) may be
    // written. Hardcoded because this header stays processor-free.
    int    minPitch = 1, maxPitch = 95;
};

// trail must be time-ascending (pitchTrail is by construction). Output is
// time-ascending, non-overlapping notes; empty when nothing survives filtering.
std::vector<ExportedNote> segmentPitchTrail (const std::vector<TrailPoint>& trail,
                                             const SegmentationParams& p = {});

} // namespace rbtake
