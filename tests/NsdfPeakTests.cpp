#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "NsdfPeak.h"
#include <vector>
#include <cmath>
#include <random>
#include <iostream>
#include <iomanip>
#include <iterator>

// Direct unit tests for the MPM peak picker. The point of extracting it (see NsdfPeak.h)
// is that these run on NSDF tables built here — either by hand, to hit a specific branch
// deterministically, or from a synthetic waveform, to exercise realistic curve shapes.
// Neither was reachable while the picker lived inside detectPitchMPM.

namespace
{

constexpr double kPi         = 3.14159265358979323846;  // MSVC has no M_PI without _USE_MATH_DEFINES
constexpr double kDetectRate = 24000.0;   // the internal rate at 48/96/192 kHz hosts
constexpr int    kLagBufCap  = 640;       // RBVocalSingAudioProcessor::kMaxLagBuf

// Reference NSDF: 2·acf(τ) / m(τ) over a window, computed directly. Deliberately the
// naive O(N × maxLag) form — this is the definition the FFT route in detectPitchMPM is
// an optimisation of, so it is the right thing to test the picker against.
std::vector<float> nsdfOf (const std::vector<float>& x, int maxLag)
{
    const int n = (int) x.size();
    std::vector<float> out ((size_t) maxLag + 1, 0.0f);

    for (int tau = 0; tau <= maxLag; ++tau)
    {
        double acf = 0.0, m = 0.0;
        for (int j = 0; j + tau < n; ++j)
        {
            acf += (double) x[(size_t) j] * (double) x[(size_t) (j + tau)];
            m   += (double) x[(size_t) j] * (double) x[(size_t) j]
                 + (double) x[(size_t) (j + tau)] * (double) x[(size_t) (j + tau)];
        }
        out[(size_t) tau] = (m > 0.0) ? (float) (2.0 * acf / m) : 0.0f;
    }
    return out;
}

// Band-limited sawtooth at an arbitrary analysis rate: harmonics 1..H at 1/k amplitude,
// stopping at Nyquist. Generating directly at the target rate models a correct
// anti-aliased resample, which is what the rate sweep below needs to compare fairly.
// Closer to a voice (and to ReaSynth) than a bare sine, which matters — a pure sine has
// an unusually wide zero-lag lobe and flatters the picker's low end.
std::vector<float> sawtoothAt (double freqHz, double rate, int numSamples, int harmonics = 12)
{
    std::vector<float> x ((size_t) numSamples, 0.0f);
    for (int k = 1; k <= harmonics; ++k)
    {
        const double f = freqHz * k;
        if (f >= rate * 0.5) break;
        for (int i = 0; i < numSamples; ++i)
            x[(size_t) i] += (float) (std::sin (2.0 * kPi * f * i / rate) / k);
    }
    return x;
}

std::vector<float> sawtooth (double freqHz, int numSamples, int harmonics = 8)
{
    return sawtoothAt (freqHz, kDetectRate, numSamples, harmonics);
}

// Detected frequency for a note fed through the production lag bounds, or 0 if the
// picker found nothing usable. Mirrors detectPitchMPM's tail exactly.
float detectHz (double freqHz, int windowSamples = 768)
{
    const auto b    = rbpitch::lagBounds (kDetectRate, kLagBufCap);
    const auto wave = sawtooth (freqHz, windowSamples);
    const auto nsdf = nsdfOf (wave, b.maxLag);
    const auto peak = rbpitch::findNsdfPeak (nsdf.data(), { b.minLag, b.maxLag });
    return (peak.tau > 0.0f) ? (float) (kDetectRate / peak.tau) : 0.0f;
}

float midiToHz (double midi) { return (float) (440.0 * std::pow (2.0, (midi - 69.0) / 12.0)); }

// ── Rate-sweep support (used only by the hidden [.sweep] case at the bottom) ──

// Additive white noise at a target SNR, deterministic per seed.
void addNoise (std::vector<float>& x, double snrDb, uint32_t seed)
{
    double sumSq = 0.0;
    for (float s : x) sumSq += (double) s * s;
    const double sigRms   = std::sqrt (sumSq / (double) x.size());
    const double noiseRms = sigRms / std::pow (10.0, snrDb / 20.0);

    std::mt19937 rng (seed);
    std::normal_distribution<double> dist (0.0, noiseRms);
    for (float& s : x) s += (float) dist (rng);
}

// One case: synthesise at `rate` for a fixed *duration* (so window length in seconds is
// held constant and only the rate varies), then run the production picker over the
// production band. lagBufCapacity is deliberately generous — this measures the algorithm,
// not kMaxLagBuf.
float detectAtRate (double freqHz, double rate, double windowSec, double snrDb, uint32_t seed)
{
    const int  n = (int) std::lround (windowSec * rate);
    const auto b = rbpitch::lagBounds (rate, 4096);

    auto wave = sawtoothAt (freqHz, rate, n);
    if (snrDb > 0.0) addNoise (wave, snrDb, seed);

    const auto nsdf = nsdfOf (wave, b.maxLag);
    const auto peak = rbpitch::findNsdfPeak (nsdf.data(), { b.minLag, b.maxLag });
    return (peak.tau > 0.0f) ? (float) (rate / peak.tau) : 0.0f;
}

struct SweepStats
{
    int    correct = 0, octave = 0, wrong = 0, missed = 0;
    double sumAbsCents = 0.0, maxAbsCents = 0.0;
    int    worstMidi = -1;

    void add (int midi, float want, float got)
    {
        if (got <= 0.0f) { ++missed; return; }

        const double cents = 1200.0 * std::log2 ((double) got / (double) want);
        const double a     = std::abs (cents);

        if (a <= 50.0)
        {
            ++correct;
            sumAbsCents += a;
            if (a > maxAbsCents) { maxAbsCents = a; worstMidi = midi; }
        }
        else if (std::abs (a - 1200.0) <= 50.0 || std::abs (a - 2400.0) <= 50.0) ++octave;
        else                                                                     ++wrong;
    }

    double meanCents() const { return correct > 0 ? sumAbsCents / correct : 0.0; }
};

} // namespace

// ─── Lag bounds ───────────────────────────────────────────────────────────────

TEST_CASE ("nsdf: lag bounds match the documented scanned band", "[nsdf]")
{
    const auto b = rbpitch::lagBounds (kDetectRate, kLagBufCap);
    REQUIRE (b.minLag == 20);    // 24000 / 1200 Hz
    REQUIRE (b.maxLag == 400);   // 24000 / 60 Hz, under the buffer cap
}

TEST_CASE ("nsdf: the scanned band brackets the RB vocal range with headroom", "[nsdf]")
{
    // Boundary rejection discards a peak landing on either bound, so the band has to
    // clear MIDI 36-84 rather than merely touch it (§6: 1.01x fails, 1.06x is fine).
    REQUIRE (rbpitch::kMinFreqHz < midiToHz (36) / 1.05);
    REQUIRE (rbpitch::kMaxFreqHz > midiToHz (84) * 1.05);
}

TEST_CASE ("nsdf: maxLag has buffer headroom at every supported rate", "[nsdf]")
{
    // decimFactor holds detectRate in ~22.05-25.2 kHz, except a host rate just under
    // 36 kHz which decimates by 1 — the worst case kMaxLagBuf is sized against. The
    // invariant is that the cap never actually binds, so the scanned band is the full
    // configured one at every rate rather than being silently narrowed.
    for (double rate : { 22050.0, 24000.0, 25200.0, 35999.0 })
    {
        const auto b = rbpitch::lagBounds (rate, kLagBufCap);
        INFO ("detectRate " << rate);
        REQUIRE (b.maxLag < kLagBufCap);
        REQUIRE (b.maxLag == (int) (rate / rbpitch::kMinFreqHz));
    }
}

TEST_CASE ("nsdf: the buffer cap clamps maxLag when it would overflow", "[nsdf]")
{
    // The clamp itself still has to work — a future rate or bound change could make it
    // load-bearing (widening the low end to 60 Hz would, see PITCH_DETECTION_FINDINGS).
    REQUIRE (rbpitch::lagBounds (24000.0, 100).maxLag == 99);
}

// ─── Deterministic branch coverage on hand-built tables ───────────────────────

TEST_CASE ("nsdf: a peak on minLag is rejected", "[nsdf]")
{
    // Highest value sits exactly on the low boundary, then the run closes.
    std::vector<float> t (201, -0.5f);
    t[16] = 1.0f;
    t[17] = 0.4f;

    const auto r = rbpitch::findNsdfPeak (t.data(), { 16, 200 });
    REQUIRE (r.tau < 0.0f);
    REQUIRE (r.clarity == 0.0f);   // withheld, not reported as confident
}

TEST_CASE ("nsdf: a peak on maxLag is rejected", "[nsdf]")
{
    // Positive run rising all the way to the top of the range and never closing —
    // the "ran out of room at the bottom of the pitch range" case.
    std::vector<float> t (201, -0.5f);
    for (int tau = 180; tau <= 200; ++tau)
        t[(size_t) tau] = 0.5f + 0.02f * (float) (tau - 180);

    const auto r = rbpitch::findNsdfPeak (t.data(), { 16, 200 });
    REQUIRE (r.tau < 0.0f);
    REQUIRE (r.clarity == 0.0f);
}

TEST_CASE ("nsdf: an interior peak is accepted and reports its clarity", "[nsdf]")
{
    std::vector<float> t (201, -0.5f);
    t[99] = 0.6f; t[100] = 0.95f; t[101] = 0.6f;

    const auto r = rbpitch::findNsdfPeak (t.data(), { 16, 200 });
    REQUIRE (r.tau == Catch::Approx (100.0).epsilon (0.01));
    REQUIRE (r.clarity == Catch::Approx (0.95f).epsilon (0.001));
}

TEST_CASE ("nsdf: low clarity withholds the lag but still reports confidence", "[nsdf]")
{
    // Below the 0.80 gate: breath / unvoiced consonants correlate weakly. The caller
    // needs the number (it drives currentClarity) even though the pitch is discarded.
    std::vector<float> t (201, -0.5f);
    t[99] = 0.4f; t[100] = 0.5f; t[101] = 0.4f;

    const auto r = rbpitch::findNsdfPeak (t.data(), { 16, 200 });
    REQUIRE (r.tau < 0.0f);
    REQUIRE (r.clarity == Catch::Approx (0.5f).epsilon (0.001));
}

TEST_CASE ("nsdf: parabolic interpolation refines an asymmetric peak", "[nsdf]")
{
    // Neighbours lean right, so the true peak is above the integer lag.
    std::vector<float> t (201, -0.5f);
    t[99] = 0.80f; t[100] = 1.0f; t[101] = 0.90f;

    const auto r = rbpitch::findNsdfPeak (t.data(), { 16, 200 });
    REQUIRE (r.tau > 100.0f);
    REQUIRE (r.tau < 100.5f);
}

TEST_CASE ("nsdf: no key maximum yields no detection", "[nsdf]")
{
    // Everything negative — no positive run to select at all.
    std::vector<float> t (201, -0.2f);

    const auto r = rbpitch::findNsdfPeak (t.data(), { 16, 200 });
    REQUIRE (r.tau < 0.0f);
    REQUIRE (r.clarity == 0.0f);
}

TEST_CASE ("nsdf: a degenerate lag range fails closed", "[nsdf]")
{
    std::vector<float> t (201, 1.0f);
    REQUIRE (rbpitch::findNsdfPeak (t.data(), { 100, 100 }).tau < 0.0f);
    REQUIRE (rbpitch::findNsdfPeak (t.data(), { 100,  99 }).tau < 0.0f);
}

// ─── Octave rival (the octave guard's evidence) ───────────────────────────────
//
// Every table here uses the production key threshold, so the pick the walk makes is the
// pick it would make on real audio. The shared shape is a peak at 100 that clears the
// threshold and one at 50 that does not — i.e. the picker reporting an octave too low,
// which is exactly the failure the rival search exists to detect.

namespace
{

// Baseline table: chosen peak at 100 (0.95), optional rival peak at 50 of the given
// height. keyThreshold = 0.93 x 0.95 = 0.8835, so any rival below that is skipped by the
// walk and left for the rival search to find.
std::vector<float> tableWithRival (float rivalHeight)
{
    std::vector<float> t (201, -0.5f);
    t[99] = 0.60f; t[100] = 0.95f; t[101] = 0.60f;

    if (rivalHeight > 0.0f)
    {
        t[49] = rivalHeight * 0.7f;
        t[50] = rivalHeight;
        t[51] = rivalHeight * 0.7f;
    }
    return t;
}

constexpr rbpitch::PeakParams kGuardOn { 16, 200, 0.93f, 0.80f, rbpitch::kOctaveRivalRatio };

} // namespace

TEST_CASE ("nsdf: a strong peak near half the chosen lag is reported as an octave rival", "[nsdf]")
{
    const auto t = tableWithRival (0.86f);
    const auto r = rbpitch::findNsdfPeak (t.data(), kGuardOn);

    // The walk still picks the low octave — this is the live defect, not a hypothetical.
    REQUIRE (r.tau == Catch::Approx (100.0).epsilon (0.01));

    // ...and the rival points back at the true period, interpolated like the main peak so
    // a correcting caller gets a usable lag rather than a bare integer.
    REQUIRE (r.octaveTau == Catch::Approx (50.0).epsilon (0.01));
    REQUIRE (r.octaveRatio == Catch::Approx (0.86f / 0.95f).epsilon (0.001));
}

TEST_CASE ("nsdf: a clean peak reports no octave rival", "[nsdf]")
{
    // Nothing near tau/2 — the trough a correct pick sits above. No false alarm.
    const auto t = tableWithRival (0.0f);
    const auto r = rbpitch::findNsdfPeak (t.data(), kGuardOn);

    REQUIRE (r.tau == Catch::Approx (100.0).epsilon (0.01));
    REQUIRE (r.octaveTau < 0.0f);
    REQUIRE (r.octaveRatio == 0.0f);
}

TEST_CASE ("nsdf: a weak rival reports its ratio but withholds the lag", "[nsdf]")
{
    // Present but unconvincing. The ratio is still published so a caller can show how
    // close the call was; the lag is withheld so nothing corrects onto it.
    const auto t = tableWithRival (0.70f);
    const auto r = rbpitch::findNsdfPeak (t.data(), kGuardOn);

    REQUIRE (r.octaveRatio == Catch::Approx (0.70f / 0.95f).epsilon (0.001));
    REQUIRE (r.octaveRatio < rbpitch::kOctaveRivalRatio);
    REQUIRE (r.octaveTau < 0.0f);
}

TEST_CASE ("nsdf: the octave rival search is off by default", "[nsdf]")
{
    // octaveRivalRatio defaults to 0, which skips the scan entirely — the guard costs
    // nothing and reports nothing for users who never switch it on.
    const auto t = tableWithRival (0.86f);
    const auto r = rbpitch::findNsdfPeak (t.data(), { 16, 200 });

    REQUIRE (r.tau == Catch::Approx (100.0).epsilon (0.01));
    REQUIRE (r.octaveTau < 0.0f);
    REQUIRE (r.octaveRatio == 0.0f);
}

TEST_CASE ("nsdf: an octave rival outside the walked region is not reported", "[nsdf]")
{
    // Chosen lag 34 puts half of it on top of minLag, so the rival window would read lags
    // the walk never considered. Correcting onto one would report a pitch above the
    // scanned band that this detector never validated.
    std::vector<float> t (201, -0.5f);
    t[33] = 0.60f; t[34] = 0.95f; t[35] = 0.60f;

    const auto r = rbpitch::findNsdfPeak (t.data(), kGuardOn);
    REQUIRE (r.tau == Catch::Approx (34.0).epsilon (0.01));
    REQUIRE (r.octaveTau < 0.0f);
    REQUIRE (r.octaveRatio == 0.0f);
}

TEST_CASE ("nsdf: a rising flank through the rival window is not a rival", "[nsdf]")
{
    // A broad run whose own peak (0.87) misses the key threshold, positioned so the rival
    // window catches its flank rather than its top. Height alone would qualify it; the
    // local-maximum test is what rejects it.
    std::vector<float> t (201, -0.5f);
    for (int tau = 45; tau <= 60; ++tau)
        t[(size_t) tau] = 0.30f + 0.038f * (float) (tau - 45);
    t[99] = 0.60f; t[100] = 0.95f; t[101] = 0.60f;

    const auto r = rbpitch::findNsdfPeak (t.data(), kGuardOn);
    REQUIRE (r.tau == Catch::Approx (100.0).epsilon (0.01));
    REQUIRE (r.octaveTau < 0.0f);
    REQUIRE (r.octaveRatio == 0.0f);
}

TEST_CASE ("nsdf: a rejected frame reports no octave rival", "[nsdf]")
{
    // Below the clarity gate: no lag is published, so there is no chosen octave for a
    // rival to be relative to. The rival search sits after the gate for this reason.
    std::vector<float> t (201, -0.5f);
    t[99] = 0.40f; t[100] = 0.50f; t[101] = 0.40f;
    t[49] = 0.30f; t[50] = 0.45f;  t[51] = 0.30f;

    const auto r = rbpitch::findNsdfPeak (t.data(), kGuardOn);
    REQUIRE (r.tau < 0.0f);
    REQUIRE (r.octaveTau < 0.0f);
    REQUIRE (r.octaveRatio == 0.0f);
}

TEST_CASE ("nsdf: the octave rival is found on a real octave-down waveform", "[nsdf]")
{
    // The synthetic version of the reported defect: a sawtooth whose every second period
    // is attenuated. nsdf(T) is damped by the amplitude mismatch while nsdf(2T) compares
    // like with like, so the walk skips the true period. AI stem separation produces the
    // same shape through phase smearing and residual bleed.
    //
    // The attenuation is chosen, not arbitrary. NSDF normalises by the energy of both
    // halves, so alternating by a factor a lands nsdf(T) near 2a / (1 + a^2): 0.7 gives
    // ~0.94, which still clears the 0.93 key threshold and detects correctly. 0.6 gives
    // ~0.88 — under the threshold (so the walk skips it) but over the rival ratio (so the
    // guard can see it). That window, roughly 0.85 to 0.93, IS the set of octave errors
    // this evidence can catch, and the test sits inside it deliberately.
    const double freq   = midiToHz (60);
    const double period = kDetectRate / freq;

    auto wave = sawtooth (freq, 768);
    for (int i = 0; i < (int) wave.size(); ++i)
        if (((int) ((double) i / period)) % 2 == 1)
            wave[(size_t) i] *= 0.6f;

    const auto b    = rbpitch::lagBounds (kDetectRate, kLagBufCap);
    const auto nsdf = nsdfOf (wave, b.maxLag);
    const auto r    = rbpitch::findNsdfPeak (nsdf.data(),
                                             { b.minLag, b.maxLag, 0.93f, 0.80f,
                                               rbpitch::kOctaveRivalRatio });

    REQUIRE (r.tau > 0.0f);
    const double gotHz = kDetectRate / (double) r.tau;
    INFO ("reported " << gotHz << " Hz, wanted " << freq
          << " Hz, rival ratio " << r.octaveRatio);

    // The pick is an octave low, and the rival recovers the true pitch.
    REQUIRE (gotHz == Catch::Approx (freq / 2.0).epsilon (0.03));
    REQUIRE (r.octaveTau > 0.0f);
    REQUIRE (kDetectRate / (double) r.octaveTau == Catch::Approx (freq).epsilon (0.03));
}

// ─── Synthetic waveforms across the vocal range ───────────────────────────────

TEST_CASE ("nsdf: mid-range vocal pitches detect accurately", "[nsdf]")
{
    for (int midi : { 45, 52, 60, 67, 74 })
    {
        const float want = midiToHz (midi);
        const float got  = detectHz (want);
        INFO ("MIDI " << midi);
        REQUIRE (got == Catch::Approx (want).epsilon (0.02));
    }
}

// The RB vocal range is MIDI 36-84. Before the range fix the detector covered only
// 40-82 (confirmed in REAPER 2026-08-03): MIDI 36-39 and 83-84 produced no detection at
// all, because minLag was doubling as an implicit zero-lag-lobe skip. Sweeping every
// semitone rather than spot-checking the edges is deliberate — widening the bounds moves
// the scan closer to the zero-lag lobe, so a regression would surface in the middle of
// the range, not at the ends that prompted the fix.

TEST_CASE ("nsdf: every semitone of the RB vocal range is detectable", "[nsdf]")
{
    for (int midi = 36; midi <= 84; ++midi)
    {
        const float want = midiToHz (midi);
        INFO ("MIDI " << midi << " (" << want << " Hz)");
        REQUIRE (detectHz (want) == Catch::Approx (want).epsilon (0.02));
    }
}

TEST_CASE ("nsdf: notes below the scanned band report nothing", "[nsdf]")
{
    // A signal of period T repeats at 2T, 3T... — never at T/2 — so there is no in-band
    // sub-multiple for a too-low note to fold onto. No detection is the only outcome.
    for (int midi : { 24, 30 })
    {
        INFO ("MIDI " << midi);
        REQUIRE (detectHz (midiToHz (midi)) == 0.0f);
    }
}

TEST_CASE ("nsdf: notes above the scanned band fold to an exact sub-period", "[nsdf]")
{
    // Not a defect and not fixable: a signal periodic at f is also genuinely periodic at
    // f/2, f/3..., so once f is above the band the detector reports the highest of those
    // that falls inside it. Asserting "no detection" here would be asserting something no
    // detector can deliver. Kept as a test so the *shape* of the failure stays known.
    for (int midi : { 96, 100 })
    {
        const float hz  = midiToHz (midi);
        const float got = detectHz (hz);
        INFO ("MIDI " << midi << ": " << hz << " Hz -> " << got << " Hz");

        if (got > 0.0f)
        {
            const double ratio = hz / got;
            REQUIRE (ratio >= 1.9);                                    // never reported at pitch
            REQUIRE (std::abs (ratio - std::round (ratio)) < 0.05);    // an exact sub-period
        }
    }
}

// ─── Analysis-rate sweep (hidden — run with: RBVocalSingTests.exe "[sweep]") ───
//
// Re-measures finding 1 of the 2026-08 detection-range work against MPM/NSDF. That finding
// ("16 kHz is measurably lossy; 24 kHz is the lowest lossless rate") was measured on
// YIN/CMND in Lua, and its own caveats flag the rate floor as the result most needing
// re-measurement here — MPM picks an integer lag and interpolates a different curve.
//
// Method mirrors the original: band-limited sawtooths generated directly at each analysis
// rate (modelling a correct anti-aliased resample), across noise levels and seeds. Two
// deviations, both deliberate: the range is MIDI 36-84 (the actual RB vocal range, wider
// than the 40-83 swept in Lua), and the window is a fixed 48 ms rather than a fixed
// sample count, so rate is the only variable.
//
// Decides whether step 3 (the 24 kHz decimator move) is worth doing at all.

TEST_CASE ("nsdf: analysis-rate sweep across the RB vocal range", "[.sweep]")
{
    struct RateCase { double rate; const char* note; };
    const RateCase rates[] = {
        { 14700.0, "current @ 44.1 kHz host" },
        { 16000.0, "current @ 48/96/192 kHz" },
        { 22050.0, "proposed @ 44.1 kHz host" },
        { 24000.0, "proposed @ 48/96/192 kHz" },
        { 32000.0, "reference" },
        { 48000.0, "reference (no decimation)" },
    };
    // 0 = clean. Several seeds per SNR: with only two, the miss counts came out
    // non-monotonic in rate, which is a sign of under-sampling rather than a real effect.
    const double noiseCases[] = { 0.0,
                                  12.0, 12.0, 12.0, 12.0, 12.0, 12.0,
                                   6.0,  6.0,  6.0,  6.0,  6.0,  6.0 };
    const double kWindowSec   = 0.048;

    std::cout << "\n  NSDF analysis-rate sweep - MIDI 36-84, 48 ms window, "
              << "clean + 2x12 dB + 2x6 dB SNR\n"
              << "  rate    correct  octave  wrong  missed   mean|c|   max|c|  worst  "
              << "hi-range(72-84)\n";

    for (const auto& rc : rates)
    {
        SweepStats all, hi;
        for (int midi = 36; midi <= 84; ++midi)
        {
            const float want = midiToHz (midi);
            for (size_t s = 0; s < std::size (noiseCases); ++s)
            {
                const float got = detectAtRate (want, rc.rate, kWindowSec,
                                                noiseCases[s], (uint32_t) (1000 + s));
                all.add (midi, want, got);
                if (midi >= 72) hi.add (midi, want, got);
            }
        }

        std::cout << "  " << std::setw (6) << (int) rc.rate
                  << std::setw (9) << all.correct
                  << std::setw (8) << all.octave
                  << std::setw (7) << all.wrong
                  << std::setw (8) << all.missed
                  << std::setw (10) << std::fixed << std::setprecision (2) << all.meanCents()
                  << std::setw (9) << all.maxAbsCents
                  << std::setw (7) << all.worstMidi
                  << "   " << hi.correct << "/" << (hi.correct + hi.octave + hi.wrong + hi.missed)
                  << " ok, mean " << hi.meanCents() << "c"
                  << "   (" << rc.note << ")\n";
    }
    std::cout << std::endl;

    SUCCEED ("sweep is diagnostic output, not a pass/fail assertion");
}

// Window-duration sweep at the current and proposed rates. SHORT_NOTE_HIT_TRACKING §1
// wants a shorter window to resolve short notes; finding 7 says length barely matters
// once the buffer geometry is right. Both were measured on clean or near-clean signals —
// this checks what a shorter window costs on *noisy* input, which is the case that
// decides option A (keep 48 ms) vs option C (shrink to 32 ms).
TEST_CASE ("nsdf: window-duration sweep at 16 vs 24 kHz", "[.sweep]")
{
    const double rates[]     = { 16000.0, 24000.0 };
    const double windowsMs[] = { 24.0, 32.0, 48.0, 64.0 };
    const double noiseCases[] = { 0.0,
                                  12.0, 12.0, 12.0, 12.0, 12.0, 12.0,
                                   6.0,  6.0,  6.0,  6.0,  6.0,  6.0 };

    std::cout << "\n  Window-duration sweep - MIDI 36-84, same noise mix\n"
              << "   rate   window   correct  missed   mean|c|   lo-range(36-47) missed\n";

    for (double rate : rates)
    {
        for (double winMs : windowsMs)
        {
            SweepStats all, lo;
            for (int midi = 36; midi <= 84; ++midi)
            {
                const float want = midiToHz (midi);
                for (size_t s = 0; s < std::size (noiseCases); ++s)
                {
                    const float got = detectAtRate (want, rate, winMs / 1000.0,
                                                    noiseCases[s], (uint32_t) (1000 + s));
                    all.add (midi, want, got);
                    if (midi <= 47) lo.add (midi, want, got);
                }
            }
            std::cout << "  " << std::setw (6) << (int) rate
                      << std::setw (8) << (int) winMs << " ms"
                      << std::setw (9) << all.correct
                      << std::setw (8) << all.missed
                      << std::setw (10) << std::fixed << std::setprecision (2) << all.meanCents()
                      << std::setw (14) << lo.missed << "/"
                      << (lo.correct + lo.octave + lo.wrong + lo.missed) << "\n";
        }
    }
    std::cout << std::endl;

    SUCCEED ("sweep is diagnostic output, not a pass/fail assertion");
}
