#include "NsdfPeak.h"
#include <algorithm>
#include <cmath>

namespace rbpitch
{

LagBounds lagBounds (double detectRate, int lagBufCapacity) noexcept
{
    return { (int) (detectRate / kMaxFreqHz),
             std::min ((int) (detectRate / kMinFreqHz), lagBufCapacity - 1) };
}

namespace
{

// Sub-sample refinement of an integer peak by fitting a parabola through it and its two
// neighbours. Caller must have established that tau ± 1 are readable.
float parabolicTau (const float* nsdf, int tau) noexcept
{
    const float s0    = nsdf[tau - 1];
    const float s1    = nsdf[tau];
    const float s2    = nsdf[tau + 1];
    const float denom = 2.0f * s1 - s2 - s0;

    return (std::abs (denom) > 1e-6f) ? (float) tau + (s2 - s0) / (2.0f * denom)
                                      : (float) tau;
}

} // namespace

PeakResult findNsdfPeak (const float* nsdf, const PeakParams& p) noexcept
{
    // Degenerate range: the walk below and the parabolic interpolation both assume at
    // least one interior lag exists. Cannot happen at any supported host rate, but this
    // is a public pure function now, so fail closed rather than read out of bounds.
    if (p.maxLag <= p.minLag + 1)
        return {};

    // Skip the zero-lag lobe. nsdf(0) is 1 by definition and stays positive for roughly a
    // third of a period, so a walk that begins inside that lobe selects its flank instead
    // of the true peak — and the flank is highest at the lowest lag, which is why the
    // symptom was a peak pinned on minLag. Canonical MPM waits for the first negative
    // crossing from tau = 0; doing it any later (e.g. from minLag) reintroduces the bug
    // for low notes, and starting the *search* there rather than at tau = 0 would hand
    // high notes their 2x sub-period instead. Established by the 2026-08 range findings.
    int firstNegative = 1;
    while (firstNegative <= p.maxLag && nsdf[firstNegative] > 0.0f)
        ++firstNegative;

    if (firstNegative > p.maxLag)
        return {};   // never crossed zero: the period is far below the searched band

    // Past the lobe, but never below the configured frequency ceiling.
    const int searchStart = std::max (p.minLag, firstNegative);
    if (searchStart >= p.maxLag)
        return {};

    // Key threshold is relative to the best evidence in the region actually walked, so
    // the discarded zero-lag flank cannot inflate it.
    const float globalMax    = *std::max_element (nsdf + searchStart, nsdf + p.maxLag + 1);
    const float keyThreshold = p.keyThresholdRatio * globalMax;

    // MPM key-maximum walk: track each positive-going run and take the first whose peak
    // reaches the key threshold. Taking the *first* qualifying run rather than the global
    // maximum is what rejects octave-down errors — 2τ correlates as well as τ.
    int   bestTau  = -1;
    bool  inPos    = false;
    float localMax = 0.0f;
    int   localTau = searchStart;

    for (int tau = searchStart; tau <= p.maxLag; ++tau)
    {
        if (nsdf[tau] > 0.0f)
        {
            if (! inPos || nsdf[tau] > localMax) { localMax = nsdf[tau]; localTau = tau; }
            inPos = true;
        }
        else if (inPos)
        {
            if (localMax >= keyThreshold) { bestTau = localTau; break; }
            inPos    = false;
            localMax = 0.0f;
        }
    }

    // Ran to the end still inside a positive run (the peak is the last thing in range).
    if (bestTau < 0 && inPos && localMax >= keyThreshold)
        bestTau = localTau;

    if (bestTau < 0)
        return {};

    // A peak sitting on a scan boundary is never a trustworthy pitch — it means the true
    // period is outside the searched range and the walk ran out of room, or a transient
    // pinned the estimate at the cap. Rejecting also guarantees bestTau ± 1 is in range
    // for the interpolation below, so this must stay ahead of it.
    if (bestTau <= p.minLag || bestTau >= p.maxLag)
        return {};

    PeakResult r;
    r.clarity = std::min (localMax, 1.0f);

    // Breath, consonants and noise pass an energy gate but correlate poorly. Report the
    // clarity regardless so the caller can publish it, but withhold the lag.
    if (r.clarity < p.clarityThreshold)
        return r;

    r.tau = parabolicTau (nsdf, bestTau);

    // ── Octave rival ──────────────────────────────────────────────────────────
    // Evidence that the walk above landed an octave too low. For a *correct* pick at lag
    // tau, nsdf(tau/2) sits in a trough — a signal periodic at tau is not periodic at
    // tau/2 — so a genuine peak there says the true period is the shorter one and the
    // walk skipped it. That happens when nsdf(tau) is damped below the key threshold
    // while nsdf(2·tau) is not: period-to-period amplitude jitter, creak, and the phasey
    // residue of AI stem separation all do it.
    //
    // The mirror check is deliberately absent. A peak at 2·tau ALWAYS exists for a
    // periodic signal, so it is not evidence of anything, and taking the first qualifying
    // run already biases this picker against octave-up errors. Adding a 2·tau test to
    // "complete the symmetry" would only manufacture false alarms.
    if (p.octaveRivalRatio > 0.0f && localMax > 0.0f)
    {
        // ~6% either side of tau/2, the same headroom the band bounds use: an octave-down
        // pick is rarely at exactly 2x, and the rival peak drifts with it.
        const int lo = (int) std::lround ((double) bestTau * 0.47);
        const int hi = (int) std::lround ((double) bestTau * 0.53);

        // The whole window (plus the neighbours the peak test and interpolation read)
        // must sit inside the region actually walked. When tau/2 falls in the zero-lag
        // lobe or above the band ceiling there is no trustworthy rival to find, and
        // correcting onto a lag outside the scanned band would report a pitch this
        // detector never validated.
        if (lo >= searchStart + 1 && hi <= p.maxLag - 1 && lo <= hi)
        {
            int rivalTau = lo;
            for (int tau = lo + 1; tau <= hi; ++tau)
                if (nsdf[tau] > nsdf[rivalTau]) rivalTau = tau;

            // A genuine local maximum, not the flank of something outside the window —
            // a flank keeps climbing past one edge and would otherwise qualify on height.
            const float rivalVal = nsdf[rivalTau];
            if (rivalVal > 0.0f
                && nsdf[rivalTau - 1] <= rivalVal
                && nsdf[rivalTau + 1] <= rivalVal)
            {
                r.octaveRatio = rivalVal / localMax;
                if (r.octaveRatio >= p.octaveRivalRatio)
                    r.octaveTau = parabolicTau (nsdf, rivalTau);
            }
        }
    }

    return r;
}

} // namespace rbpitch
