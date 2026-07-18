#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>

TEST_CASE ("state: all 13 persisted fields round-trip through a fresh processor", "[state]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store       ((int) VocalMode::Harmonies, std::memory_order_relaxed);
    proc.gateThreshold.store   (0.0456f, std::memory_order_relaxed);
    proc.savedFrameInterval   = 1.0 / 30.0;
    proc.savedReaperPriority  = true;
    proc.savedOctaveWrap      = false;
    proc.savedPitchTolCents   = 75;
    proc.savedPitchTrail      = false;
    proc.savedLatencyOffsetMs = 123;
    proc.savedPixPerSec       = 275.0f;
    proc.savedLyricScale      = 1.5f;
    proc.savedSquashGuard     = 3;
    proc.savedLyricMode       = 1;
    proc.savedRenderer        = 2;

    juce::MemoryBlock block;
    proc.getStateInformation (block);

    // Fresh second instance proves no shared/global state is involved.
    RBVocalSingAudioProcessor proc2;
    proc2.setStateInformation (block.getData(), (int) block.getSize());

    REQUIRE (proc2.vocalMode.load()     == (int) VocalMode::Harmonies);
    REQUIRE (proc2.gateThreshold.load() == Catch::Approx (0.0456f));
    REQUIRE (proc2.savedFrameInterval   == Catch::Approx (1.0 / 30.0));
    REQUIRE (proc2.savedReaperPriority  == true);
    REQUIRE (proc2.savedOctaveWrap      == false);
    REQUIRE (proc2.savedPitchTolCents   == 75);
    REQUIRE (proc2.savedPitchTrail      == false);
    REQUIRE (proc2.savedLatencyOffsetMs == 123);
    REQUIRE (proc2.savedPixPerSec       == Catch::Approx (275.0f));
    REQUIRE (proc2.savedLyricScale      == Catch::Approx (1.5f));
    REQUIRE (proc2.savedSquashGuard     == 3);
    REQUIRE (proc2.savedLyricMode       == 1);
    REQUIRE (proc2.savedRenderer        == 2);
}

TEST_CASE ("state: garbage bytes are rejected without crashing, defaults intact", "[state]")
{
    RBVocalSingAudioProcessor proc;
    const std::vector<juce::uint8> garbage { 0x00, 0x01, 0xFF, 0xDE, 0xAD };

    proc.setStateInformation (garbage.data(), (int) garbage.size());

    REQUIRE (proc.vocalMode.load()     == (int) VocalMode::Solo);
    REQUIRE (proc.gateThreshold.load() == Catch::Approx (0.01f));
    REQUIRE (proc.savedFrameInterval   == Catch::Approx (1.0 / 60.0));
    REQUIRE (proc.savedReaperPriority  == false);
    REQUIRE (proc.savedOctaveWrap      == true);
    REQUIRE (proc.savedPitchTolCents   == 50);
    REQUIRE (proc.savedPitchTrail      == true);
    REQUIRE (proc.savedLatencyOffsetMs == 0);
    REQUIRE (proc.savedPixPerSec       == Catch::Approx (200.0f));
    REQUIRE (proc.savedLyricScale      == Catch::Approx (1.0f));
    REQUIRE (proc.savedSquashGuard     == 0);
    REQUIRE (proc.savedLyricMode       == 0);
    REQUIRE (proc.savedRenderer        == 0);
}

TEST_CASE ("state: empty block is rejected without crashing, defaults intact", "[state]")
{
    RBVocalSingAudioProcessor proc;

    proc.setStateInformation (nullptr, 0);

    REQUIRE (proc.vocalMode.load()     == (int) VocalMode::Solo);
    REQUIRE (proc.gateThreshold.load() == Catch::Approx (0.01f));
    REQUIRE (proc.savedFrameInterval   == Catch::Approx (1.0 / 60.0));
    REQUIRE (proc.savedReaperPriority  == false);
    REQUIRE (proc.savedOctaveWrap      == true);
    REQUIRE (proc.savedPitchTolCents   == 50);
    REQUIRE (proc.savedPitchTrail      == true);
    REQUIRE (proc.savedLatencyOffsetMs == 0);
    REQUIRE (proc.savedPixPerSec       == Catch::Approx (200.0f));
    REQUIRE (proc.savedLyricScale      == Catch::Approx (1.0f));
    REQUIRE (proc.savedSquashGuard     == 0);
    REQUIRE (proc.savedLyricMode       == 0);
    REQUIRE (proc.savedRenderer        == 0);
}
