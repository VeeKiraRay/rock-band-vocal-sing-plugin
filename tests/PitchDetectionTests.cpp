#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "NsdfPeak.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <cmath>

namespace
{
    // Feeds numBlocks blocks of blockSize samples of a continuous-phase sine through
    // processBlock. Detection analyses a 768-sample window of the ~24 kHz decimated
    // stream every 256-sample hop, and decimFactor scales with the host rate — so
    // the default block count scales too: 9 blocks of 512 samples per 48 kHz = 2304
    // internal samples = the initial window fill plus exactly six hops, so
    // follow-up feeds (silence, a new tone) start on a hop boundary.
    void feedSine (RBVocalSingAudioProcessor& proc, double sampleRate, float freqHz,
                   float amplitude, int blockSize = 512, int numBlocks = -1)
    {
        if (numBlocks < 0)
            numBlocks = 9 * std::max (1, (int) (sampleRate / 48000.0));

        proc.prepareToPlay (sampleRate, blockSize);
        juce::MidiBuffer midi;
        int sampleIndex = 0;

        for (int b = 0; b < numBlocks; ++b)
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                const float sample = amplitude * std::sin (2.0f * juce::MathConstants<float>::pi
                                                             * freqHz * (float) sampleIndex / (float) sampleRate);
                buffer.setSample (0, i, sample);
                buffer.setSample (1, i, sample);
                ++sampleIndex;
            }
            proc.processBlock (buffer, midi);
        }
    }

    juce::AudioBuffer<float> silentBlock (int blockSize = 512)
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        buffer.clear();
        return buffer;
    }

    // A bare AudioProcessor under test has no host-supplied play head, so
    // currentPlaybackTime (and therefore each HopSample's timestamp) never
    // advances — every processBlock call would report the same frozen time. This
    // fake play head advances in step with fed audio so hop-queue tests see
    // realistic, monotonically increasing timestamps across block boundaries.
    struct FakePlayHead : public juce::AudioPlayHead
    {
        double         sampleRate     { 48000.0 };
        juce::int64    samplesElapsed { 0 };

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setTimeInSeconds ((double) samplesElapsed / sampleRate);
            return info;
        }
    };

    // Like feedSine, but attaches a FakePlayHead that advances with the fed audio.
    void feedSineWithPlayhead (RBVocalSingAudioProcessor& proc, FakePlayHead& playHead,
                                double sampleRate, float freqHz, float amplitude,
                                int blockSize = 512, int numBlocks = -1)
    {
        if (numBlocks < 0)
            numBlocks = 9 * std::max (1, (int) (sampleRate / 48000.0));

        proc.prepareToPlay (sampleRate, blockSize);
        playHead.sampleRate = sampleRate;
        proc.setPlayHead (&playHead);

        juce::MidiBuffer midi;
        int sampleIndex = 0;

        for (int b = 0; b < numBlocks; ++b)
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                const float sample = amplitude * std::sin (2.0f * juce::MathConstants<float>::pi
                                                             * freqHz * (float) sampleIndex / (float) sampleRate);
                buffer.setSample (0, i, sample);
                buffer.setSample (1, i, sample);
                ++sampleIndex;
            }
            proc.processBlock (buffer, midi);
            playHead.samplesElapsed += blockSize;
        }
    }
}

TEST_CASE ("pitch: 440 Hz sine at 48 kHz detects A4 (MIDI 69)", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    feedSine (proc, 48000.0, 440.0f, 0.5f);

    REQUIRE (proc.currentMidiNote.load() == 69);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (440.0f).epsilon (0.01));
}

TEST_CASE ("pitch: 110 Hz sine (low male A2) at 48 kHz detects MIDI 45", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    feedSine (proc, 48000.0, 110.0f, 0.5f);

    REQUIRE (proc.currentMidiNote.load() == 45);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (110.0f).epsilon (0.01));
}

TEST_CASE ("pitch: ~880 Hz sine at 48 kHz detects MIDI 81", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    feedSine (proc, 48000.0, 880.0f, 0.5f);

    REQUIRE (proc.currentMidiNote.load() == 81);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (880.0f).epsilon (0.01));
}

TEST_CASE ("pitch: 440 Hz sine at 44.1 kHz still detects MIDI 69 (sample-rate independence)", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    feedSine (proc, 44100.0, 440.0f, 0.5f);

    REQUIRE (proc.currentMidiNote.load() == 69);
}

TEST_CASE ("pitch: 32 kHz host runs undecimated and still detects MIDI 57", "[pitch]")
{
    // Newly reachable since the internal rate moved to 24 kHz: lround(32000/24000) = 1,
    // so a 32 kHz host is the decimFactor == 1 path — detectRate 32000, maxLag 533.
    // That is the case kMaxLagBuf (640) and kFftSize (768 + 533 = 1301) are sized
    // against, and nothing else in the suite exercises it. Under the old 16 kHz target
    // this rate decimated by 2 and never hit the undecimated path at all.
    RBVocalSingAudioProcessor proc;
    feedSine (proc, 32000.0, 220.0f, 0.5f);

    REQUIRE (proc.currentMidiNote.load() == 57);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (220.0f).epsilon (0.01));
}

TEST_CASE ("pitch: overlapping windows re-estimate every hop during a glide", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    feedSine (proc, 48000.0, 440.0f, 0.5f);   // ends exactly on a hop boundary
    REQUIRE (proc.currentMidiNote.load() == 69);

    // One hop = 256 internal samples × 2 decimation = 512 host samples (~11 ms).
    // Glide 440 → 494 Hz with continuous phase over 10 hops and count how many hops
    // publish a fresh estimate: the overlapping engine re-analyses every hop, so
    // nearly all of them must update — a non-overlapping 768-sample window could
    // only have produced ~5 estimates over the same 7680 host samples. (A hard
    // tone *switch* is deliberately not asserted here: a window mixing two tones
    // legitimately correlates best at a near-common period, so MPM's output on the
    // transition frames is ambiguous by nature.)
    juce::MidiBuffer midi;
    double phase   = 0.0;
    int    updates = 0;
    float  lastHz  = proc.currentFrequency.load();
    for (int hop = 0; hop < 10; ++hop)
    {
        juce::AudioBuffer<float> buffer (2, 768);
        for (int i = 0; i < 768; ++i)
        {
            const double t = (double) (hop * 768 + i) / 7680.0;
            const double f = 440.0 + t * (494.0 - 440.0);
            phase += juce::MathConstants<double>::twoPi * f / 48000.0;
            const float s = 0.5f * (float) std::sin (phase);
            buffer.setSample (0, i, s);
            buffer.setSample (1, i, s);
        }
        proc.processBlock (buffer, midi);

        const float hz = proc.currentFrequency.load();
        if (hz != lastHz) { ++updates; lastHz = hz; }
    }

    REQUIRE (updates >= 6);
    // The final window averages the glide's last stretch — a loose 3% is enough.
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (494.0f).epsilon (0.03));
}

TEST_CASE ("pitch: fractional MIDI pitch is published before rounding (c1)", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    proc.pitchMethod.store (2);   // MPM — the algorithm the editor fixes at startup
    // 450 Hz sits between semitones: 69 + 12*log2(450/440) ≈ 69.389.
    feedSine (proc, 48000.0, 450.0f, 0.5f);

    REQUIRE (proc.currentMidiNote.load() == 69);
    REQUIRE (proc.currentMidiFloat.load() == Catch::Approx (69.389).margin (0.05));
}

TEST_CASE ("pitch: loud unpitched noise is rejected by the clarity gate (c5)", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    juce::MidiBuffer midi;
    juce::Random rng (12345);   // fixed seed — deterministic noise

    for (int b = 0; b < 12; ++b)
    {
        juce::AudioBuffer<float> buffer (2, 512);
        for (int i = 0; i < 512; ++i)
        {
            const float s = 0.6f * (rng.nextFloat() * 2.0f - 1.0f);
            buffer.setSample (0, i, s);
            buffer.setSample (1, i, s);
        }
        proc.processBlock (buffer, midi);
    }

    // RMS ≈ 0.35, far above the energy gate — before the clarity gate this could
    // publish garbage pitches (the "breath / ssss" failure mode).
    REQUIRE (proc.currentMidiNote.load() == -1);
    REQUIRE (proc.currentClarity.load() < 0.8f);
}

TEST_CASE ("pitch: amplitude below gate threshold stays undetected", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    // Default gateThreshold is 0.01f RMS; 0.001 amplitude sine has RMS ~0.0007, well under.
    feedSine (proc, 48000.0, 440.0f, 0.001f);

    REQUIRE (proc.currentMidiNote.load() == -1);
    REQUIRE (proc.currentMidiFloat.load() == -1.0f);
}

TEST_CASE ("pitch: hold-decay keeps the note through kPitchHoldCycles then resets", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    juce::MidiBuffer midi;

    // 8 steady blocks + 1 linear fade-out block = 4608 samples = exactly two
    // detection windows, ending with the front-end filters settled. An abrupt
    // cutoff would leave the 70 Hz high-pass ringing (its output is phase-shifted,
    // so it is mid-cycle whenever the input stops) loudly enough to pass the RMS
    // gate for one window and corrupt the silence arithmetic below.
    {
        int sampleIndex = 0;
        for (int b = 0; b < 9; ++b)
        {
            juce::AudioBuffer<float> buffer (2, 512);
            for (int i = 0; i < 512; ++i)
            {
                float amp = 0.5f;
                if (b == 8)
                    amp *= (float) (512 - i) / 512.0f;
                const float sample = amp * std::sin (2.0f * juce::MathConstants<float>::pi
                                                       * 440.0f * (float) sampleIndex / 48000.0f);
                buffer.setSample (0, i, sample);
                buffer.setSample (1, i, sample);
                ++sampleIndex;
            }
            proc.processBlock (buffer, midi);
        }
    }
    REQUIRE (proc.currentMidiNote.load() == 69);

    // Feed silence until the hold expires, then assert how long it lasted in *real
    // time*. kPitchHoldCycles is specified as ≈260 ms, not as a hop count, and the hop
    // cadence moves whenever detectRate does — so asserting milliseconds keeps this
    // test meaningful across internal-rate changes instead of needing its arithmetic
    // re-derived each time (it did, twice, before this was rewritten).
    //
    // Expect ~288 ms: kPitchHoldCycles hops of hold, plus the ~3 hops it takes the tone
    // to leave the overlapping window (those windows still detect and re-arm the hold).
    // The lower bound is set to catch kPitchHoldCycles not being rescaled alongside a
    // rate change, which would land near 200 ms.
    int  silentBlocks  = 0;
    bool heldAtHalfway = false;

    while (proc.currentMidiNote.load() != -1 && silentBlocks < 200)
    {
        auto silence = silentBlock (512);
        proc.processBlock (silence, midi);
        ++silentBlocks;

        if (silentBlocks == 12)   // ~128 ms in, comfortably inside the hold
            heldAtHalfway = (proc.currentMidiNote.load() == 69);
    }

    const double heldMs = silentBlocks * 512.0 / 48000.0 * 1000.0;
    INFO ("cleared after " << silentBlocks << " silent blocks = " << heldMs << " ms");

    REQUIRE (silentBlocks < 200);   // it must actually clear, not hold forever
    REQUIRE (heldAtHalfway);
    REQUIRE (heldMs > 240.0);
    REQUIRE (heldMs < 360.0);
    REQUIRE (proc.currentMidiFloat.load() == -1.0f);
}

TEST_CASE ("pitch: prepareToPlay clears a stale partial accumulation (CODE_REVIEW.md 1.6)", "[pitch]")
{
    RBVocalSingAudioProcessor proc;

    // Leave 512 decimated samples of a 110 Hz sine sitting in the accumulator
    // (2 blocks × 512 host ÷ 2 decimation) — under the 768-sample window, so no
    // detection fires and a stale fill would survive.
    feedSine (proc, 48000.0, 110.0f, 0.5f, 512, 2);
    REQUIRE (proc.currentMidiNote.load() == -1);

    // Restart (feedSine re-runs prepareToPlay) and feed just past one window's worth
    // of 440 Hz (768 × 2 = 1536 host samples → 4 blocks). Without the reset the first
    // detection window would be part stale 110 Hz samples and misreport the pitch.
    feedSine (proc, 48000.0, 440.0f, 0.5f, 512, 4);

    REQUIRE (proc.currentMidiNote.load() == 69);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (440.0f).epsilon (0.01));
}

TEST_CASE ("pitch: 192 kHz, 110 Hz sine detects MIDI 45 (CODE_REVIEW.md 1.2)", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    proc.pitchMethod.store (0);   // Autocorrelation — the fallback whose guard this covers
    feedSine (proc, 192000.0, 110.0f, 0.5f);

    // Detection runs on the fixed ~24 kHz decimated stream (192 kHz ÷ 8), so this
    // fallback's window guard holds at any host rate by construction. The 2% frequency
    // tolerance reflects its integer-lag resolution (~0.7% per lag step at 110 Hz, ±1 lag
    // of bias) — the rounded MIDI note is the regression guard here. It also covers the
    // mean-vs-sum normalisation: without it the shrinking overlap biases this detector
    // toward short lags and it pins on minLag (MIDI 87).
    REQUIRE (proc.currentMidiNote.load() == 45);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (110.0f).epsilon (0.02));
}

TEST_CASE ("pitch: 192 kHz, 110 Hz sine with MPM (production algorithm) detects MIDI 45", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    proc.pitchMethod.store (2);   // MPM — the algorithm the editor fixes at startup
    feedSine (proc, 192000.0, 110.0f, 0.5f);

    // At the fixed ~24 kHz internal rate the 60 Hz floor needs lag ≤ 400, well inside
    // kMaxLagBuf at every host rate — MPM's clamp can't raise the detection floor.
    REQUIRE (proc.currentMidiNote.load() == 45);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (110.0f).epsilon (0.01));
}

// ─── Octave guard ─────────────────────────────────────────────────────────────
// End-to-end cover for the octave-low error a user hit charting from an AI-separated
// vocal stem: the detector reported the right pitch class an octave below the truth.

namespace
{
    // A sawtooth whose every second period is attenuated — the synthetic form of the
    // reported failure. NSDF normalises by the energy of both halves, so alternating by a
    // factor a lands nsdf(T) near 2a/(1+a^2); 0.6 gives ~0.88, which is under MPM's 0.93
    // key threshold (so the walk skips the true period and takes 2T) but over the rival
    // ratio (so the guard can see it). 0.7 would detect correctly and prove nothing.
    // The window either side of that is measured in tests/NsdfPeakTests.cpp.
    void feedAlternatingSaw (RBVocalSingAudioProcessor& proc, double sampleRate,
                             double freqHz, int numBlocks = 12, int blockSize = 512)
    {
        proc.prepareToPlay (sampleRate, blockSize);
        juce::MidiBuffer midi;

        const double period = sampleRate / freqHz;
        int sampleIndex = 0;

        for (int b = 0; b < numBlocks; ++b)
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                float s = 0.0f;
                for (int k = 1; k <= 8; ++k)
                {
                    const double f = freqHz * k;
                    if (f >= sampleRate * 0.5) break;
                    s += (float) (std::sin (2.0 * juce::MathConstants<double>::pi
                                              * f * sampleIndex / sampleRate) / k);
                }
                s *= 0.5f;
                if (((int) ((double) sampleIndex / period)) % 2 == 1)
                    s *= 0.6f;

                buffer.setSample (0, i, s);
                buffer.setSample (1, i, s);
                ++sampleIndex;
            }
            proc.processBlock (buffer, midi);
        }
    }

    constexpr double kGuardTestHz = 261.63;   // C4, MIDI 60
}

TEST_CASE ("octave guard: off reports the octave-low pitch and no rival evidence", "[pitch][octave]")
{
    RBVocalSingAudioProcessor proc;
    proc.pitchMethod.store (2);
    // octaveGuard defaults to 0 — this is the pre-guard detector, asserted unchanged.
    feedAlternatingSaw (proc, 48000.0, kGuardTestHz);

    INFO ("reported " << proc.currentFrequency.load() << " Hz");
    REQUIRE (proc.currentMidiNote.load() == 48);            // C3 — an octave below C4
    REQUIRE (proc.currentOctaveRatio.load() == 0.0f);       // search never ran
}

TEST_CASE ("octave guard: warn publishes the evidence without changing the pitch", "[pitch][octave]")
{
    RBVocalSingAudioProcessor proc;
    proc.pitchMethod.store (2);
    proc.octaveGuard.store (1);
    feedAlternatingSaw (proc, 48000.0, kGuardTestHz);

    INFO ("rival ratio " << proc.currentOctaveRatio.load());
    REQUIRE (proc.currentMidiNote.load() == 48);            // pitch untouched by design
    REQUIRE (proc.currentOctaveRatio.load() >= rbpitch::kOctaveRivalRatio);
}

TEST_CASE ("octave guard: correct reports the true octave", "[pitch][octave]")
{
    RBVocalSingAudioProcessor proc;
    proc.pitchMethod.store (2);
    proc.octaveGuard.store (2);
    feedAlternatingSaw (proc, 48000.0, kGuardTestHz);

    INFO ("reported " << proc.currentFrequency.load() << " Hz");
    REQUIRE (proc.currentMidiNote.load() == 60);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (kGuardTestHz).epsilon (0.03));

    // Corrected hops stay flagged, so the user can audit where the guard acted rather
    // than having it silently rewrite pitches.
    REQUIRE (proc.currentOctaveRatio.load() >= rbpitch::kOctaveRivalRatio);
}

TEST_CASE ("octave guard: a clean tone is left alone in every mode", "[pitch][octave]")
{
    // The guard must not manufacture corrections on input that was never ambiguous —
    // this is the false-positive check that matters most in Correct mode.
    for (int mode : { 0, 1, 2 })
    {
        RBVocalSingAudioProcessor proc;
        proc.pitchMethod.store (2);
        proc.octaveGuard.store (mode);
        feedSine (proc, 48000.0, 440.0f, 0.5f);

        INFO ("octaveGuard = " << mode);
        REQUIRE (proc.currentMidiNote.load() == 69);
        REQUIRE (proc.currentOctaveRatio.load() < rbpitch::kOctaveRivalRatio);
    }
}

TEST_CASE ("octave guard: the rival evidence rides the hop queue", "[pitch][octave][hopqueue]")
{
    // The stats chip and the trail colouring read per-hop values, not the atomic, so the
    // ratio has to survive the queue — otherwise the pinned marker and the badge disagree.
    RBVocalSingAudioProcessor proc;
    proc.pitchMethod.store (2);
    proc.octaveGuard.store (1);
    feedAlternatingSaw (proc, 48000.0, kGuardTestHz);

    RBVocalSingAudioProcessor::HopSample buf[RBVocalSingAudioProcessor::kHopQueueCapacity];
    const int n = proc.drainHopSamples (buf, RBVocalSingAudioProcessor::kHopQueueCapacity);

    REQUIRE (n > 0);
    REQUIRE (buf[n - 1].octaveRatio >= rbpitch::kOctaveRivalRatio);
}

// ─── Hop-sample queue (bug #3 fix, see _upcoming/SHORT_NOTE_HIT_TRACKING.md) ────
// The audio thread queues one HopSample per detection hop instead of only ever
// publishing the latest atomics, so the UI thread can see every hop-level
// estimate instead of point-sampling once per frame.

TEST_CASE ("hop queue: drainHopSamples returns one entry per hop, strictly time-ascending", "[pitch][hopqueue]")
{
    RBVocalSingAudioProcessor proc;
    FakePlayHead playHead;
    feedSineWithPlayhead (proc, playHead, 48000.0, 440.0f, 0.5f);   // ends on a hop boundary (see feedSine)

    RBVocalSingAudioProcessor::HopSample buf[RBVocalSingAudioProcessor::kHopQueueCapacity];
    const int n = proc.drainHopSamples (buf, RBVocalSingAudioProcessor::kHopQueueCapacity);

    REQUIRE (n > 0);
    for (int i = 1; i < n; ++i)
        REQUIRE (buf[i].time > buf[i - 1].time);

    // The window has fully settled on the tone by the last hop.
    REQUIRE (buf[n - 1].voiced);
    REQUIRE (buf[n - 1].midiFloat == Catch::Approx (69.0f).margin (0.5f));
}

TEST_CASE ("hop queue: a partial drain leaves the remaining samples for the next call", "[pitch][hopqueue]")
{
    RBVocalSingAudioProcessor proc;
    FakePlayHead playHead;
    feedSineWithPlayhead (proc, playHead, 48000.0, 440.0f, 0.5f);

    RBVocalSingAudioProcessor::HopSample first[1];
    const int n1 = proc.drainHopSamples (first, 1);
    REQUIRE (n1 == 1);

    RBVocalSingAudioProcessor::HopSample rest[RBVocalSingAudioProcessor::kHopQueueCapacity];
    const int n2 = proc.drainHopSamples (rest, RBVocalSingAudioProcessor::kHopQueueCapacity);
    REQUIRE (n2 > 0);
    REQUIRE (rest[0].time > first[0].time);   // continues in time after the first entry
}

TEST_CASE ("hop queue: overflow drops newest without corrupting the drain (UI-stall case)", "[pitch][hopqueue]")
{
    RBVocalSingAudioProcessor proc;
    FakePlayHead playHead;
    playHead.sampleRate = 48000.0;
    proc.prepareToPlay (48000.0, 512);
    proc.setPlayHead (&playHead);
    juce::MidiBuffer midi;

    // Comfortably more than kHopQueueCapacity hops' worth of audio, never drained in
    // between — simulates a stalled UI thread. Must not crash or corrupt the queue.
    int sampleIndex = 0;
    for (int b = 0; b < 200; ++b)
    {
        juce::AudioBuffer<float> buffer (2, 512);
        for (int i = 0; i < 512; ++i)
        {
            const float s = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                              * 440.0f * (float) sampleIndex / 48000.0f);
            buffer.setSample (0, i, s);
            buffer.setSample (1, i, s);
            ++sampleIndex;
        }
        proc.processBlock (buffer, midi);
        playHead.samplesElapsed += 512;
    }

    // AbstractFifo reserves one slot to disambiguate full from empty, so the usable
    // capacity is one less than kHopQueueCapacity — assert a tight bound (not the
    // exact number) so this doesn't over-specify that implementation detail.
    RBVocalSingAudioProcessor::HopSample buf[RBVocalSingAudioProcessor::kHopQueueCapacity];
    const int n = proc.drainHopSamples (buf, RBVocalSingAudioProcessor::kHopQueueCapacity);

    REQUIRE (n > RBVocalSingAudioProcessor::kHopQueueCapacity - 4);
    REQUIRE (n <= RBVocalSingAudioProcessor::kHopQueueCapacity);
    for (int i = 1; i < n; ++i)
        REQUIRE (buf[i].time > buf[i - 1].time);
}
