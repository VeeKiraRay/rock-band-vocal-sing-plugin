#include "StemAudioReader.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

// rbstem::AudioReader — the offline stem source (planned stem-analyzer work, P2).
//
// What these can and cannot cover is worth being explicit about. REAPER is not present in
// a unit-test run, so the function-pointer *signatures* cannot be checked here at all: a
// wrong name resolves to nullptr and canAnalyzeOffline() catches it, but a wrong signature
// is caught by nothing short of running inside REAPER. What is covered is everything this
// class decides on its own — the null-guards that keep an older REAPER from crashing, the
// accessor lifetime, and the double-to-float read path, which is where a silent bug would
// otherwise hide.

namespace
{
    // Stands in for REAPER: records what it was asked for, hands back a known ramp.
    namespace fake
    {
        int    accessorObject = 0;   // address stands in for an AudioAccessor*
        int    destroyCount   = 0;
        int    lastRate       = 0;
        int    lastChannels   = 0;
        int    lastNumSamples = 0;
        double lastStartSec   = 0.0;
        bool   hasAudio       = true;
        bool   stateChanged   = false;

        void reset()
        {
            destroyCount = 0; lastRate = 0; lastChannels = 0; lastNumSamples = 0;
            lastStartSec = 0.0; hasAudio = true; stateChanged = false;
        }

        void*  create   (void*)        { return &accessorObject; }
        void   destroy  (void*)        { ++destroyCount; }
        double startTime (void*)       { return 1.5; }
        double endTime   (void*)       { return 9.0; }
        bool   validate  (void*)       { return stateChanged; }

        int samples (void*, int rate, int channels, double startSec, int numSamples,
                     double* buffer)
        {
            lastRate = rate; lastChannels = channels;
            lastStartSec = startSec; lastNumSamples = numSamples;

            if (! hasAudio)
                return 0;   // REAPER leaves the buffer untouched in this case

            for (int i = 0; i < numSamples; ++i)
                buffer[i] = 0.5 * (double) (i + 1);
            return 1;
        }
    }

    ReaperApi workingApi()
    {
        ReaperApi api;
        api.CreateTrackAudioAccessor   = &fake::create;
        api.DestroyAudioAccessor       = &fake::destroy;
        api.GetAudioAccessorSamples    = &fake::samples;
        api.GetAudioAccessorStartTime  = &fake::startTime;
        api.GetAudioAccessorEndTime    = &fake::endTime;
        api.AudioAccessorValidateState = &fake::validate;
        return api;
    }

    int dummyTrack = 0;
}

TEST_CASE ("stem reader: canAnalyzeOffline needs the whole set", "[stem]")
{
    REQUIRE (ReaperApi{}.canAnalyzeOffline() == false);

    auto api = workingApi();
    REQUIRE (api.canAnalyzeOffline());

    // An older REAPER can resolve some of the set and not the rest, which is the case the
    // gate exists for — any one missing must disable the whole feature.
    auto partial = workingApi();
    partial.GetAudioAccessorSamples = nullptr;
    REQUIRE (partial.canAnalyzeOffline() == false);
}

TEST_CASE ("stem reader: refuses to open without the API or a track", "[stem]")
{
    fake::reset();
    rbstem::AudioReader reader;

    const ReaperApi empty;
    REQUIRE (reader.open (empty, &dummyTrack) == false);
    REQUIRE (reader.isOpen() == false);

    auto api = workingApi();
    REQUIRE (reader.open (api, nullptr) == false);
    REQUIRE (reader.isOpen() == false);

    // Nothing was ever created, so nothing may be destroyed.
    reader.close();
    REQUIRE (fake::destroyCount == 0);
}

TEST_CASE ("stem reader: closed reads report failure and yield silence", "[stem]")
{
    rbstem::AudioReader reader;
    float dest[4] = { 9.0f, 9.0f, 9.0f, 9.0f };

    REQUIRE (reader.readMono (0.0, 48000.0, 4, dest) == false);
    for (float v : dest)
        REQUIRE (v == 0.0f);   // silence, not whatever the caller left there
}

TEST_CASE ("stem reader: open, read, close round trip", "[stem]")
{
    fake::reset();
    auto api = workingApi();

    rbstem::AudioReader reader;
    REQUIRE (reader.open (api, &dummyTrack));
    REQUIRE (reader.isOpen());
    REQUIRE (reader.getTrack() == &dummyTrack);
    REQUIRE (reader.startTime() == Catch::Approx (1.5));
    REQUIRE (reader.endTime()   == Catch::Approx (9.0));

    float dest[4] = {};
    REQUIRE (reader.readMono (3.25, 48000.0, 4, dest));

    // Mono is requested explicitly: multi-channel stems are out of scope, and asking for
    // more channels would interleave them into the same buffer.
    REQUIRE (fake::lastChannels   == 1);
    REQUIRE (fake::lastRate       == 48000);
    REQUIRE (fake::lastNumSamples == 4);
    REQUIRE (fake::lastStartSec   == Catch::Approx (3.25));

    // REAPER hands back doubles; the conversion to float must preserve them in order.
    REQUIRE (dest[0] == Catch::Approx (0.5f));
    REQUIRE (dest[1] == Catch::Approx (1.0f));
    REQUIRE (dest[2] == Catch::Approx (1.5f));
    REQUIRE (dest[3] == Catch::Approx (2.0f));

    reader.close();
    REQUIRE (fake::destroyCount == 1);
    REQUIRE (reader.isOpen() == false);

    // Closing twice must not double-destroy.
    reader.close();
    REQUIRE (fake::destroyCount == 1);
}

TEST_CASE ("stem reader: a gap between items reads as silence, not stale audio", "[stem]")
{
    fake::reset();
    auto api = workingApi();

    rbstem::AudioReader reader;
    REQUIRE (reader.open (api, &dummyTrack));

    float dest[4] = {};
    REQUIRE (reader.readMono (3.0, 48000.0, 4, dest));
    REQUIRE (dest[3] == Catch::Approx (2.0f));

    // Now a range with no audio. REAPER returns 0 and leaves the buffer alone, so without
    // the pre-zeroing the previous block would be handed to the detector as if it were
    // real audio at this position — which the take export would then chart.
    fake::hasAudio = false;
    REQUIRE (reader.readMono (7.0, 48000.0, 4, dest) == false);
    for (float v : dest)
        REQUIRE (v == 0.0f);
}

TEST_CASE ("stem reader: reopening swaps the accessor and releases the old one", "[stem]")
{
    fake::reset();
    auto api = workingApi();
    int otherTrack = 0;

    rbstem::AudioReader reader;
    REQUIRE (reader.open (api, &dummyTrack));
    REQUIRE (reader.open (api, &otherTrack));   // open() closes the previous accessor
    REQUIRE (fake::destroyCount == 1);
    REQUIRE (reader.getTrack() == &otherTrack);
}

TEST_CASE ("stem reader: refresh reports what REAPER reports", "[stem]")
{
    fake::reset();
    auto api = workingApi();

    rbstem::AudioReader reader;
    REQUIRE (reader.refresh() == false);   // closed: nothing to validate

    REQUIRE (reader.open (api, &dummyTrack));
    REQUIRE (reader.refresh() == false);

    fake::stateChanged = true;
    REQUIRE (reader.refresh());
}

TEST_CASE ("stem reader: the destructor releases the accessor", "[stem]")
{
    fake::reset();
    auto api = workingApi();
    {
        rbstem::AudioReader reader;
        REQUIRE (reader.open (api, &dummyTrack));
    }
    REQUIRE (fake::destroyCount == 1);
}
