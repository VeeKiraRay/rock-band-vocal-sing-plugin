#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>

TEST_CASE ("state: all 23 persisted fields round-trip through a fresh processor", "[state]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store       ((int) VocalMode::Harmonies, std::memory_order_relaxed);
    proc.gateThreshold.store   (0.0456f, std::memory_order_relaxed);
    proc.octaveGuard.store     (2, std::memory_order_relaxed);
    proc.octaveThreshold.store (0.70f, std::memory_order_relaxed);
    proc.savedFrameInterval   = 1.0 / 30.0;
    proc.savedReaperPriority  = true;
    proc.savedOctaveWrap      = false;
    proc.savedPitchTolCents   = 75;
    proc.savedPitchTrail      = false;
    proc.savedLatencyOffsetMs = 123;
    proc.savedInputOffsetMs   = 45;
    proc.savedLiveMicInput    = true;
    proc.savedPixPerSec       = 275.0f;
    proc.savedLyricScale      = 1.5f;
    proc.savedSquashGuard     = 3;
    proc.savedNoteMarkings    = 1;   // not the default, so a round-trip failure shows up
    proc.savedLyricMode       = 1;
    proc.savedRenderer        = 2;
    proc.savedStemMode        = true;
    proc.advTuningEnabled.store    (true,   std::memory_order_relaxed);
    proc.advClarityThreshold.store (0.55f,  std::memory_order_relaxed);
    proc.advMinNoteLen.store       (0.070,  std::memory_order_relaxed);
    proc.advMaxGapMerge.store      (0.140,  std::memory_order_relaxed);
    proc.advSplitHysteresis.store  (0.400,  std::memory_order_relaxed);

    juce::MemoryBlock block;
    proc.getStateInformation (block);

    // Fresh second instance proves no shared/global state is involved.
    RBVocalSingAudioProcessor proc2;
    proc2.setStateInformation (block.getData(), (int) block.getSize());

    REQUIRE (proc2.vocalMode.load()     == (int) VocalMode::Harmonies);
    REQUIRE (proc2.gateThreshold.load() == Catch::Approx (0.0456f));
    REQUIRE (proc2.octaveGuard.load()   == 2);
    REQUIRE (proc2.octaveThreshold.load() == Catch::Approx (0.70f));
    REQUIRE (proc2.savedFrameInterval   == Catch::Approx (1.0 / 30.0));
    REQUIRE (proc2.savedReaperPriority  == true);
    REQUIRE (proc2.savedOctaveWrap      == false);
    REQUIRE (proc2.savedPitchTolCents   == 75);
    REQUIRE (proc2.savedPitchTrail      == false);
    REQUIRE (proc2.savedLatencyOffsetMs == 123);
    REQUIRE (proc2.savedInputOffsetMs   == 45);
    REQUIRE (proc2.savedLiveMicInput    == true);
    REQUIRE (proc2.savedPixPerSec       == Catch::Approx (275.0f));
    REQUIRE (proc2.savedLyricScale      == Catch::Approx (1.5f));
    REQUIRE (proc2.savedSquashGuard     == 3);
    REQUIRE (proc2.savedNoteMarkings    == 1);
    REQUIRE (proc2.savedLyricMode       == 1);
    REQUIRE (proc2.savedRenderer        == 2);
    REQUIRE (proc2.savedStemMode        == true);
    REQUIRE (proc2.advTuningEnabled.load()    == true);
    REQUIRE (proc2.advClarityThreshold.load() == Catch::Approx (0.55f));
    REQUIRE (proc2.advMinNoteLen.load()       == Catch::Approx (0.070));
    REQUIRE (proc2.advMaxGapMerge.load()      == Catch::Approx (0.140));
    REQUIRE (proc2.advSplitHysteresis.load()  == Catch::Approx (0.400));
}

TEST_CASE ("state: garbage bytes are rejected without crashing, defaults intact", "[state]")
{
    RBVocalSingAudioProcessor proc;
    const std::vector<juce::uint8> garbage { 0x00, 0x01, 0xFF, 0xDE, 0xAD };

    proc.setStateInformation (garbage.data(), (int) garbage.size());

    REQUIRE (proc.vocalMode.load()     == (int) VocalMode::Solo);
    REQUIRE (proc.gateThreshold.load() == Catch::Approx (0.01f));
    REQUIRE (proc.octaveGuard.load()   == 0);
    REQUIRE (proc.savedFrameInterval   == Catch::Approx (1.0 / 60.0));
    REQUIRE (proc.savedReaperPriority  == false);
    REQUIRE (proc.savedOctaveWrap      == true);
    REQUIRE (proc.savedPitchTolCents   == 50);
    REQUIRE (proc.savedPitchTrail      == true);
    REQUIRE (proc.savedLatencyOffsetMs == 0);
    REQUIRE (proc.savedInputOffsetMs   == 0);
    REQUIRE (proc.savedLiveMicInput    == false);
    REQUIRE (proc.savedPixPerSec       == Catch::Approx (200.0f));
    REQUIRE (proc.savedLyricScale      == Catch::Approx (1.0f));
    REQUIRE (proc.savedSquashGuard     == 0);
    REQUIRE (proc.savedNoteMarkings    == 3);   // Both, unlike its neighbours here
    REQUIRE (proc.savedLyricMode       == 0);
    REQUIRE (proc.savedRenderer        == 0);
    REQUIRE (proc.savedStemMode        == false);
}

TEST_CASE ("state: a session saved before the calibration split loads unchanged", "[state]")
{
    // Sessions written before Calibration was split into Output + Input have a
    // "latencyMs" property and no "inputLatencyMs". Those must keep their calibration and
    // pick up an input offset of 0, which is the value at which detectionOffsetSec()
    // reduces to its pre-split form — so reopening an old project changes nothing.
    juce::ValueTree legacy ("RBVocalSingState");
    legacy.setProperty ("latencyMs", -60,  nullptr);
    legacy.setProperty ("liveMic",   true, nullptr);

    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (*legacy.createXml(), block);

    RBVocalSingAudioProcessor proc;
    proc.setStateInformation (block.getData(), (int) block.getSize());

    REQUIRE (proc.savedLatencyOffsetMs == -60);
    REQUIRE (proc.savedInputOffsetMs   == 0);
    REQUIRE (proc.savedLiveMicInput    == true);

    // Same reasoning for the octave guard: no "octaveGuard" property means Off, which is
    // the mode at which the detector is bit-for-bit its pre-guard self. An old session
    // must never come back with pitches being silently rewritten.
    REQUIRE (proc.octaveGuard.load()   == 0);

    // Note markings go the other way, and deliberately: an absent property means Both, not
    // the Lyric-only behaviour such a session was saved with. The pitch is the point of the
    // setting, so an old project shows it rather than hiding it behind a menu.
    REQUIRE (proc.savedNoteMarkings    == 3);
}

TEST_CASE ("state: out-of-range octave guard values are clamped, not trusted", "[state]")
{
    // Unlike the saved* settings, these are read by the audio thread and drive behaviour
    // directly, so a corrupt or future-version value has to land somewhere safe. The
    // threshold matters most: a stored 0 would disable the rival search while the UI
    // still showed the guard as on.
    juce::ValueTree bad ("RBVocalSingState");
    bad.setProperty ("octaveGuard",  99,   nullptr);
    bad.setProperty ("octaveThresh", 0.0,  nullptr);

    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (*bad.createXml(), block);

    RBVocalSingAudioProcessor proc;
    proc.setStateInformation (block.getData(), (int) block.getSize());

    REQUIRE (proc.octaveGuard.load() == 2);
    REQUIRE (proc.octaveThreshold.load()
                 == Catch::Approx (RBVocalSingAudioProcessor::kOctaveThresholdMin));
}

TEST_CASE ("state: a session saved before the octave threshold loads at the default", "[state]")
{
    juce::ValueTree legacy ("RBVocalSingState");
    legacy.setProperty ("octaveGuard", 1, nullptr);   // guard on, no threshold stored

    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (*legacy.createXml(), block);

    RBVocalSingAudioProcessor proc;
    proc.setStateInformation (block.getData(), (int) block.getSize());

    REQUIRE (proc.octaveGuard.load() == 1);
    REQUIRE (proc.octaveThreshold.load() == Catch::Approx (rbpitch::kOctaveRivalRatio));
}

TEST_CASE ("state: empty block is rejected without crashing, defaults intact", "[state]")
{
    RBVocalSingAudioProcessor proc;

    proc.setStateInformation (nullptr, 0);

    REQUIRE (proc.vocalMode.load()     == (int) VocalMode::Solo);
    REQUIRE (proc.gateThreshold.load() == Catch::Approx (0.01f));
    REQUIRE (proc.octaveGuard.load()   == 0);
    REQUIRE (proc.savedFrameInterval   == Catch::Approx (1.0 / 60.0));
    REQUIRE (proc.savedReaperPriority  == false);
    REQUIRE (proc.savedOctaveWrap      == true);
    REQUIRE (proc.savedPitchTolCents   == 50);
    REQUIRE (proc.savedPitchTrail      == true);
    REQUIRE (proc.savedLatencyOffsetMs == 0);
    REQUIRE (proc.savedInputOffsetMs   == 0);
    REQUIRE (proc.savedLiveMicInput    == false);
    REQUIRE (proc.savedPixPerSec       == Catch::Approx (200.0f));
    REQUIRE (proc.savedLyricScale      == Catch::Approx (1.0f));
    REQUIRE (proc.savedSquashGuard     == 0);
    REQUIRE (proc.savedNoteMarkings    == 3);   // Both, unlike its neighbours here
    REQUIRE (proc.savedLyricMode       == 0);
    REQUIRE (proc.savedRenderer        == 0);
    REQUIRE (proc.savedStemMode        == false);
}

// ─── Advanced tuning (expert, master-switched) ────────────────────────────────

TEST_CASE ("tuning: with the switch off the shipped constants are guaranteed", "[state][tuning]")
{
    // The structural safety property. Not "the defaults happen to match" — with the switch
    // off the tuning values are never read at all, so a future knob added with a wrong
    // default still cannot change an existing install.
    RBVocalSingAudioProcessor proc;
    REQUIRE (proc.advTuningEnabled.load() == false);

    // Values deliberately set to something absurd; none of it may reach the detector.
    proc.advClarityThreshold.store (0.05f, std::memory_order_relaxed);
    proc.advMinNoteLen.store       (0.45,  std::memory_order_relaxed);

    REQUIRE (proc.detectorParams().clarityThreshold
                 == Catch::Approx (rbpitch::kClarityThreshold));
    REQUIRE (proc.segmentationParams().minNoteLen
                 == Catch::Approx (rbtake::SegmentationParams{}.minNoteLen));
}

TEST_CASE ("tuning: the switch turns the values on for detection and export alike",
           "[state][tuning]")
{
    RBVocalSingAudioProcessor proc;
    proc.advClarityThreshold.store (0.40f, std::memory_order_relaxed);
    proc.advMinNoteLen.store       (0.05,  std::memory_order_relaxed);

    proc.advTuningEnabled.store (true, std::memory_order_relaxed);
    REQUIRE (proc.detectorParams().clarityThreshold  == Catch::Approx (0.40f));
    REQUIRE (proc.segmentationParams().minNoteLen    == Catch::Approx (0.05));
}

TEST_CASE ("tuning: the Mic gate applies whatever the switch says", "[state][tuning]")
{
    // The Settings Mic gate is not part of the Advanced panel: it is the one volume
    // control the user already knows, and a second gate there would be two controls for
    // one value. It must keep applying in both states.
    RBVocalSingAudioProcessor proc;
    proc.gateThreshold.store (0.042f, std::memory_order_relaxed);

    proc.advTuningEnabled.store (false, std::memory_order_relaxed);
    REQUIRE (proc.detectorParams().gateThreshold == Catch::Approx (0.042f));

    proc.advTuningEnabled.store (true, std::memory_order_relaxed);
    REQUIRE (proc.detectorParams().gateThreshold == Catch::Approx (0.042f));
}

TEST_CASE ("tuning: Live-mic used does not touch the tuning", "[state][tuning]")
{
    // It answers a question about the rig and gates the calibration offset. Wiring
    // detection sensitivity to it would mean flipping a timing control silently retuned
    // the detector, which is exactly why the master switch exists instead.
    RBVocalSingAudioProcessor proc;
    proc.advTuningEnabled.store    (true,  std::memory_order_relaxed);
    proc.advClarityThreshold.store (0.40f, std::memory_order_relaxed);

    proc.savedLiveMicInput = false;
    const auto offMic = proc.detectorParams().clarityThreshold;
    proc.savedLiveMicInput = true;
    const auto onMic  = proc.detectorParams().clarityThreshold;

    REQUIRE (offMic == Catch::Approx (onMic));
    REQUIRE (onMic  == Catch::Approx (0.40f));
}

TEST_CASE ("tuning: restore resets the values but leaves the switch alone", "[state][tuning]")
{
    RBVocalSingAudioProcessor proc;
    proc.advTuningEnabled.store    (true,  std::memory_order_relaxed);
    proc.advClarityThreshold.store (0.1f,  std::memory_order_relaxed);
    proc.advMinNoteLen.store       (0.45,  std::memory_order_relaxed);
    proc.advMaxGapMerge.store      (0.45,  std::memory_order_relaxed);
    proc.advSplitHysteresis.store  (1.90,  std::memory_order_relaxed);

    proc.restoreAdvancedDefaults();

    const rbtake::SegmentationParams shipped;
    REQUIRE (proc.advClarityThreshold.load() == Catch::Approx (rbpitch::kClarityThreshold));
    REQUIRE (proc.advMinNoteLen.load()       == Catch::Approx (shipped.minNoteLen));
    REQUIRE (proc.advMaxGapMerge.load()      == Catch::Approx (shipped.maxGapMerge));
    REQUIRE (proc.advSplitHysteresis.load()  == Catch::Approx (shipped.splitHysteresis));

    // The button restores what the knobs say; switching the panel off underneath the user
    // would be a surprise rather than a restore.
    REQUIRE (proc.advTuningEnabled.load() == true);
}

TEST_CASE ("tuning: a session from before the Advanced panel loads with it off",
           "[state][tuning]")
{
    juce::ValueTree legacy ("RBVocalSingState");
    legacy.setProperty ("gateThreshold", 0.045, nullptr);

    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (*legacy.createXml(), block);

    RBVocalSingAudioProcessor proc;
    proc.setStateInformation (block.getData(), (int) block.getSize());

    REQUIRE (proc.advTuningEnabled.load() == false);
    REQUIRE (proc.gateThreshold.load()    == Catch::Approx (0.045f));
    // ...and with it off, detection is exactly what that session had.
    REQUIRE (proc.detectorParams().clarityThreshold
                 == Catch::Approx (rbpitch::kClarityThreshold));
}

TEST_CASE ("tuning: out-of-range stored values are clamped, not trusted", "[state][tuning]")
{
    juce::ValueTree bad ("RBVocalSingState");
    bad.setProperty ("advClarity",   5.0,   nullptr);
    bad.setProperty ("advMinNote",   99.0,  nullptr);
    bad.setProperty ("advSplitHyst", -3.0,  nullptr);

    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (*bad.createXml(), block);

    RBVocalSingAudioProcessor proc;
    proc.setStateInformation (block.getData(), (int) block.getSize());

    REQUIRE (proc.advClarityThreshold.load() <= 1.0f);
    REQUIRE (proc.advMinNoteLen.load()       <= 0.50);
    REQUIRE (proc.advSplitHysteresis.load()  >= 0.10);
}
