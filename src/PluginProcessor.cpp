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
}

// ─── processBlock ─────────────────────────────────────────────────────────────

void RBVocalSingAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                               juce::MidiBuffer&)
{
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

        if (pitchAccumFill >= kPitchBufSize)
        {
            const float hz = detectPitch (pitchAccum, kPitchBufSize);

            if (hz > 0.0f)
            {
                const float midi = 69.0f + 12.0f * std::log2f (hz / 440.0f);
                const int   note = std::clamp (static_cast<int> (std::roundf (midi)), 0, 127);
                currentFrequency.store (hz,   std::memory_order_relaxed);
                currentMidiNote .store (note, std::memory_order_relaxed);
                pitchHoldFrames = kPitchHoldCycles;
            }
            else if (pitchHoldFrames > 0)
            {
                --pitchHoldFrames;
            }
            else
            {
                currentFrequency.store (0.0f, std::memory_order_relaxed);
                currentMidiNote .store (-1,   std::memory_order_relaxed);
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
        proc.reaperApi.load (static_cast<reaper::IReaperHostApplication*> (obj));
        proc.chartStatus = "Reaper connected — reading PART VOCALS...";
    }
}

// ─── State serialisation ──────────────────────────────────────────────────────

void RBVocalSingAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state ("RBVocalSingState");
    state.setProperty ("vocalMode",      vocalMode.load (std::memory_order_relaxed),           nullptr);
    state.setProperty ("gateThreshold",  (double) gateThreshold.load (std::memory_order_relaxed), nullptr);
    state.setProperty ("frameInterval",  savedFrameInterval,    nullptr);
    state.setProperty ("reaperPriority", savedReaperPriority,   nullptr);
    state.setProperty ("octaveWrap",     savedOctaveWrap,       nullptr);
    state.setProperty ("latencyMs",      savedLatencyOffsetMs,  nullptr);
    state.setProperty ("pixPerSec",      (double) savedPixPerSec, nullptr);
    state.setProperty ("lyricMode",      savedLyricMode,        nullptr);
    state.setProperty ("renderer",       savedRenderer,         nullptr);
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
    savedLatencyOffsetMs =        (int)    state.getProperty ("latencyMs",      0);
    savedPixPerSec       = (float)(double) state.getProperty ("pixPerSec",      200.0);
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
