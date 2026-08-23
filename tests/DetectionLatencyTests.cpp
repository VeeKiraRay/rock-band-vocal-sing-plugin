#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

// ─── Detection latency measurement ────────────────────────────────────────────
// How late does a HopSample report a change that happened at a known project time?
//
// This exists because a user reported mid-phrase notes reading "early" while the
// phrase's first and last notes lined up after calibration. The drawing path can only
// apply one constant offset to every note, so any *difference* between a phrase-start
// onset and a mid-phrase pitch step has to come from the detection chain. These tests
// measure both, in milliseconds, so the difference is a number instead of a theory.
//
// Three separate contributions make up these figures:
//   1. window fill   — a 768-sample window spans ~32 ms, and the new content must
//                      dominate it before the NSDF locks on (inherent to the method)
//   2. window anchor — a hop's window ends at the sample that triggered it, so filing the
//                      estimate there dated it ~16 ms late. Fixed: processBlock now
//                      anchors hop timestamps at the window centre, which is what took
//                      the onset figure below from ~2 hops to ~0.5.
//   3. median-of-3   — group delay on a pitch step, and only once the history has filled:
//                      medianCount resets on every unvoiced hop, so an onset out of
//                      silence publishes its first estimate unfiltered while a legato
//                      pitch step does not.
//
// (3) is why a mid-phrase syllable is still reported ~2 hops later than a phrase-start
// onset — the asymmetry that made a single calibration value unable to suit both. About
// one of those hops is the median itself; the other is (1) being harder mid-phrase, where
// the NSDF must overcome the *previous* note still filling the window rather than silence.
// Correcting the median half in the timestamp was considered and rejected: subtracting a
// hop only on the median-active path makes the bypassed hop and the first median hop share
// a timestamp, breaking the strictly-ascending invariant the hop-queue tests assert and
// rbtake::segmentPitchTrail documents as a precondition.

namespace
{
    struct LatencyPlayHead : public juce::AudioPlayHead
    {
        double      sampleRate     { 48000.0 };
        juce::int64 samplesElapsed { 0 };

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setTimeInSeconds ((double) samplesElapsed / sampleRate);
            return info;
        }
    };

    // Feeds one continuous stream through processBlock, keeping sine phase across
    // segments so a tone change is a pure frequency step with no discontinuity (a
    // phase jump is a broadband click that the detector would legitimately mis-read).
    // Blocks are whole, so every segment boundary lands exactly on a block boundary
    // and its project time is known to the sample.
    struct Feeder
    {
        RBVocalSingAudioProcessor& proc;
        LatencyPlayHead&           playHead;
        double                     sampleRate;
        int                        blockSize;
        double                     phase      { 0.0 };
        juce::int64                samplesFed { 0 };

        Feeder (RBVocalSingAudioProcessor& p, LatencyPlayHead& ph, double sr, int bs)
            : proc (p), playHead (ph), sampleRate (sr), blockSize (bs)
        {
            proc.prepareToPlay (sampleRate, blockSize);
            playHead.sampleRate     = sampleRate;
            playHead.samplesElapsed = 0;
            proc.setPlayHead (&playHead);
        }

        double now() const { return (double) samplesFed / sampleRate; }

        // freqHz <= 0 feeds silence. fadeOut ramps the last block to zero: an abrupt
        // cutoff leaves the front end's high-pass ringing loudly enough to hold the RMS
        // gate open for an extra window and corrupt the offset measurement.
        void feed (double freqHz, float amplitude, double seconds, bool fadeOut = false)
        {
            const int blocks = (int) std::lround (seconds * sampleRate / blockSize);
            juce::MidiBuffer midi;

            for (int b = 0; b < blocks; ++b)
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                for (int i = 0; i < blockSize; ++i)
                {
                    float s = 0.0f;
                    if (freqHz > 0.0)
                    {
                        phase += juce::MathConstants<double>::twoPi * freqHz / sampleRate;
                        float amp = amplitude;
                        if (fadeOut && b == blocks - 1)
                            amp *= (float) (blockSize - i) / (float) blockSize;
                        s = amp * (float) std::sin (phase);
                    }
                    buffer.setSample (0, i, s);
                    buffer.setSample (1, i, s);
                }
                proc.processBlock (buffer, midi);
                playHead.samplesElapsed += blockSize;
                samplesFed              += blockSize;
            }
        }

        std::vector<RBVocalSingAudioProcessor::HopSample> drain()
        {
            RBVocalSingAudioProcessor::HopSample buf[RBVocalSingAudioProcessor::kHopQueueCapacity];
            const int n = proc.drainHopSamples (buf, RBVocalSingAudioProcessor::kHopQueueCapacity);
            return { buf, buf + n };
        }
    };

    constexpr double kA3Hz = 220.0;      // MIDI 57
    constexpr double kC4Hz = 261.6256;   // MIDI 60

    // First hop at or after `after` whose reported pitch is within `margin` semitones
    // of `targetMidi`. Returns -1.0 if the change is never reported.
    double firstReportedAt (const std::vector<RBVocalSingAudioProcessor::HopSample>& hops,
                            double after, float targetMidi, float margin = 0.5f)
    {
        for (const auto& h : hops)
            if (h.time >= after && h.voiced && h.midiFloat >= 0.0f
                && std::abs (h.midiFloat - targetMidi) <= margin)
                return h.time;

        return -1.0;
    }
}

// The three measurements share one feed per host rate, so the numbers are directly
// comparable and the expensive part (a few seconds of synthesized audio through the
// full front end) runs once. The queue is drained between segments because it holds
// only kHopQueueCapacity hops — the same reason updateNoteHits drains every frame.

TEST_CASE ("latency: detection chain reports onsets and legato steps late by a measurable amount",
           "[pitch][latency]")
{
    const double sampleRate = GENERATE (44100.0, 48000.0);

    RBVocalSingAudioProcessor proc;
    proc.pitchMethod.store (2);   // MPM — the algorithm the editor fixes at startup

    LatencyPlayHead playHead;
    Feeder feeder (proc, playHead, sampleRate, 512);

    // ── 1. Onset out of silence (phrase start) ────────────────────────────────
    feeder.feed (0.0, 0.0f, 0.30);          // settle the front-end filters
    feeder.drain();
    const double tOnset = feeder.now();
    feeder.feed (kA3Hz, 0.5f, 0.50);
    const auto onsetHops = feeder.drain();

    const double onsetAt = firstReportedAt (onsetHops, tOnset, 57.0f);
    REQUIRE (onsetAt >= 0.0);
    const double onsetMs = (onsetAt - tOnset) * 1000.0;

    // ── 2. Legato pitch step (mid-phrase syllable) ────────────────────────────
    // No silence between the tones, so the RMS gate never closes and the median-of-3
    // history stays full — the case where its group delay actually applies.
    const double tStep = feeder.now();
    feeder.feed (kC4Hz, 0.5f, 0.50);
    const auto stepHops = feeder.drain();

    const double stepAt = firstReportedAt (stepHops, tStep, 60.0f);
    REQUIRE (stepAt >= 0.0);
    const double stepMs = (stepAt - tStep) * 1000.0;

    // ── 3. Offset into silence (gate close) ───────────────────────────────────
    const double tOff = feeder.now();
    feeder.feed (kC4Hz, 0.5f, 0.05, true);   // fade the tail out over the last block
    feeder.feed (0.0, 0.0f, 0.30);
    const auto offHops = feeder.drain();

    double offAt = -1.0;
    for (const auto& h : offHops)
        if (h.time >= tOff && ! h.voiced) { offAt = h.time; break; }
    REQUIRE (offAt >= 0.0);
    const double offMs = (offAt - tOff - 0.05) * 1000.0;   // measured from the fade end

    WARN ("detection latency @ " << sampleRate << " Hz:"
          << "  onset " << onsetMs << " ms"
          << " | legato step " << stepMs << " ms"
          << " | step-minus-onset " << (stepMs - onsetMs) << " ms"
          << " | gate close " << offMs << " ms");

    // Bounds are stated in hops, since that is what these latencies are actually made of
    // — every figure below came out identical in hops at both host rates, and expressing
    // them in milliseconds would make the test rate-specific for no benefit.
    const double hopMs = 256.0 / (sampleRate / std::lround (sampleRate / 24000.0)) * 1000.0;

    // Causal: nothing may be reported before it happened.
    REQUIRE (onsetMs > 0.0);
    REQUIRE (stepMs  > 0.0);

    // An onset costs ~0.5 hop with the window-centre anchor in place. The upper bound is
    // set at one hop specifically to fail if that anchor is ever reverted, which would
    // put this back at ~2 hops.
    REQUIRE (onsetMs < 1.0 * hopMs);

    // A legato step costs ~2.5 hops: the ~0.5 above plus the mid-phrase penalty below.
    REQUIRE (stepMs < 3.0 * hopMs);

    // The remaining asymmetry, and the reason phrase edges and phrase interiors cannot
    // both be satisfied by one calibration number. Pinned at ~2 hops in both directions:
    // this is a documented, deliberate residual, so the test fails if it grows *or* if it
    // silently disappears (which would mean the median bypass changed and this comment,
    // and the calibration guidance in CLAUDE.md, went stale).
    REQUIRE (stepMs - onsetMs > 1.5 * hopMs);
    REQUIRE (stepMs - onsetMs < 2.5 * hopMs);
}
