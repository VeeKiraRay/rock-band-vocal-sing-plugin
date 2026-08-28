#pragma once
#include "reaper_api.h"
#include <vector>

// Reads a REAPER track's media-item audio at arbitrary project times, with no transport
// running — the source the offline stem analyzer detects from.
//
// Owns one REAPER AudioAccessor. Creating an accessor is expensive, so a caller holds one
// of these open across frames and only reopens when the target track changes; the
// destructor releases it, which is the whole reason this is a type rather than four loose
// calls. **Message thread only**, like every other REAPER API use in this project.
//
// Two properties of a *track* accessor that decide how a reading should be interpreted:
//
//   - It sees the track's media items, so it is **pre track FX**. Corrective FX on the
//     stem track is not heard here, which is usually what charting wants but is not what
//     the ear hears on playback.
//   - It is indexed in **project time**, so the play rate cannot affect a reading.

namespace rbstem
{

class AudioReader
{
public:
    AudioReader() = default;
    ~AudioReader() { close(); }

    AudioReader (const AudioReader&) = delete;
    AudioReader& operator= (const AudioReader&) = delete;

    // Opens an accessor on track (a MediaTrack*). Closes any previous one first, so
    // reopening on a new track is a single call. Returns false when the REAPER build has
    // no accessor API, when track is null, or when REAPER declines to create one.
    bool open (const ReaperApi& api, void* track);
    void close();

    bool  isOpen()   const noexcept { return accessor != nullptr; }
    void* getTrack() const noexcept { return openedTrack; }

    // True when the underlying media changed since the previous call — the caller should
    // treat anything it derived from earlier reads as stale. Cheap enough to poll.
    bool refresh();

    // Bounds of the audio this accessor can produce, in project seconds. Both return 0
    // when closed. Note the span covers every item on the track, so a stray clip far down
    // the timeline stretches endTime; a sweep should clamp against the song end rather
    // than trusting this alone.
    double startTime() const;
    double endTime()   const;

    // Reads numSamples of mono audio starting at startSec into dest, converted to float.
    // Returns false when the range holds no audio, in which case dest is filled with
    // silence rather than left undefined — which is exactly what a gap between items
    // should look like to the detector.
    //
    // Mono by request (numchannels = 1): multi-channel stems are deliberately out of
    // scope, and the live path is mono by construction too.
    bool readMono (double startSec, double sampleRate, int numSamples, float* dest);

private:
    const ReaperApi* api         { nullptr };
    void*            accessor    { nullptr };   // AudioAccessor*
    void*            openedTrack { nullptr };   // MediaTrack* the accessor was made from

    // GetAudioAccessorSamples writes doubles; this is the conversion staging buffer, kept
    // across calls so a chunked sweep does not reallocate every block.
    std::vector<double> scratch;
};

} // namespace rbstem
