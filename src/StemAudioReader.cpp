#define NOMINMAX
#include "StemAudioReader.h"
#include <algorithm>
#include <cmath>

// Implementation notes live in StemAudioReader.h. Everything here is null-guarded twice
// over: canAnalyzeOffline() covers the whole set at open() time, and each call re-checks
// its own pointer, because an older REAPER can resolve some of the set and not the rest.

namespace rbstem
{

bool AudioReader::open (const ReaperApi& a, void* track)
{
    close();

    if (! a.canAnalyzeOffline() || track == nullptr)
        return false;

    void* created = a.CreateTrackAudioAccessor (track);
    if (created == nullptr)
        return false;

    api         = &a;
    accessor    = created;
    openedTrack = track;
    return true;
}

void AudioReader::close()
{
    if (accessor != nullptr && api != nullptr && api->DestroyAudioAccessor != nullptr)
        api->DestroyAudioAccessor (accessor);

    api         = nullptr;
    accessor    = nullptr;
    openedTrack = nullptr;
}

bool AudioReader::refresh()
{
    if (accessor == nullptr || api == nullptr || api->AudioAccessorValidateState == nullptr)
        return false;

    return api->AudioAccessorValidateState (accessor);
}

double AudioReader::startTime() const
{
    if (accessor == nullptr || api == nullptr || api->GetAudioAccessorStartTime == nullptr)
        return 0.0;

    return api->GetAudioAccessorStartTime (accessor);
}

double AudioReader::endTime() const
{
    if (accessor == nullptr || api == nullptr || api->GetAudioAccessorEndTime == nullptr)
        return 0.0;

    return api->GetAudioAccessorEndTime (accessor);
}

bool AudioReader::readMono (double startSec, double sampleRate, int numSamples, float* dest)
{
    if (dest == nullptr || numSamples <= 0)
        return false;

    if (accessor == nullptr || api == nullptr || api->GetAudioAccessorSamples == nullptr
        || sampleRate <= 0.0)
    {
        std::fill (dest, dest + numSamples, 0.0f);
        return false;
    }

    if (scratch.size() < (size_t) numSamples)
        scratch.resize ((size_t) numSamples);

    // Zero first, for two reasons: REAPER leaves the buffer alone for a range with no
    // audio (so a gap between items must read as silence rather than as whatever was
    // here last call), and if a future REAPER ever accumulated into the buffer instead of
    // overwriting, a stale block would otherwise leak into the next read.
    std::fill (scratch.begin(), scratch.begin() + numSamples, 0.0);

    const int got = api->GetAudioAccessorSamples (accessor,
                                                  (int) std::lround (sampleRate),
                                                  1,             // mono - see the header
                                                  startSec,
                                                  numSamples,
                                                  scratch.data());

    for (int i = 0; i < numSamples; ++i)
        dest[i] = (float) scratch[(size_t) i];

    return got != 0;
}

} // namespace rbstem
