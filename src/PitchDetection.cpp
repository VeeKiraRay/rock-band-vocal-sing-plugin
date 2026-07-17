#define NOMINMAX
#include "PluginProcessor.h"
#include <cmath>
#include <algorithm>

// Monophonic pitch detectors for processBlock. detectPitch() applies the RMS gate
// then dispatches to the algorithm chosen by pitchMethod. All members of
// RBVocalSingAudioProcessor; processBlock (in PluginProcessor.cpp) calls detectPitch
// across the TU boundary.

// ─── Pitch dispatcher ─────────────────────────────────────────────────────────

float RBVocalSingAudioProcessor::detectPitch (const float* samples, int numSamples)
{
    float sum = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        sum += samples[i] * samples[i];
    const bool aboveGate = std::sqrtf (sum / (float) numSamples) >= gateThreshold.load (std::memory_order_relaxed);
    audioAboveGate.store (aboveGate, std::memory_order_relaxed);
    if (!aboveGate)
        return 0.0f;

    switch (pitchMethod.load (std::memory_order_relaxed))
    {
        case 1:  return detectPitchYIN             (samples, numSamples);
        case 2:  return detectPitchMPM             (samples, numSamples);
        default: return detectPitchAutocorrelation (samples, numSamples);
    }
}

// ─── Autocorrelation ──────────────────────────────────────────────────────────

float RBVocalSingAudioProcessor::detectPitchAutocorrelation (const float* samples, int numSamples)
{
    const int minLag = (int) (storedSampleRate / 1000.0);
    const int maxLag = std::min ((int) (storedSampleRate / 80.0), kMaxLagBuf - 1);

    if (numSamples < maxLag * 2) return 0.0f;

    float bestCorr = -1.0f;
    int   bestLag  = minLag;

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        float corr = 0.0f;
        const int limit = numSamples - lag;
        for (int n = 0; n < limit; ++n)
            corr += samples[n] * samples[n + lag];

        if (corr > bestCorr) { bestCorr = corr; bestLag = lag; }
    }

    return (float) storedSampleRate / (float) bestLag;
}

// ─── YIN ──────────────────────────────────────────────────────────────────────

float RBVocalSingAudioProcessor::detectPitchYIN (const float* samples, int numSamples)
{
    const int minLag = (int) (storedSampleRate / 1000.0);
    const int maxLag = std::min ((int) (storedSampleRate / 80.0), kMaxLagBuf - 1);
    const int W      = numSamples - maxLag;

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

    return (float) storedSampleRate / betterTau;
}

// ─── MPM ──────────────────────────────────────────────────────────────────────

float RBVocalSingAudioProcessor::detectPitchMPM (const float* samples, int numSamples)
{
    const int minLag = (int) (storedSampleRate / 1000.0);
    const int maxLag = std::min ((int) (storedSampleRate / 80.0), kMaxLagBuf - 1);

    float nsdf[kMaxLagBuf];
    for (int tau = 0; tau <= maxLag; ++tau)
    {
        float acf = 0.0f, norm = 0.0f;
        for (int j = 0; j < numSamples - tau; ++j)
        {
            acf  += samples[j] * samples[j + tau];
            norm += samples[j] * samples[j] + samples[j + tau] * samples[j + tau];
        }
        nsdf[tau] = (norm > 0.0f) ? 2.0f * acf / norm : 0.0f;
    }

    const float globalMax    = *std::max_element (nsdf + minLag, nsdf + maxLag + 1);
    const float keyThreshold = 0.93f * globalMax;

    int   bestTau  = -1;
    bool  inPos    = false;
    float localMax = 0.0f;
    int   localTau = minLag;

    for (int tau = minLag; tau <= maxLag; ++tau)
    {
        if (nsdf[tau] > 0.0f)
        {
            if (!inPos || nsdf[tau] > localMax) { localMax = nsdf[tau]; localTau = tau; }
            inPos = true;
        }
        else if (inPos)
        {
            if (localMax >= keyThreshold) { bestTau = localTau; break; }
            inPos    = false;
            localMax = 0.0f;
        }
    }

    if (bestTau < 0 && inPos && localMax >= keyThreshold)
        bestTau = localTau;

    if (bestTau < 0) return 0.0f;

    float betterTau = (float) bestTau;
    if (bestTau > minLag && bestTau < maxLag)
    {
        const float s0    = nsdf[bestTau - 1];
        const float s1    = nsdf[bestTau];
        const float s2    = nsdf[bestTau + 1];
        const float denom = 2.0f * s1 - s2 - s0;
        if (std::abs (denom) > 1e-6f)
            betterTau = (float) bestTau + (s2 - s0) / (2.0f * denom);
    }

    return (float) storedSampleRate / betterTau;
}
