#define NOMINMAX
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "NsdfPeak.h"
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
void RBVocalSingAudioProcessor::prepareToPlay (double sampleRate, int)
{
    // Every front-end decision — decimation factor, filter design, and clearing the
    // partial accumulation so stale samples cannot bleed across a stop/start or a
    // sample-rate change — belongs to the detector now.
    detector.prepare (sampleRate);
    hostSampleRate = sampleRate;

    hopFifo.reset();
}

// SPSC push/drain pair backing the audio-thread -> UI-thread hop queue (see the
// HopSample comment in the header). Standard AbstractFifo pattern: prepareToWrite/
// prepareToRead may hand back a wrapped (start1/size1 + start2/size2) range.

void RBVocalSingAudioProcessor::pushHopSample (double time, float midiFloat, bool voiced,
                                                float octaveRatio, bool octaveDoubt) noexcept
{
    int start1, size1, start2, size2;
    hopFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 > 0)
        hopBuffer[(size_t) start1] = { time, midiFloat, voiced, octaveRatio, octaveDoubt };
    // size1 == 0 means the fifo is full (UI thread stalled) — drop this sample.
    hopFifo.finishedWrite (size1);
}

int RBVocalSingAudioProcessor::drainHopSamples (HopSample* dest, int maxCount) noexcept
{
    int start1, size1, start2, size2;
    hopFifo.prepareToRead (maxCount, start1, size1, start2, size2);
    int n = 0;
    for (int i = 0; i < size1; ++i) dest[n++] = hopBuffer[(size_t) (start1 + i)];
    for (int i = 0; i < size2; ++i) dest[n++] = hopBuffer[(size_t) (start2 + i)];
    hopFifo.finishedRead (size1 + size2);
    return n;
}

// ─── processBlock ─────────────────────────────────────────────────────────────

void RBVocalSingAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                               juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (buffer.getNumChannels() == 0 || buffer.getNumSamples() == 0)
        return;

    // Audio-thread cost instrumentation (RB_PITCH_PROFILE). Two timer reads per block
    // plus one per detection (~62/s) — QueryPerformanceCounter, tens of ns, no syscall
    // and no allocation. Compiled out entirely by default.
#if RB_PITCH_PROFILE
    const juce::int64 profBlockStart = juce::Time::getHighResolutionTicks();
#endif

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
    lastBlockSize.store (numSamples, std::memory_order_relaxed);

    // Snapshot the settings atomics into the detector once per block rather than once per
    // hop. A settings change is a user action at UI rate, so at most one block (~10 ms)
    // separates the two — and it keeps the detector free of atomics, which is what lets a
    // second instance run at a different sensitivity.
    detector.setParams (detectorParams());

    // Per-hop timestamps for the HopSample queue: blockStartTime corresponds to
    // sample 0 of this buffer (currentPlaybackTime, just updated above or carried
    // over from a previous block), offset by the sample index within the block.
    const double blockStartTime = currentPlaybackTime.load (std::memory_order_relaxed);

    // A hop's window *ends* at the sample that triggered it, so its estimate describes
    // audio centred half a window earlier. Filing it at the trailing edge dated every
    // detection ~16 ms late, which showed up as a pitch trail — and an exported take —
    // lagging the audio it came from. Anchor at the window centre instead. This removes
    // the geometric half of the analysis delay; the rest (how much of the window the new
    // content must fill before the NSDF locks onto it) is inherent to the method and
    // stays. Measured before/after in tests/DetectionLatencyTests.cpp.
    const double windowCentreOffset = detector.windowCentreSeconds();

    for (int i = 0; i < numSamples; ++i)
    {
#if RB_PITCH_PROFILE
        const juce::int64 profDetStart = juce::Time::getHighResolutionTicks();
#endif
        rbpitch::DetectorResult det;
        if (! detector.pushSample (src[i], det))
            continue;

#if RB_PITCH_PROFILE
        {
            // Times the whole hop-completing pushSample call, so it now includes one
            // sample of front-end filtering on top of the analysis it used to measure —
            // tens of nanoseconds against a figure in the hundreds of microseconds.
            //
            // Note the clock is now read once per *sample* rather than once per hop,
            // because whether a hop completed is only known after the call. That costs
            // ~512 extra timer reads per block, which lands in profBlockLoad below at
            // roughly 0.1% of the deadline — immaterial, but it is a floor under that
            // number that did not exist before. RB_PITCH_PROFILE is off by default.
            const double ms = (juce::Time::getHighResolutionTicks() - profDetStart) * 1000.0
                                  / (double) juce::Time::getHighResolutionTicksPerSecond();
            profDetectMs.store (0.9 * profDetectMs.load (std::memory_order_relaxed) + 0.1 * ms,
                                std::memory_order_relaxed);
            profDetectCount.fetch_add (1, std::memory_order_relaxed);
        }
#endif

        audioAboveGate    .store (det.aboveGate,   std::memory_order_relaxed);
        currentClarity    .store (det.clarity,     std::memory_order_relaxed);
        currentOctaveRatio.store (det.octaveRatio, std::memory_order_relaxed);
        currentOctaveDoubt.store (det.octaveDoubt, std::memory_order_relaxed);
        currentFrequency  .store (det.hz,          std::memory_order_relaxed);
        currentMidiFloat  .store (det.midiFloat,   std::memory_order_relaxed);
        currentMidiNote   .store (det.midiNote,    std::memory_order_relaxed);

        // Publish this hop's result (voiced, held, or silent) so the UI thread can see
        // every estimate instead of point-sampling the latest atomics once per frame —
        // see the HopSample comment in the header.
        // Clamped so a hop near the very start of a project can't be dated before it.
        // Several hops can share the clamped value there; consumers already tolerate
        // repeated hop times (a stopped transport reports the same time every block).
        const double hopTime = std::max (0.0, blockStartTime + (double) i / hostSampleRate
                                                  - windowCentreOffset);
        pushHopSample (hopTime, det.midiFloat, det.aboveGate,
                       det.octaveRatio, det.octaveDoubt);
    }

#if RB_PITCH_PROFILE
    {
        const double sec = (juce::Time::getHighResolutionTicks() - profBlockStart)
                               / (double) juce::Time::getHighResolutionTicksPerSecond();
        const double budget = (double) numSamples / hostSampleRate;
        if (budget > 0.0)
            profBlockLoad.store (0.9 * profBlockLoad.load (std::memory_order_relaxed)
                                     + 0.1 * (sec / budget),
                                 std::memory_order_relaxed);
    }
#endif
}

// ─── Tuning selection ─────────────────────────────────────────────────────────
// One place where "is this a live mic?" turns into concrete numbers, so the three
// detection paths (processBlock, the cursor readout, the batch sweep) and the take export
// cannot drift apart.

rbpitch::DetectorParams RBVocalSingAudioProcessor::detectorParams() const
{
    rbpitch::DetectorParams p;
    p.method          = pitchMethod.load     (std::memory_order_relaxed);
    p.octaveGuard     = octaveGuard.load     (std::memory_order_relaxed);
    p.octaveThreshold = octaveThreshold.load (std::memory_order_relaxed);

    // The Mic gate applies to every source, tuning or no tuning — it is the one volume
    // control the user already knows, and duplicating it in the Advanced panel would be
    // two controls for one value.
    p.gateThreshold = gateThreshold.load (std::memory_order_relaxed);

    // Read only when the switch is on, so the shipped constant is not merely the default
    // but the guaranteed value while the panel is off.
    p.clarityThreshold = advTuningEnabled.load (std::memory_order_relaxed)
                            ? advClarityThreshold.load (std::memory_order_relaxed)
                            : rbpitch::kClarityThreshold;

    return p;   // holdCycles left at its default; callers that need 0 set it themselves
}

rbtake::SegmentationParams RBVocalSingAudioProcessor::segmentationParams() const
{
    rbtake::SegmentationParams p;   // the shipped constants

    // Same switch as detection, so the panel has one rule and one visible state.
    if (advTuningEnabled.load (std::memory_order_relaxed))
    {
        p.minNoteLen      = advMinNoteLen.load      (std::memory_order_relaxed);
        p.maxGapMerge     = advMaxGapMerge.load     (std::memory_order_relaxed);
        p.splitHysteresis = advSplitHysteresis.load (std::memory_order_relaxed);
    }

    return p;
}

void RBVocalSingAudioProcessor::restoreAdvancedDefaults()
{
    const rbtake::SegmentationParams seg;   // shipped values, the single source of truth

    advClarityThreshold.store (rbpitch::kClarityThreshold,  std::memory_order_relaxed);
    advMinNoteLen.store       (seg.minNoteLen,              std::memory_order_relaxed);
    advMaxGapMerge.store      (seg.maxGapMerge,             std::memory_order_relaxed);
    advSplitHysteresis.store  (seg.splitHysteresis,         std::memory_order_relaxed);
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
    state.setProperty ("octaveGuard",    octaveGuard.load (std::memory_order_relaxed),          nullptr);
    state.setProperty ("octaveThresh",   (double) octaveThreshold.load (std::memory_order_relaxed), nullptr);
    state.setProperty ("frameInterval",  savedFrameInterval.load(),    nullptr);
    state.setProperty ("reaperPriority", savedReaperPriority.load(),   nullptr);
    state.setProperty ("octaveWrap",     savedOctaveWrap.load(),       nullptr);
    state.setProperty ("pitchTolCents",  savedPitchTolCents.load(),    nullptr);
    state.setProperty ("pitchTrail",     savedPitchTrail.load(),       nullptr);
    state.setProperty ("smoothPlayhead", savedSmoothPlayhead.load(),   nullptr);
    state.setProperty ("latencyMs",      savedLatencyOffsetMs.load(),  nullptr);
    state.setProperty ("inputLatencyMs", savedInputOffsetMs.load(),    nullptr);
    state.setProperty ("liveMic",        savedLiveMicInput.load(),     nullptr);
    state.setProperty ("advEnabled",     advTuningEnabled.load (std::memory_order_relaxed),             nullptr);
    state.setProperty ("advClarity",     (double) advClarityThreshold.load (std::memory_order_relaxed), nullptr);
    state.setProperty ("advMinNote",     advMinNoteLen.load (std::memory_order_relaxed),      nullptr);
    state.setProperty ("advGapMerge",    advMaxGapMerge.load (std::memory_order_relaxed),     nullptr);
    state.setProperty ("advSplitHyst",   advSplitHysteresis.load (std::memory_order_relaxed), nullptr);
    state.setProperty ("pixPerSec",      (double) savedPixPerSec.load(), nullptr);
    state.setProperty ("lyricScale",     (double) savedLyricScale.load(), nullptr);
    state.setProperty ("squashGuard",    savedSquashGuard.load(),      nullptr);
    state.setProperty ("noteMarkings",   savedNoteMarkings.load(),     nullptr);
    state.setProperty ("lyricMode",      savedLyricMode.load(),        nullptr);
    state.setProperty ("stemMode",      savedStemMode.load(),         nullptr);
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
    // Absent in sessions saved before the octave guard; 0 is Off, so those load unchanged.
    octaveGuard.store   (juce::jlimit (0, 2, (int) state.getProperty ("octaveGuard", 0)),      std::memory_order_relaxed);
    octaveThreshold.store (juce::jlimit (kOctaveThresholdMin, kOctaveThresholdMax,
                                         (float) (double) state.getProperty ("octaveThresh",
                                                                             (double) rbpitch::kOctaveRivalRatio)),
                           std::memory_order_relaxed);
    savedFrameInterval   =        (double) state.getProperty ("frameInterval",  1.0 / 60.0);
    savedReaperPriority  =        (bool)   state.getProperty ("reaperPriority", false);
    savedOctaveWrap      =        (bool)   state.getProperty ("octaveWrap",     true);
    savedPitchTolCents   =        (int)    state.getProperty ("pitchTolCents",  50);
    savedPitchTrail      =        (bool)   state.getProperty ("pitchTrail",     true);
    savedSmoothPlayhead  =        (bool)   state.getProperty ("smoothPlayhead", false);
    savedLatencyOffsetMs =        (int)    state.getProperty ("latencyMs",      0);
    // Absent in sessions saved before the calibration split; 0 reproduces the old
    // single-slider behaviour exactly, so those sessions load with no visible change.
    savedInputOffsetMs   =        (int)    state.getProperty ("inputLatencyMs", 0);
    savedLiveMicInput    =        (bool)   state.getProperty ("liveMic",        false);

    // Absent key means off, which is the state in which none of the tuning below is read —
    // so a session from before the Advanced panel loads with detection untouched.
    advTuningEnabled.store ((bool) state.getProperty ("advEnabled", false),
                            std::memory_order_relaxed);
    advClarityThreshold.store (juce::jlimit (0.0f, 1.0f,
                                (float) (double) state.getProperty ("advClarity",
                                                    (double) rbpitch::kClarityThreshold)),
                               std::memory_order_relaxed);
    advMinNoteLen.store (juce::jlimit (0.02, 0.50,
                            (double) state.getProperty ("advMinNote",
                                        rbtake::SegmentationParams{}.minNoteLen)),
                         std::memory_order_relaxed);
    advMaxGapMerge.store (juce::jlimit (0.0, 0.50,
                            (double) state.getProperty ("advGapMerge",
                                        rbtake::SegmentationParams{}.maxGapMerge)),
                          std::memory_order_relaxed);
    advSplitHysteresis.store (juce::jlimit (0.10, 2.00,
                                (double) state.getProperty ("advSplitHyst",
                                            rbtake::SegmentationParams{}.splitHysteresis)),
                              std::memory_order_relaxed);
    savedPixPerSec       = (float)(double) state.getProperty ("pixPerSec",      200.0);
    savedLyricScale      = (float)(double) state.getProperty ("lyricScale",     1.0);
    savedSquashGuard     =        (int)    state.getProperty ("squashGuard",    0);
    savedNoteMarkings    =        (int)    state.getProperty ("noteMarkings",   3);
    savedLyricMode       =        (int)    state.getProperty ("lyricMode",      0);
    savedStemMode        =        (bool)   state.getProperty ("stemMode",      false);
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
