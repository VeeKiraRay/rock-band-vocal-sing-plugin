#pragma once

// NSDF peak-picking for the MPM pitch detector — the decision layer that turns a
// normalised square difference function into a lag estimate.
//
// Pure functions, deliberately JUCE- and processor-free so the unit tests can build an
// NSDF table by hand (or from a synthetic waveform) and drive the picker directly,
// without pushing audio through processBlock. Same rationale as TakeExport.h — and the
// reason it exists: boundary rejection and the key-threshold walk were previously only
// reachable by hoping a synthesised signal happened to trigger them.
//
// Called once per detection hop on the audio thread: no allocation, no locks, no
// exceptions. The constants below come from the 2026-08 detection-range findings.

namespace rbpitch
{

// Scanned lag range for a given internal detection rate. Shared by all three detectors
// (MPM, YIN, autocorrelation) so the searched pitch band is defined in exactly one place.
//
//   minLag -> highest detectable frequency (detectRate / kMaxFreqHz)
//   maxLag -> lowest  detectable frequency (detectRate / kMinFreqHz), capped by the
//             caller's lag-buffer size.
struct LagBounds { int minLag, maxLag; };

// The band is sized around the Rock Band vocal range, MIDI 36-84 (65.4 Hz - 1046.5 Hz),
// with ~6% headroom at each end. The headroom is not slack: boundary rejection discards
// a peak landing on either bound, so a bound sitting tight against real content rejects
// it outright — measured at 1.01x (fails) vs 1.06x (fine), 2026-08. The previous
// 80/1000 Hz band left the top and bottom of the vocal range undetectable.
constexpr double kMaxFreqHz = 1200.0;  // top of the scanned band (MIDI 84 + ~9%)
constexpr double kMinFreqHz =   60.0;  // bottom of the scanned band (MIDI 36 - ~8%)

// Default strength (relative to the chosen peak) at which the peak near half the chosen
// lag counts as a credible octave rival. A *default*, not policy — the plugin exposes it
// as a setting and passes the user's value through PeakParams; this layer only ever reads
// the parameter. See the octave-rival block in the .cpp for why the evidence is
// one-directional.
//
// The reachable range is bounded above by the key threshold, and the bound is not
// obvious. For the walk to have made an octave-low error at all it must have SKIPPED the
// run at the true period T, i.e. nsdf(T) < keyThresholdRatio x globalMax. When 2T is the
// tallest peak — the usual shape of this failure — that forces
//
//     ratio = nsdf(T) / nsdf(2T)  <  keyThresholdRatio   (0.93)
//
// so a genuine octave error can never score above ~0.93, and a threshold set near there
// catches only the mildest damping. 0.85 was tried first and missed a clear error on real
// material for exactly that reason (2026-08-27).
constexpr float kOctaveRivalRatio = 0.80f;

LagBounds lagBounds (double detectRate, int lagBufCapacity) noexcept;

struct PeakParams
{
    int   minLag;
    int   maxLag;
    float keyThresholdRatio { 0.93f };  // MPM "key maximum": fraction of the global peak
                                        //   a candidate must reach to be chosen
    float clarityThreshold  { 0.80f };  // below this the frame counts as unvoiced

    // Octave-rival search (the "octave guard"). 0 disables it outright — no extra scan
    // runs and both rival fields below keep their defaults, so the guard costs nothing
    // and changes nothing when the user has it switched off.
    float octaveRivalRatio  { 0.0f };
};

struct PeakResult
{
    // Parabolically interpolated lag. < 0 means no trustworthy pitch this frame: no key
    // maximum was found, the chosen peak sat on a scan boundary, or clarity was too low.
    float tau { -1.0f };

    // Height of the chosen NSDF peak (0–1) — MPM's voicing confidence. Stays 0 unless a
    // peak survived both picking and boundary rejection, so a rejected-on-boundary frame
    // publishes no confidence rather than a misleading one.
    float clarity { 0.0f };

    // Octave rival: the peak near tau/2, i.e. the evidence that this estimate is an octave
    // too LOW. Both stay at their defaults unless octaveRivalRatio asked for the search.
    //
    //   octaveTau   — parabolically interpolated lag of that peak, and therefore the lag a
    //                 correcting caller should use instead. < 0 when no credible rival was
    //                 found (including when it was found but fell under the ratio).
    //   octaveRatio — its height relative to the chosen peak, reported whenever a peak was
    //                 located at all, so a caller can show *how* close the call was.
    float octaveTau   { -1.0f };
    float octaveRatio {  0.0f };
};

// nsdf must be readable over [0, maxLag]. Values are the standard NSDF
// (2·acf(τ) / m(τ)), so ~1 at a perfectly periodic lag and negative between peaks.
PeakResult findNsdfPeak (const float* nsdf, const PeakParams& p) noexcept;

} // namespace rbpitch
