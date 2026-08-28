#pragma once
#include <JuceHeader.h>
#include "NsdfPeak.h"

// The pitch-detection front end: raw host-rate samples in, one estimate per hop out.
//
// Instance-owned rather than living on the processor so more than one can exist at a
// time. The audio thread runs one; the planned offline stem analyzer runs a second over
// audio read straight from the project. Sharing a single instance would race the audio
// thread and — worse — let the two cross-contaminate the median history, the hold
// counter and octaveRunHops, so the octave guard would report runs that never happened.
//
// Not JUCE-free (juce::dsp::FFT computes the NSDF autocorrelation), but processor-free:
// a test can construct one and push samples with no plugin scaffolding. The decision
// layer it calls, rbpitch::findNsdfPeak, is pure — see NsdfPeak.h.
//
// Audio-thread contract, unchanged by the extraction: once prepare() has run there is no
// allocation and no locking on the push path.

namespace rbpitch
{

// ─── Front-end geometry ───────────────────────────────────────────────────────
// Detection runs at a fixed ~24 kHz internal rate: the caller's samples are conditioned
// at the host rate (high-pass at the band floor against rumble/plosives, 6 kHz low-pass
// anti-alias), every decimFactor-th sample is kept into a ring of the last kPitchWindow
// samples, and the ring is analysed every kPitchHop new samples — overlapping windows,
// one estimate every ~11 ms (~94/s). All window and lag maths uses the internal rate,
// never the host rate, so these constants hold at every host rate by construction.
// kPitchWindow = 768 internal samples ≈ 32 ms at 24 kHz.
//
// The rate was 16 kHz until 2026-08-03. Measured at 24 kHz (sweeps in
// tests/NsdfPeakTests.cpp, tabulated in the 2026-08 detection-range findings):
// ~14% fewer dropouts on noisy input at equal accuracy, high-range (MIDI 72-84) error
// 3.6 -> 1.5 cents, and — because the window is a fixed *sample count* — the analysis
// window shortens 48 -> 32 ms. 24 kHz is also the ceiling: integer decimation from a
// 48 kHz host reaches only 48000/24000/16000, and a higher internal rate would push
// kPitchWindow + maxLag past kFftSize.
constexpr int kPitchWindow     = 768;
constexpr int kPitchHop        = 256;  // 3 hops per window
constexpr int kPitchHoldCycles = 24;   // ≈ 260 ms of hold at the ~11 ms hop cadence

// Must exceed the largest lag, detectRate / kMinFreqHz (60 Hz); the worst case is a host
// rate just under 36 kHz (decimFactor 1 -> lag ≈ 599), hence 640. Note kPitchWindow >=
// 2 x maxLag does not hold there — the correlation width only has to be a fraction of the
// lag range, not exceed it (measured down to 0.4x with no loss, 2026-08).
constexpr int kMaxLagBuf = 640;

// Frames whose NSDF peak is below this are unvoiced (breath, consonants, noise): they
// feed the hold decay instead of publishing a garbage pitch.
constexpr float kClarityThreshold = 0.80f;

// Consecutive voiced hops a rival must survive before the octave guard believes it.
//
// A peak near half the chosen lag is evidence of *period ambiguity*, which an octave
// error causes but so do two benign, common things: a pitch transition (the 768-sample
// window spans two notes, so consecutive periods differ and nsdf(T) is damped) and a
// vowel whose first formant sits near the 2nd harmonic (H2 exceeds H1, so the waveform
// genuinely has a strong quasi-period at T/2). Both are brief — a transition contaminates
// about kPitchWindow / kPitchHop = 3 hops — while a real octave error persists for most
// of the note. 6 hops (~64 ms) clears the transition case with margin and still fires
// well inside a short note.
//
// Found by a user testing Warn mode on a real vocal: glides and mid-sustain moments lit
// up red on a passage whose neighbouring notes made an octave jump implausible.
constexpr int kOctaveRunHops = 6;

// MPM's NSDF autocorrelation term is IFFT(|FFT(x)|²) — O(N log N) instead of the direct
// O(N x maxLag) form. kFftSize must be >= kPitchWindow + maxLag (<= 1087) so the circular
// correlation is linear over the scanned lags.
constexpr int kFftOrder = 11;
constexpr int kFftSize  = 1 << kFftOrder;   // 2048

// ─── Settings ─────────────────────────────────────────────────────────────────
// Plain values, not atomics: the owner reads its own settings once per block and hands
// them over. That is what lets a second instance run the same code at a different
// sensitivity without a second copy of the algorithm.

struct DetectorParams
{
    // RMS over one analysis window below this counts as silence: no detection runs and
    // the frame feeds the hold decay. The user's "Mic gate".
    float gateThreshold { 0.01f };

    // 0 = Autocorrelation, 1 = YIN, 2 = MPM. MPM is the production algorithm; the other
    // two remain as code-level fallbacks.
    int method { 2 };

    // Octave guard: 0 = Off, 1 = Warn, 2 = Warn + correct. Off skips the rival search
    // entirely, so the detector is bit-for-bit its pre-guard self.
    int   octaveGuard     { 0 };
    float octaveThreshold { kOctaveRivalRatio };

    // Below this NSDF peak height a frame is unvoiced. A parameter rather than a fixed
    // constant so the stem analyzer can let more consonants through on rough material;
    // the live path leaves it at kClarityThreshold.
    float clarityThreshold { kClarityThreshold };

    // Hops a lost pitch keeps being published before the readout falls to silence. 0
    // disables the hold, which is what a cursor-position readout wants: with nothing
    // streaming, a held value would describe audio that is not under the cursor.
    int holdCycles { kPitchHoldCycles };
};

// One hop's estimate. hz/midiFloat/midiNote are the *published* values — post-median and
// post-hold — so a caller can store them straight into its readout without reapplying
// either rule.
struct DetectorResult
{
    float hz        {  0.0f };
    float midiFloat { -1.0f };
    int   midiNote  { -1 };

    bool  aboveGate   { false };  // this window's RMS cleared gateThreshold
    float clarity     { 0.0f };   // 0 on a rejected or held hop
    float octaveRatio { 0.0f };   // 0 on a rejected or held hop — evidence, not decision
    bool  octaveDoubt { false };  // the persistence-filtered decision (kOctaveRunHops)
};

class Detector
{
public:
    // Picks the decimation factor and designs the conditioning filters for this host
    // rate, and drops any partial accumulation so stale samples cannot bleed into the
    // first window after a stop/start or a sample-rate change.
    void prepare (double hostSampleRate);

    void setParams (const DetectorParams& p) noexcept { params = p; }
    const DetectorParams& getParams() const noexcept  { return params; }

    // Push one raw host-rate sample. Returns true when this sample completed a hop, in
    // which case out holds that hop's estimate. Everything downstream — atomics, the hop
    // queue, timestamps — is the caller's job, which is exactly what lets the offline
    // analyzer file its results somewhere else entirely.
    bool pushSample (float raw, DetectorResult& out);

    double getDetectRate() const noexcept { return detectRate; }

    // A hop's window *ends* at the sample that completed it, so its estimate describes
    // audio centred half a window earlier. A caller timestamping a hop subtracts this;
    // filing at the trailing edge dated every detection ~16 ms late (2026-08-08).
    double windowCentreSeconds() const noexcept
    {
        return 0.5 * (double) kPitchWindow / detectRate;
    }

private:
    // One transposed-direct-form-II biquad section. Coefficients are written by
    // setBiquadLP/HP in prepare() only; process() runs on the audio thread.
    struct Biquad
    {
        float b0 {}, b1 {}, b2 {}, a1 {}, a2 {};
        float z1 {}, z2 {};
        float process (float x) noexcept
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0.0f; }
    };
    static void setBiquadLP (Biquad&, double sampleRate, double freq);
    static void setBiquadHP (Biquad&, double sampleRate, double freq);

    float detectPitch                (const float*, int);
    float detectPitchAutocorrelation (const float*, int);
    float detectPitchYIN             (const float*, int);
    float detectPitchMPM             (const float*, int);

    DetectorParams params;

    double detectRate  { 16000.0 };   // internal detection rate = host rate / decimFactor
    int    decimFactor { 3 };
    int    decimPhase  { 0 };
    Biquad hpFilter;                  // high-pass at the band floor (rumble / plosives)
    Biquad lpFilter1, lpFilter2;      // 2 × 6 kHz low-pass (decimation anti-alias)

    // Ring of the most recent kPitchWindow internal samples, linearised into
    // windowScratch for each detection so the detectors see contiguous memory.
    float pitchRing[kPitchWindow]     = {};
    float windowScratch[kPitchWindow] = {};
    int   ringPos    { 0 };   // next write index == oldest sample once filled
    int   ringFilled { 0 };   // saturates at kPitchWindow
    int   hopCount   { 0 };   // internal samples since the last detection

    // Written by detectPitch, read when the hop's result is assembled.
    bool  lastAboveGate         { false };
    float lastDetectClarity     { 0.0f };
    float lastDetectOctaveRatio { 0.0f };
    bool  lastDetectOctaveDoubt { false };
    int   octaveRunHops         { 0 };   // consecutive voiced hops with a rival above
                                         //   threshold; reset by silence and by any hop
                                         //   without one, so a run never bridges a gap

    // The published pitch is the median of the last 3 accepted estimates once the history
    // is full — a single-frame octave/subharmonic error (note onsets, tone transitions)
    // never reaches the UI, at the cost of one hop ≈ 11 ms of latency. Reset on every
    // unvoiced frame so a fresh onset publishes immediately.
    float medianHist[3] = {};
    int   medianCount { 0 };

    // Survive across hops: the hold decay republishes these while pitchHoldFrames lasts.
    int   pitchHoldFrames { 0 };
    float pubFrequency    {  0.0f };
    float pubMidiFloat    { -1.0f };
    int   pubMidiNote     { -1 };

    // Constructed once with the detector (allocation happens off the audio thread);
    // fftScratch is the in-place real-FFT workspace (2 × kFftSize floats).
    juce::dsp::FFT pitchFft { kFftOrder };
    float fftScratch[2 * kFftSize] = {};
};

} // namespace rbpitch
