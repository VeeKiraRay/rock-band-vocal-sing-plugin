#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PitchDetector.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>

// rbpitch::Detector — the pitch-detection front end after it was lifted out of the
// processor (planned stem-analyzer work, P1).
//
// The extraction exists so a second instance can run over project audio without racing
// the audio thread or sharing its median history, hold counter and octave-guard run
// counter. These tests pin the two properties that makes true:
//
//   1. A standalone Detector produces exactly what processBlock produces on the same
//      samples. If the two ever diverge, an offline sweep would disagree with a
//      real-time pass over the same stem — which is the whole point of the analyzer.
//   2. Two instances are genuinely independent: feeding one does not move the other.
//
// The tolerance-based half of the comparison (offline sweep vs a real-time pass over a
// real stem, where block boundaries put the hop phase somewhere different) is a manual
// in-REAPER check.

namespace
{
    constexpr double kRate      = 48000.0;
    constexpr int    kBlockSize = 512;
    constexpr int    kNumBlocks = 9;   // window fill plus six hops, as in PitchDetectionTests

    float sineAt (int sampleIndex, float freqHz, float amplitude)
    {
        return amplitude * std::sin (2.0f * juce::MathConstants<float>::pi
                                        * freqHz * (float) sampleIndex / (float) kRate);
    }

    // Every hop the processor's own detector published, drained from the hop queue.
    std::vector<RBVocalSingAudioProcessor::HopSample>
    hopsFromProcessor (float freqHz, float amplitude)
    {
        RBVocalSingAudioProcessor proc;
        proc.pitchMethod.store (2);   // MPM — the algorithm the editor fixes at startup
        proc.prepareToPlay (kRate, kBlockSize);

        juce::MidiBuffer midi;
        int sampleIndex = 0;

        for (int b = 0; b < kNumBlocks; ++b)
        {
            juce::AudioBuffer<float> buffer (2, kBlockSize);
            for (int i = 0; i < kBlockSize; ++i)
            {
                const float s = sineAt (sampleIndex++, freqHz, amplitude);
                buffer.setSample (0, i, s);
                buffer.setSample (1, i, s);
            }
            proc.processBlock (buffer, midi);
        }

        RBVocalSingAudioProcessor::HopSample buf[RBVocalSingAudioProcessor::kHopQueueCapacity];
        const int n = proc.drainHopSamples (buf, RBVocalSingAudioProcessor::kHopQueueCapacity);
        return { buf, buf + n };
    }

    // The same audio through a bare Detector, with the params processBlock would hand it.
    std::vector<rbpitch::DetectorResult> hopsFromDetector (float freqHz, float amplitude)
    {
        rbpitch::Detector det;
        det.prepare (kRate);
        det.setParams ({});   // defaults match the processor's settings defaults

        std::vector<rbpitch::DetectorResult> out;
        for (int i = 0; i < kNumBlocks * kBlockSize; ++i)
        {
            rbpitch::DetectorResult r;
            if (det.pushSample (sineAt (i, freqHz, amplitude), r))
                out.push_back (r);
        }
        return out;
    }
}

TEST_CASE ("detector: a standalone instance matches the processBlock path exactly", "[pitch]")
{
    // A440 at a comfortable level: well clear of the gate, squarely inside the band.
    const auto viaProcessor = hopsFromProcessor (440.0f, 0.5f);
    const auto viaDetector  = hopsFromDetector  (440.0f, 0.5f);

    REQUIRE (! viaDetector.empty());
    REQUIRE (viaProcessor.size() == viaDetector.size());

    for (size_t i = 0; i < viaDetector.size(); ++i)
    {
        INFO ("hop " << i);
        // Bit-for-bit, not approximately: same code over the same samples in the same
        // order. An Approx here would hide exactly the drift this test exists to catch.
        REQUIRE (viaProcessor[i].midiFloat   == viaDetector[i].midiFloat);
        REQUIRE (viaProcessor[i].voiced      == viaDetector[i].aboveGate);
        REQUIRE (viaProcessor[i].octaveRatio == viaDetector[i].octaveRatio);
        REQUIRE (viaProcessor[i].octaveDoubt == viaDetector[i].octaveDoubt);
    }

    // Sanity: the run actually detected the tone rather than agreeing on silence.
    REQUIRE (viaDetector.back().midiFloat == Catch::Approx (69.0).margin (0.5));
}

TEST_CASE ("detector: silence agrees with the processor too", "[pitch]")
{
    // The gated path has its own state (octaveRunHops reset, hold decay, median reset),
    // so it needs pinning separately from the voiced path above.
    const auto viaProcessor = hopsFromProcessor (440.0f, 0.0f);
    const auto viaDetector  = hopsFromDetector  (440.0f, 0.0f);

    REQUIRE (viaProcessor.size() == viaDetector.size());
    REQUIRE (! viaDetector.empty());

    for (size_t i = 0; i < viaDetector.size(); ++i)
    {
        INFO ("hop " << i);
        REQUIRE (viaDetector[i].aboveGate == false);
        REQUIRE (viaDetector[i].midiFloat == -1.0f);
        REQUIRE (viaProcessor[i].midiFloat == viaDetector[i].midiFloat);
    }
}

TEST_CASE ("detector: two instances do not share state", "[pitch]")
{
    // The reason the extraction was worth doing. If these shared anything, the offline
    // analyzer would corrupt the live readout's median history and — worse — the octave
    // guard's consecutive-hop counter, making it report runs that never happened.
    rbpitch::Detector a, b;
    a.prepare (kRate);
    b.prepare (kRate);
    a.setParams ({});
    b.setParams ({});

    std::vector<rbpitch::DetectorResult> aOut;
    for (int i = 0; i < kNumBlocks * kBlockSize; ++i)
    {
        rbpitch::DetectorResult r;
        if (a.pushSample (sineAt (i, 440.0f, 0.5f), r))
            aOut.push_back (r);
    }

    REQUIRE (! aOut.empty());

    // b was never fed, so it must still be exactly as prepared: no hop can have completed.
    rbpitch::DetectorResult probe;
    REQUIRE (b.pushSample (0.0f, probe) == false);

    // And feeding b now must reproduce a's sequence from scratch, not continue it.
    std::vector<rbpitch::DetectorResult> bOut;
    b.prepare (kRate);
    for (int i = 0; i < kNumBlocks * kBlockSize; ++i)
    {
        rbpitch::DetectorResult r;
        if (b.pushSample (sineAt (i, 440.0f, 0.5f), r))
            bOut.push_back (r);
    }

    REQUIRE (bOut.size() == aOut.size());
    for (size_t i = 0; i < bOut.size(); ++i)
    {
        INFO ("hop " << i);
        REQUIRE (bOut[i].midiFloat == aOut[i].midiFloat);
    }
}

TEST_CASE ("detector: holdCycles 0 drops to silence with no hold tail", "[pitch]")
{
    // What the stem analyzer's cursor readout will need: reading a window that ends at
    // the cursor, a held value would describe audio that is not under the cursor.
    // Default holdCycles keeps republishing; 0 must not.
    const auto runWithHold = [] (int holdCycles)
    {
        rbpitch::Detector det;
        det.prepare (kRate);
        rbpitch::DetectorParams p;
        p.holdCycles = holdCycles;
        det.setParams (p);

        // Tone long enough to lock on, then silence.
        rbpitch::DetectorResult r, last;
        for (int i = 0; i < kNumBlocks * kBlockSize; ++i)
            if (det.pushSample (sineAt (i, 440.0f, 0.5f), r))
                last = r;

        REQUIRE (last.midiFloat > 0.0f);   // locked on before the silence

        // One hop's worth of silence is enough to show whether the hold engages.
        for (int i = 0; i < 4 * rbpitch::kPitchHop * 2; ++i)
            if (det.pushSample (0.0f, r))
                last = r;

        return last.midiFloat;
    };

    REQUIRE (runWithHold (rbpitch::kPitchHoldCycles) > 0.0f);   // still held
    REQUIRE (runWithHold (0) == -1.0f);                          // dropped immediately
}
