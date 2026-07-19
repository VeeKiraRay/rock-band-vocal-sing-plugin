#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <cmath>

namespace
{
    // Feeds numBlocks blocks of blockSize samples of a continuous-phase sine through
    // processBlock. Detection analyses a 768-sample window of the ~16 kHz decimated
    // stream every 256-sample hop, and decimFactor scales with the host rate — so
    // the default block count scales too: 9 blocks of 512 samples per 48 kHz = 1536
    // internal samples = the initial window fill plus exactly three hops, so
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

TEST_CASE ("pitch: overlapping windows re-estimate every hop during a glide", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    feedSine (proc, 48000.0, 440.0f, 0.5f);   // ends exactly on a hop boundary
    REQUIRE (proc.currentMidiNote.load() == 69);

    // One hop = 256 internal samples × 3 decimation = 768 host samples (~16 ms).
    // Glide 440 → 494 Hz with continuous phase over 10 hops and count how many hops
    // publish a fresh estimate: the overlapping engine re-analyses every hop, so
    // nearly all of them must update — a non-overlapping 768-sample window could
    // only have produced ~3 estimates over the same 7680 host samples. (A hard
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

    // One hop = 256 internal samples × 3 (decimation at 48 kHz) = 768 host samples.
    // The first two silent detections still see sine in the overlapping window (a
    // tone takes a full window, 3 hops, to leave) and re-arm the hold; pure-silence
    // detections start at the 3rd. kPitchHoldCycles = 16 of those are tolerated, so
    // the note survives through silent block 28; the 19th silent detection — the
    // 17th pure-silence one, at host offset ~14592 during block 29 — clears it.
    for (int block = 1; block <= 28; ++block)
    {
        auto silence = silentBlock (512);
        proc.processBlock (silence, midi);

        if (block % 4 == 0)
            REQUIRE (proc.currentMidiNote.load() == 69);
    }

    for (int block = 0; block < 2; ++block)
    {
        auto silence = silentBlock (512);
        proc.processBlock (silence, midi);
    }

    REQUIRE (proc.currentMidiNote.load() == -1);
    REQUIRE (proc.currentMidiFloat.load() == -1.0f);
}

TEST_CASE ("pitch: prepareToPlay clears a stale partial accumulation (CODE_REVIEW.md 1.6)", "[pitch]")
{
    RBVocalSingAudioProcessor proc;

    // Leave 512 decimated samples of a 110 Hz sine sitting in the accumulator
    // (3 blocks × 512 host ÷ 3 decimation) — under the 768-sample window, so no
    // detection fires and a stale fill would survive.
    feedSine (proc, 48000.0, 110.0f, 0.5f, 512, 3);
    REQUIRE (proc.currentMidiNote.load() == -1);

    // Restart (feedSine re-runs prepareToPlay) and feed just past one window's worth
    // of 440 Hz (768 × 3 = 2304 host samples → 5 blocks). Without the reset the only
    // detection window would be two-thirds stale 110 Hz samples and misreport the
    // pitch.
    feedSine (proc, 48000.0, 440.0f, 0.5f, 512, 5);

    REQUIRE (proc.currentMidiNote.load() == 69);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (440.0f).epsilon (0.01));
}

TEST_CASE ("pitch: 192 kHz, 110 Hz sine detects MIDI 45 (CODE_REVIEW.md 1.2)", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    proc.pitchMethod.store (0);   // Autocorrelation — the fallback whose guard this covers
    feedSine (proc, 192000.0, 110.0f, 0.5f);

    // Detection runs on the fixed ~16 kHz decimated stream (192 kHz ÷ 12), so the
    // Autocorrelation detector's `numSamples < maxLag * 2` guard holds at any host
    // rate by construction. The 2% frequency tolerance reflects this fallback's
    // integer-lag resolution (~0.7% per lag step at 110 Hz, ±1 lag of bias) — the
    // rounded MIDI note is the regression guard here.
    REQUIRE (proc.currentMidiNote.load() == 45);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (110.0f).epsilon (0.02));
}

TEST_CASE ("pitch: 192 kHz, 110 Hz sine with MPM (production algorithm) detects MIDI 45", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    proc.pitchMethod.store (2);   // MPM — the algorithm the editor fixes at startup
    feedSine (proc, 192000.0, 110.0f, 0.5f);

    // At the fixed ~16 kHz internal rate the 80 Hz floor needs lag ≤ 200, well inside
    // kMaxLagBuf at every host rate — MPM's clamp can't raise the detection floor.
    REQUIRE (proc.currentMidiNote.load() == 45);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (110.0f).epsilon (0.01));
}
