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
// exceptions. Analysis and rationale: _upcoming/PITCH_DETECTION_FINDINGS.md.

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
// it outright — measured at 1.01x (fails) vs 1.06x (fine) in
// _upcoming/PITCH_DETECTION_FINDINGS.md §6. The previous 80/1000 Hz band left the top
// and bottom of the vocal range undetectable.
constexpr double kMaxFreqHz = 1200.0;  // top of the scanned band (MIDI 84 + ~9%)
constexpr double kMinFreqHz =   60.0;  // bottom of the scanned band (MIDI 36 - ~8%)

LagBounds lagBounds (double detectRate, int lagBufCapacity) noexcept;

struct PeakParams
{
    int   minLag;
    int   maxLag;
    float keyThresholdRatio { 0.93f };  // MPM "key maximum": fraction of the global peak
                                        //   a candidate must reach to be chosen
    float clarityThreshold  { 0.80f };  // below this the frame counts as unvoiced
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
};

// nsdf must be readable over [0, maxLag]. Values are the standard NSDF
// (2·acf(τ) / m(τ)), so ~1 at a perfectly periodic lag and negative between peaks.
PeakResult findNsdfPeak (const float* nsdf, const PeakParams& p) noexcept;

} // namespace rbpitch
