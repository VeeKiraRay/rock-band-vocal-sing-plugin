#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <cmath>

namespace
{
    // Feeds numBlocks blocks of blockSize samples of a continuous-phase sine through
    // processBlock. The detection window scales with the sample rate (2048 samples up
    // to 48 kHz, 8192 at 192 kHz), so the default block count scales too: 8 blocks of
    // 512 samples per 48 kHz — comfortably past one accumulator fill at any rate.
    void feedSine (RBVocalSingAudioProcessor& proc, double sampleRate, float freqHz,
                   float amplitude, int blockSize = 512, int numBlocks = -1)
    {
        if (numBlocks < 0)
            numBlocks = 8 * std::max (1, (int) (sampleRate / 48000.0));

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

TEST_CASE ("pitch: amplitude below gate threshold stays undetected", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    // Default gateThreshold is 0.01f RMS; 0.001 amplitude sine has RMS ~0.0007, well under.
    feedSine (proc, 48000.0, 440.0f, 0.001f);

    REQUIRE (proc.currentMidiNote.load() == -1);
}

TEST_CASE ("pitch: hold-decay keeps the note through kPitchHoldCycles then resets", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    feedSine (proc, 48000.0, 440.0f, 0.5f);
    REQUIRE (proc.currentMidiNote.load() == 69);

    juce::MidiBuffer midi;

    // 2048-sample accumulator / 512-sample blocks = 4 blocks per detection pass.
    // kPitchHoldCycles = 6 detection passes of silence are tolerated = 24 silent blocks.
    for (int block = 1; block <= 24; ++block)
    {
        auto silence = silentBlock (512);
        proc.processBlock (silence, midi);

        if (block % 4 == 0)
            REQUIRE (proc.currentMidiNote.load() == 69);
    }

    // One more accumulator-fill (4 more blocks) pushes past the hold budget.
    for (int block = 0; block < 4; ++block)
    {
        auto silence = silentBlock (512);
        proc.processBlock (silence, midi);
    }

    REQUIRE (proc.currentMidiNote.load() == -1);
}

TEST_CASE ("pitch: prepareToPlay clears a stale partial accumulation (CODE_REVIEW.md 1.6)", "[pitch]")
{
    RBVocalSingAudioProcessor proc;

    // Leave 1536 samples of a 110 Hz sine sitting in the accumulator — under the
    // 2048-sample window, so no detection fires and a stale fill would survive.
    feedSine (proc, 48000.0, 110.0f, 0.5f, 512, 3);
    REQUIRE (proc.currentMidiNote.load() == -1);

    // Restart (feedSine re-runs prepareToPlay) and feed exactly one window's worth of
    // 440 Hz. Without the reset the only detection window would be three-quarters
    // stale 110 Hz samples and misreport the pitch.
    feedSine (proc, 48000.0, 440.0f, 0.5f, 512, 4);

    REQUIRE (proc.currentMidiNote.load() == 69);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (440.0f).epsilon (0.01));
}

TEST_CASE ("pitch: 192 kHz, 110 Hz sine detects MIDI 45 (CODE_REVIEW.md 1.2)", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    feedSine (proc, 192000.0, 110.0f, 0.5f);

    // The detection window scales with the sample rate, so the Autocorrelation
    // detector's `numSamples < maxLag * 2` guard no longer starves at 192 kHz.
    REQUIRE (proc.currentMidiNote.load() == 45);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (110.0f).epsilon (0.01));
}

TEST_CASE ("pitch: 192 kHz, 110 Hz sine with MPM (production algorithm) detects MIDI 45", "[pitch]")
{
    RBVocalSingAudioProcessor proc;
    proc.pitchMethod.store (2);   // MPM — the algorithm the editor fixes at startup
    feedSine (proc, 192000.0, 110.0f, 0.5f);

    // kMaxLagBuf covers the full 80 Hz floor at 192 kHz (lag 2400), so MPM's clamp no
    // longer silently raises the minimum detectable frequency to ~128 Hz.
    REQUIRE (proc.currentMidiNote.load() == 45);
    REQUIRE (proc.currentFrequency.load() == Catch::Approx (110.0f).epsilon (0.01));
}
