#define NOMINMAX
#include "PluginProcessor.h"
#include "NsdfPeak.h"
#include <cmath>
#include <algorithm>

// Monophonic pitch detectors for processBlock. detectPitch() applies the RMS gate
// then dispatches to the algorithm chosen by pitchMethod. All members of
// RBVocalSingAudioProcessor; processBlock (in PluginProcessor.cpp) calls detectPitch
// across the TU boundary. The detectors operate on the decimated ~16 kHz stream —
// all lag/frequency maths uses detectRate, never the host rate.

// ─── Pitch dispatcher ─────────────────────────────────────────────────────────

float RBVocalSingAudioProcessor::detectPitch (const float* samples, int numSamples)
{
    float sum = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        sum += samples[i] * samples[i];
    const bool aboveGate = std::sqrtf (sum / (float) numSamples) >= gateThreshold.load (std::memory_order_relaxed);
    audioAboveGate.store (aboveGate, std::memory_order_relaxed);
    if (!aboveGate)
    {
        octaveRunHops = 0;   // silence ends any run — see kOctaveRunHops
        return 0.0f;
    }

    lastDetectClarity     = 0.0f;
    lastDetectOctaveRatio = 0.0f;
    lastDetectOctaveDoubt = false;

    float hz = 0.0f;
    switch (pitchMethod.load (std::memory_order_relaxed))
    {
        case 1:  hz = detectPitchYIN             (samples, numSamples); break;
        case 2:  hz = detectPitchMPM             (samples, numSamples); break;
        default: hz = detectPitchAutocorrelation (samples, numSamples); break;
    }

    // The fallback detectors have no confidence measure — a successful detection
    // counts as fully voiced so the clarity consumers behave sanely under them. They
    // have no octave-rival measure either, and lastDetectOctaveRatio stays 0 for them:
    // "no evidence of an octave error" rather than a fabricated reassurance.
    if (hz > 0.0f && lastDetectClarity <= 0.0f)
        lastDetectClarity = 1.0f;

    return hz;
}

// ─── Autocorrelation ──────────────────────────────────────────────────────────

float RBVocalSingAudioProcessor::detectPitchAutocorrelation (const float* samples, int numSamples)
{
    const auto [minLag, maxLag] = rbpitch::lagBounds (detectRate, kMaxLagBuf);

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

float RBVocalSingAudioProcessor::detectPitchYIN (const float* samples, int numSamples)
{
    const auto [minLag, maxLag] = rbpitch::lagBounds (detectRate, kMaxLagBuf);
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

float RBVocalSingAudioProcessor::detectPitchMPM (const float* samples, int numSamples)
{
    const auto [minLag, maxLag] = rbpitch::lagBounds (detectRate, kMaxLagBuf);

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
    const int   guard      = octaveGuard.load (std::memory_order_relaxed);
    const float rivalRatio = (guard != 0) ? rbpitch::kOctaveRivalRatio : 0.0f;

    const auto peak = rbpitch::findNsdfPeak (nsdf, { minLag, maxLag, 0.93f, kClarityThreshold,
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
