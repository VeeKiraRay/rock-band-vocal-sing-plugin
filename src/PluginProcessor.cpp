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

void RBVocalSingAudioProcessor::prepareToPlay (double sampleRate, int)
{
    storedSampleRate = sampleRate;

    // Keep the analysis window roughly constant in real time (~42.7 ms): 2048 samples
    // up to 48 kHz, scaling with the rate above that (96 kHz → 4096, 192 kHz → 8192).
    // Guarantees pitchBufTarget ≥ 2·maxLag up to kPitchBufCap, so the autocorrelation
    // guard can't starve at high rates (CODE_REVIEW.md 1.2).
    pitchBufTarget = juce::jlimit (kPitchBufSize, kPitchBufCap,
                                   (int) std::lround (sampleRate * (double) kPitchBufSize / 48000.0));

    // Drop any partial accumulation from before a stop/start or sample-rate change so
    // stale samples never bleed into the first detection window.
    pitchAccumFill  = 0;
    pitchHoldFrames = 0;
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
        pitchAccum[pitchAccumFill++] = src[i];

        if (pitchAccumFill >= pitchBufTarget)
        {
            const float hz = detectPitch (pitchAccum, pitchBufTarget);

            if (hz > 0.0f)
            {
                const float midi = 69.0f + 12.0f * std::log2f (hz / 440.0f);
                const int   note = std::clamp (static_cast<int> (std::roundf (midi)), 0, 127);
                currentFrequency.store (hz,   std::memory_order_relaxed);
                currentMidiFloat.store (std::clamp (midi, 0.0f, 127.0f), std::memory_order_relaxed);
                currentMidiNote .store (note, std::memory_order_relaxed);
                pitchHoldFrames = kPitchHoldCycles;
            }
            else if (pitchHoldFrames > 0)
            {
                --pitchHoldFrames;
            }
            else
            {
                currentFrequency.store (0.0f,  std::memory_order_relaxed);
                currentMidiFloat.store (-1.0f, std::memory_order_relaxed);
                currentMidiNote .store (-1,    std::memory_order_relaxed);
            }

            pitchAccumFill = 0;
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
        proc.chartStatus = "Reaper connected — reading PART VOCALS...";

        // queryInterface add-refs the returned interface. The loaded entries are plain C
        // function pointers owned by REAPER itself, not by this COM object, so the
        // reference can be dropped immediately.
        host->release();
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
