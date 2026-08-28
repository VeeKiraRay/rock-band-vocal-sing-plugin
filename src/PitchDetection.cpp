#define NOMINMAX
#include "PitchDetector.h"
#include <cmath>
#include <algorithm>

// Implementation of rbpitch::Detector — the monophonic pitch-detection front end.
//
// pushSample() is the whole pipeline: condition at the host rate, decimate to the fixed
// internal rate, fill the analysis ring, and once per kPitchHop run detectPitch() over it
// and fold the result through the median filter and hold decay. detectPitch() applies the
// RMS gate then dispatches to the algorithm chosen by params.method; the detectors operate
// on the decimated stream, so all lag/frequency maths uses detectRate, never the host rate.
//
// Nothing here touches the plugin: the caller owns publishing (atomics, hop queue,
// timestamps), which is what lets the offline stem analyzer run a second instance over
// project audio and file the results somewhere else entirely.

namespace rbpitch
{

// ─── Filter design ────────────────────────────────────────────────────────────

void Detector::setBiquadLP (Biquad& f, double sampleRate, double freq)
{
    const double w0    = juce::MathConstants<double>::twoPi
                             * std::min (freq, sampleRate * 0.45) / sampleRate;
    const double cosw  = std::cos (w0);
    const double alpha = std::sin (w0) / juce::MathConstants<double>::sqrt2;
    const double a0    = 1.0 + alpha;
    f.b0 = (float) ((1.0 - cosw) * 0.5 / a0);
    f.b1 = (float) ((1.0 - cosw)       / a0);
    f.b2 = f.b0;
    f.a1 = (float) (-2.0 * cosw        / a0);
    f.a2 = (float) ((1.0 - alpha)      / a0);
}

void Detector::setBiquadHP (Biquad& f, double sampleRate, double freq)
{
    const double w0    = juce::MathConstants<double>::twoPi
                             * std::min (freq, sampleRate * 0.45) / sampleRate;
    const double cosw  = std::cos (w0);
    const double alpha = std::sin (w0) / juce::MathConstants<double>::sqrt2;
    const double a0    = 1.0 + alpha;
    f.b0 = (float) ((1.0 + cosw) * 0.5 / a0);
    f.b1 = (float) (-(1.0 + cosw)      / a0);
    f.b2 = f.b0;
    f.a1 = (float) (-2.0 * cosw        / a0);
    f.a2 = (float) ((1.0 - alpha)      / a0);
}

// ─── Preparation ──────────────────────────────────────────────────────────────

void Detector::prepare (double hostSampleRate)
{
    // Fixed ~24 kHz internal detection rate by integer decimation: 44.1/48 kHz ÷ 2,
    // 88.2/96 kHz ÷ 4, 176.4 kHz ÷ 7, 192 kHz ÷ 8 → detectRate ≈ 22.05–25.2 kHz. The
    // window and lag ranges are in internal samples, so the detection floor and cadence
    // survive every host rate without buffer scaling. 22050 and 24000 measure identically,
    // so both host families get the same behaviour (see the rate sweep in
    // _upcoming/PITCH_DETECTION_FINDINGS.md).
    decimFactor = std::max (1, (int) std::lround (hostSampleRate / 24000.0));
    detectRate  = hostSampleRate / (double) decimFactor;

    // Front-end conditioning at the host rate: high-pass against rumble/plosives, two
    // cascaded low-passes as the decimator's anti-alias filter (the internal Nyquist is
    // ≥ 7.35 kHz at the rates above; NSDF needs nothing past ~2 kHz). The high-pass tracks
    // the bottom of the searched band rather than sitting at a fixed frequency — a cutoff
    // above the lowest expected pitch attenuates the very fundamentals the detector is
    // meant to find, and the old fixed 70 Hz sat above MIDI 36 (65.4 Hz). Deriving it
    // keeps the two from drifting apart again.
    setBiquadHP (hpFilter,  hostSampleRate, kMinFreqHz);
    setBiquadLP (lpFilter1, hostSampleRate, 6000.0);
    setBiquadLP (lpFilter2, hostSampleRate, 6000.0);
    hpFilter.reset();
    lpFilter1.reset();
    lpFilter2.reset();

    // Drop any partial accumulation from before a stop/start or sample-rate change so
    // stale samples never bleed into the first detection window.
    decimPhase      = 0;
    ringPos         = 0;
    ringFilled      = 0;
    hopCount        = 0;
    pitchHoldFrames = 0;
    medianCount     = 0;
    octaveRunHops   = 0;

    pubFrequency = 0.0f;
    pubMidiFloat = -1.0f;
    pubMidiNote  = -1;
}

// ─── The push path ────────────────────────────────────────────────────────────

bool Detector::pushSample (float raw, DetectorResult& out)
{
    // Condition at the host rate, then keep every decimFactor-th sample for the internal
    // detection pipeline (see the front-end block in the header).
    const float conditioned = lpFilter2.process (lpFilter1.process (hpFilter.process (raw)));

    if (++decimPhase < decimFactor)
        return false;
    decimPhase = 0;

    pitchRing[ringPos] = conditioned;
    if (++ringPos == kPitchWindow) ringPos = 0;
    if (ringFilled < kPitchWindow) ++ringFilled;

    // hopCount deliberately keeps climbing while the ring is still filling, so the first
    // full window analyses immediately rather than waiting another kPitchHop samples.
    if (++hopCount < kPitchHop || ringFilled < kPitchWindow)
        return false;
    hopCount = 0;

    // Linearise the ring (the oldest sample sits at ringPos) so the detectors see one
    // contiguous window.
    const int tail = kPitchWindow - ringPos;
    std::copy (pitchRing + ringPos, pitchRing + kPitchWindow, windowScratch);
    std::copy (pitchRing, pitchRing + ringPos, windowScratch + tail);

    const float hz = detectPitch (windowScratch, kPitchWindow);

    // Clarity and octave-rival evidence are zeroed on a rejected or held hop: a held pitch
    // is not fresh evidence of anything, so it must not keep re-flagging after the audio
    // that produced it has gone.
    out.aboveGate   = lastAboveGate;
    out.clarity     = (hz > 0.0f) ? lastDetectClarity : 0.0f;
    out.octaveRatio = (hz > 0.0f) ? lastDetectOctaveRatio : 0.0f;
    out.octaveDoubt = (hz > 0.0f) && lastDetectOctaveDoubt;

    if (hz > 0.0f)
    {
        // Median-of-3 over the accepted estimates (see medianHist); until the history
        // fills, the newest value passes through unfiltered.
        if (medianCount == 3)
        {
            medianHist[0] = medianHist[1];
            medianHist[1] = medianHist[2];
            medianHist[2] = hz;
        }
        else
        {
            medianHist[medianCount++] = hz;
        }
        const float pubHz = (medianCount == 3)
            ? std::max (std::min (medianHist[0], medianHist[1]),
                        std::min (std::max (medianHist[0], medianHist[1]),
                                  medianHist[2]))
            : hz;

        const float midi = 69.0f + 12.0f * std::log2f (pubHz / 440.0f);
        pubFrequency    = pubHz;
        pubMidiFloat    = std::clamp (midi, 0.0f, 127.0f);
        pubMidiNote     = std::clamp (static_cast<int> (std::roundf (midi)), 0, 127);
        pitchHoldFrames = params.holdCycles;
    }
    else
    {
        // Unvoiced frame: restart the median history so no stale estimate bridges across
        // a gap into the next onset.
        medianCount = 0;

        if (pitchHoldFrames > 0)
        {
            --pitchHoldFrames;
        }
        else
        {
            pubFrequency = 0.0f;
            pubMidiFloat = -1.0f;
            pubMidiNote  = -1;
        }
    }

    out.hz        = pubFrequency;
    out.midiFloat = pubMidiFloat;
    out.midiNote  = pubMidiNote;
    return true;
}

// ─── Pitch dispatcher ─────────────────────────────────────────────────────────

float Detector::detectPitch (const float* samples, int numSamples)
{
    float sum = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        sum += samples[i] * samples[i];
    const bool aboveGate = std::sqrtf (sum / (float) numSamples) >= params.gateThreshold;
    lastAboveGate = aboveGate;
    if (!aboveGate)
    {
        octaveRunHops = 0;   // silence ends any run — see kOctaveRunHops
        return 0.0f;
    }

    lastDetectClarity     = 0.0f;
    lastDetectOctaveRatio = 0.0f;
    lastDetectOctaveDoubt = false;

    float hz = 0.0f;
    switch (params.method)
    {
        case 1:  hz = detectPitchYIN             (samples, numSamples); break;
        case 2:  hz = detectPitchMPM             (samples, numSamples); break;
        default: hz = detectPitchAutocorrelation (samples, numSamples); break;
    }

    // The fallback detectors have no confidence measure — a successful detection counts as
    // fully voiced so the clarity consumers behave sanely under them. They have no
    // octave-rival measure either, and lastDetectOctaveRatio stays 0 for them: "no evidence
    // of an octave error" rather than a fabricated reassurance.
    if (hz > 0.0f && lastDetectClarity <= 0.0f)
        lastDetectClarity = 1.0f;

    return hz;
}

// ─── Autocorrelation ──────────────────────────────────────────────────────────

float Detector::detectPitchAutocorrelation (const float* samples, int numSamples)
{
    const auto [minLag, maxLag] = lagBounds (detectRate, kMaxLagBuf);

    // Only needs a non-empty correlation width — requiring numSamples ≥ 2 × maxLag was
    // over-strict and silently disabled this fallback once the lag range widened to
    // 60 Hz (_upcoming/PITCH_DETECTION_FINDINGS.md §7 measured 0.4× as lossless).
    if (numSamples <= maxLag) return 0.0f;

    float bestCorr = -1.0f;
    int   bestLag  = minLag;

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        float corr = 0.0f;
        const int limit = numSamples - lag;
        for (int n = 0; n < limit; ++n)
            corr += samples[n] * samples[n + lag];

        // Mean, not sum: the overlap shrinks as the lag grows, so a raw sum falls off
        // with lag for reasons that have nothing to do with periodicity and biases the
        // search toward short lags — the same defect fixed-width summation addresses in
        // YIN (_upcoming/PITCH_DETECTION_FINDINGS.md §5). Latent until the scanned band
        // widened; before that, minLag happened to sit past the crossover.
        corr /= (float) limit;

        if (corr > bestCorr) { bestCorr = corr; bestLag = lag; }
    }

    return (float) detectRate / (float) bestLag;
}

// ─── YIN ──────────────────────────────────────────────────────────────────────

float Detector::detectPitchYIN (const float* samples, int numSamples)
{
    const auto [minLag, maxLag] = lagBounds (detectRate, kMaxLagBuf);
    const int  W                = numSamples - maxLag;

    if (W <= 0) return 0.0f;

    float d[kMaxLagBuf];
    std::fill (d + 1, d + maxLag + 1, 0.0f);
    for (int tau = 1; tau <= maxLag; ++tau)
        for (int j = 0; j < W; ++j)
        {
            const float diff = samples[j] - samples[j + tau];
            d[tau] += diff * diff;
        }

    float dPrime[kMaxLagBuf];
    dPrime[0] = 1.0f;
    float runningSum = 0.0f;
    for (int tau = 1; tau <= maxLag; ++tau)
    {
        runningSum += d[tau];
        dPrime[tau] = (runningSum > 0.0f) ? (d[tau] * (float) tau / runningSum) : 1.0f;
    }

    int bestTau = -1;
    for (int tau = minLag; tau < maxLag; ++tau)
    {
        if (dPrime[tau] < 0.15f)
        {
            while (tau + 1 < maxLag && dPrime[tau + 1] < dPrime[tau])
                ++tau;
            bestTau = tau;
            break;
        }
    }

    if (bestTau < 0) return 0.0f;

    float betterTau = (float) bestTau;
    if (bestTau > minLag && bestTau < maxLag)
    {
        const float s0    = dPrime[bestTau - 1];
        const float s1    = dPrime[bestTau];
        const float s2    = dPrime[bestTau + 1];
        const float denom = 2.0f * s1 - s2 - s0;
        if (std::abs (denom) > 1e-6f)
            betterTau = (float) bestTau + (s2 - s0) / (2.0f * denom);
    }

    return (float) detectRate / betterTau;
}

// ─── MPM ──────────────────────────────────────────────────────────────────────

float Detector::detectPitchMPM (const float* samples, int numSamples)
{
    const auto [minLag, maxLag] = lagBounds (detectRate, kMaxLagBuf);

    // nsdf(τ) = 2·acf(τ) / m(τ), same values as the direct O(N × maxLag) loop this
    // replaced — only the computation route changed. acf(τ) for all lags at once is
    // IFFT(|FFT(x)|²) (zero-padded, kFftSize ≥ numSamples + maxLag keeps it linear);
    // m(τ) = Σ x_j² + Σ x_{j+τ}² over the overlap comes from a prefix sum of x².
    jassert (numSamples + maxLag <= kFftSize);
    jassert (numSamples <= kPitchWindow);

    std::fill (fftScratch, fftScratch + 2 * kFftSize, 0.0f);
    std::copy (samples, samples + numSamples, fftScratch);
    pitchFft.performRealOnlyForwardTransform (fftScratch);
    for (int k = 0; k < kFftSize; ++k)
    {
        const float re = fftScratch[2 * k];
        const float im = fftScratch[2 * k + 1];
        fftScratch[2 * k]     = re * re + im * im;
        fftScratch[2 * k + 1] = 0.0f;
    }
    pitchFft.performRealOnlyInverseTransform (fftScratch);   // fftScratch[τ] = acf(τ)

    float cum[kPitchWindow + 1];
    cum[0] = 0.0f;
    for (int j = 0; j < numSamples; ++j)
        cum[j + 1] = cum[j] + samples[j] * samples[j];

    float nsdf[kMaxLagBuf];
    for (int tau = 0; tau <= maxLag; ++tau)
    {
        const float m = cum[numSamples - tau] + cum[numSamples] - cum[tau];
        nsdf[tau] = (m > 0.0f) ? 2.0f * fftScratch[tau] / m : 0.0f;
    }

    // Peak selection, boundary rejection, the clarity gate and parabolic interpolation
    // all live in rbpitch::findNsdfPeak (src/NsdfPeak.h) — pure, so the unit tests drive
    // them on hand-built NSDF tables instead of hoping a synthesised signal hits each
    // path. Clarity is published even when the frame is rejected for being unvoiced.
    //
    // The octave guard rides along as one extra parameter: 0 skips the rival search, so
    // Off costs nothing and returns exactly what it always did.
    const int   guard      = params.octaveGuard;
    const float rivalRatio = (guard != 0) ? params.octaveThreshold : 0.0f;

    const auto peak = findNsdfPeak (nsdf, { minLag, maxLag, 0.93f, params.clarityThreshold,
                                            rivalRatio });
    lastDetectClarity     = peak.clarity;
    lastDetectOctaveRatio = peak.octaveRatio;

    if (peak.tau <= 0.0f)
    {
        octaveRunHops = 0;   // unvoiced hop — see kOctaveRunHops
        return 0.0f;
    }

    // Persistence filter. A single hop's rival proves very little: pitch transitions and
    // strong-2nd-harmonic vowels both raise one for a few hops with the pitch entirely
    // correct. Only a rival that survives kOctaveRunHops consecutive voiced hops is
    // treated as a real octave error. The raw ratio above is left unfiltered — it is the
    // evidence, and the stats chip shows it so the thresholds can be judged from real
    // material; this is the decision.
    octaveRunHops = (peak.octaveTau > 0.0f) ? octaveRunHops + 1 : 0;
    lastDetectOctaveDoubt = (octaveRunHops >= kOctaveRunHops);

    // Correct mode takes the rival lag — half the period, so an octave up. Reporting it
    // here rather than in the editor is what keeps the correction consistent across every
    // consumer: it goes through the median filter with its neighbours, and the corrected
    // value is what reaches scoring, the pitch trail and the take export. The doubt flag
    // travels with it, so a corrected hop stays visibly marked and the user can audit
    // where the guard acted.
    if (guard == 2 && lastDetectOctaveDoubt)
        return (float) detectRate / peak.octaveTau;

    return (float) detectRate / peak.tau;
}

} // namespace rbpitch
