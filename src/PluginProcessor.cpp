#define NOMINMAX
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>

// Core processor: lifecycle, the realtime processBlock, and the REAPER API handover.
// Pitch detection lives in PitchDetection.cpp; chart reading/parsing in ChartLoader.cpp.

// DEF_CLASS_IID must appear in exactly one translation unit.
JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4100)
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wnon-virtual-dtor", "-Wzero-as-null-pointer-constant")
namespace reaper {
    using namespace Steinberg;
    DEF_CLASS_IID (IReaperHostApplication)
}
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
JUCE_END_IGNORE_WARNINGS_MSVC

// ─── Startup ──────────────────────────────────────────────────────────────────

// RBJ-cookbook biquad coefficients, Q = 1/√2. The cutoff is clamped below Nyquist so
// a degenerate host rate can't produce an unstable filter.
void RBVocalSingAudioProcessor::setBiquadLP (Biquad& f, double sampleRate, double freq)
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

void RBVocalSingAudioProcessor::setBiquadHP (Biquad& f, double sampleRate, double freq)
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

void RBVocalSingAudioProcessor::prepareToPlay (double sampleRate, int)
{
    // Fixed ~16 kHz internal detection rate by integer decimation: 44.1/48 kHz ÷ 3,
    // 96 kHz ÷ 6, 176.4 kHz ÷ 11, 192 kHz ÷ 12 → detectRate ≈ 14.7–16.04 kHz. The
    // window and lag ranges are in internal samples, so the detection floor and
    // cadence survive every host rate without buffer scaling.
    decimFactor = std::max (1, (int) std::lround (sampleRate / 16000.0));
    detectRate  = sampleRate / (double) decimFactor;

    // Front-end conditioning at the host rate: high-pass against rumble/plosives,
    // two cascaded low-passes as the decimator's anti-alias filter (the internal
    // Nyquist is ≥ 7.35 kHz at the rates above; NSDF needs nothing past ~2 kHz).
    setBiquadHP (hpFilter,  sampleRate, 70.0);
    setBiquadLP (lpFilter1, sampleRate, 6000.0);
    setBiquadLP (lpFilter2, sampleRate, 6000.0);
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
}

// ─── processBlock ─────────────────────────────────────────────────────────────

void RBVocalSingAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                               juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (buffer.getNumChannels() == 0 || buffer.getNumSamples() == 0)
        return;

    // Playback time fallback for non-Reaper hosts (or until API handover fires)
    if (auto* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            const auto t = pos->getTimeInSeconds();
            if (t.hasValue())
                currentPlaybackTime.store (*t, std::memory_order_relaxed);
        }
    }

    const float* src        = buffer.getReadPointer (0);
    const int    numSamples = buffer.getNumSamples();

    for (int i = 0; i < numSamples; ++i)
    {
        // Condition at the host rate, then keep every decimFactor-th sample for the
        // ~16 kHz detection pipeline (see the front-end block in the header).
        const float conditioned = lpFilter2.process (lpFilter1.process (hpFilter.process (src[i])));

        if (++decimPhase < decimFactor)
            continue;
        decimPhase = 0;

        pitchRing[ringPos] = conditioned;
        if (++ringPos == kPitchWindow) ringPos = 0;
        if (ringFilled < kPitchWindow) ++ringFilled;

        if (++hopCount >= kPitchHop && ringFilled == kPitchWindow)
        {
            hopCount = 0;

            // Linearise the ring (the oldest sample sits at ringPos) so the
            // detectors see one contiguous window.
            const int tail = kPitchWindow - ringPos;
            std::copy (pitchRing + ringPos, pitchRing + kPitchWindow, windowScratch);
            std::copy (pitchRing, pitchRing + ringPos, windowScratch + tail);

            const float hz = detectPitch (windowScratch, kPitchWindow);
            currentClarity.store (hz > 0.0f ? lastDetectClarity : 0.0f,
                                  std::memory_order_relaxed);

            if (hz > 0.0f)
            {
                // Median-of-3 over the accepted estimates (see medianHist); until
                // the history fills, the newest value passes through unfiltered.
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
                const int   note = std::clamp (static_cast<int> (std::roundf (midi)), 0, 127);
                currentFrequency.store (pubHz, std::memory_order_relaxed);
                currentMidiFloat.store (std::clamp (midi, 0.0f, 127.0f), std::memory_order_relaxed);
                currentMidiNote .store (note,  std::memory_order_relaxed);
                pitchHoldFrames = kPitchHoldCycles;
            }
            else
            {
                // Unvoiced frame: restart the median history so no stale estimate
                // bridges across a gap into the next onset.
                medianCount = 0;

                if (pitchHoldFrames > 0)
                {
                    --pitchHoldFrames;
                }
                else
                {
                    currentFrequency.store (0.0f,  std::memory_order_relaxed);
                    currentMidiFloat.store (-1.0f, std::memory_order_relaxed);
                    currentMidiNote .store (-1,    std::memory_order_relaxed);
                }
            }
        }
    }
}

// ─── Reaper API handover ──────────────────────────────────────────────────────

void RBVocalSingAudioProcessor::Vst3Ext::setIHostApplication (Steinberg::FUnknown* ptr)
{
    if (ptr == nullptr)
        return;

    void* obj = nullptr;
    if (ptr->queryInterface (reaper::IReaperHostApplication::iid, &obj) == Steinberg::kResultOk
        && obj != nullptr)
    {
        auto* host = static_cast<reaper::IReaperHostApplication*> (obj);
        proc.reaperApi.load (host);
        proc.chartStatus = "Reaper connected - reading PART VOCALS...";

        // Keep the interface (queryInterface add-ref'd it; released in the processor
        // destructor): the plugin's parent track is queried on demand via
        // getReaperParent(1) — during instantiation the FX may not yet be attached to
        // its track, so capturing the track here can come back null.
        if (proc.reaperHost != nullptr)
            proc.reaperHost->release();
        proc.reaperHost = host;
    }
}

// ─── State serialisation ──────────────────────────────────────────────────────

void RBVocalSingAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state ("RBVocalSingState");
    state.setProperty ("vocalMode",      vocalMode.load (std::memory_order_relaxed),           nullptr);
    state.setProperty ("gateThreshold",  (double) gateThreshold.load (std::memory_order_relaxed), nullptr);
    state.setProperty ("frameInterval",  savedFrameInterval.load(),    nullptr);
    state.setProperty ("reaperPriority", savedReaperPriority.load(),   nullptr);
    state.setProperty ("octaveWrap",     savedOctaveWrap.load(),       nullptr);
    state.setProperty ("pitchTolCents",  savedPitchTolCents.load(),    nullptr);
    state.setProperty ("pitchTrail",     savedPitchTrail.load(),       nullptr);
    state.setProperty ("latencyMs",      savedLatencyOffsetMs.load(),  nullptr);
    state.setProperty ("pixPerSec",      (double) savedPixPerSec.load(), nullptr);
    state.setProperty ("lyricScale",     (double) savedLyricScale.load(), nullptr);
    state.setProperty ("squashGuard",    savedSquashGuard.load(),      nullptr);
    state.setProperty ("lyricMode",      savedLyricMode.load(),        nullptr);
    state.setProperty ("renderer",       savedRenderer.load(),         nullptr);
    auto xml = state.createXml();
    copyXmlToBinary (*xml, destData);
}

void RBVocalSingAudioProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr) return;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid() || state.getType() != juce::Identifier ("RBVocalSingState")) return;

    vocalMode.store     ((int)    state.getProperty ("vocalMode",      (int) VocalMode::Solo), std::memory_order_relaxed);
    gateThreshold.store ((float)  (double) state.getProperty ("gateThreshold", 0.01),          std::memory_order_relaxed);
    savedFrameInterval   =        (double) state.getProperty ("frameInterval",  1.0 / 60.0);
    savedReaperPriority  =        (bool)   state.getProperty ("reaperPriority", false);
    savedOctaveWrap      =        (bool)   state.getProperty ("octaveWrap",     true);
    savedPitchTolCents   =        (int)    state.getProperty ("pitchTolCents",  50);
    savedPitchTrail      =        (bool)   state.getProperty ("pitchTrail",     true);
    savedLatencyOffsetMs =        (int)    state.getProperty ("latencyMs",      0);
    savedPixPerSec       = (float)(double) state.getProperty ("pixPerSec",      200.0);
    savedLyricScale      = (float)(double) state.getProperty ("lyricScale",     1.0);
    savedSquashGuard     =        (int)    state.getProperty ("squashGuard",    0);
    savedLyricMode       =        (int)    state.getProperty ("lyricMode",      0);
    savedRenderer        =        (int)    state.getProperty ("renderer",       0);
}

// ─── Boilerplate ──────────────────────────────────────────────────────────────

juce::AudioProcessorEditor* RBVocalSingAudioProcessor::createEditor()
{
    return new RBVocalSingAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RBVocalSingAudioProcessor();
}
