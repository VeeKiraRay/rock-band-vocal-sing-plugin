#pragma once
#include <algorithm>
#include <array>
#include <cmath>

// Audible lead — how far the position the editor draws from runs ahead of what is heard.
//
// Pure and JUCE-free (header-only) so the filter is unit-tested without REAPER.
//
// While playing, the editor's position comes from processBlock, i.e. from audio being
// *rendered*. REAPER renders ahead of what is *heard*: by the output buffer, and on tracks
// not monitoring live input by anticipative FX processing (~200 ms by default). Found
// 2026-11-12 as a step back on every stop: switching anticipative FX off cut it from ~200 ms
// to the 20-50 ms of the output buffer. REAPER's GetPlayPosition() is the latency-
// compensated "what you hear" position, so the lead is measured as
//
//     lead = position drawn this frame - GetPlayPosition()
//
// once per ~20 Hz poll while playing. Samples are noisy in two known ways - the two
// positions are read a moment apart, and a seek moves one before the other - so the value
// is the median of the recent ones, with implausible samples discarded outright.

namespace rblead
{

// Samples kept for the median. At the ~20 Hz poll that is about half a second of history:
// long enough that one or two seek transients cannot move it, short enough to follow a
// change of track monitoring within a moment.
constexpr int kWindow = 9;

// A sample further than this from zero is a seek or a loop wrap caught mid-flight, not a
// latency. Measured 2026-11-12 (Windows, 48 kHz): 52 ms at 128-sample blocks and 305 ms at
// 2048 with anticipative FX off; 270 ms and 675 ms with it on - the lead grows with the
// buffer, so a 4096 buffer with anticipative FX can pass a second. Two seconds leaves room
// for that while still rejecting real seeks, which the median handles below this anyway.
constexpr double kMaxPlausibleSec = 2.0;

// Samples needed before the value is reported at all.
constexpr int kMinSamples = 3;

// Samples ignored after a reset: the first polls after playback starts catch REAPER
// before its play position and the rendered audio have both settled.
constexpr int kSettleSamples = 2;

class LeadFilter
{
public:
    void reset() noexcept
    {
        count = 0;
        next  = 0;
        skip  = kSettleSamples;
    }

    void push (double sample) noexcept
    {
        if (skip > 0) { --skip; return; }
        if (! std::isfinite (sample) || std::abs (sample) > kMaxPlausibleSec) return;

        ring[(size_t) next] = sample;
        next  = (next + 1) % kWindow;
        count = std::min (count + 1, kWindow);
        last  = sample;
    }

    bool hasValue() const noexcept { return count >= kMinSamples; }

    // Median of the samples held. Only meaningful when hasValue().
    double value() const noexcept
    {
        std::array<double, kWindow> sorted {};
        std::copy (ring.begin(), ring.begin() + count, sorted.begin());
        std::sort (sorted.begin(), sorted.begin() + count);
        return (count % 2 == 1) ? sorted[(size_t) (count / 2)]
                                : 0.5 * (sorted[(size_t) (count / 2 - 1)] + sorted[(size_t) (count / 2)]);
    }

    // The most recent accepted sample, for diagnostics.
    double lastSample() const noexcept { return last; }

private:
    std::array<double, kWindow> ring {};
    int    count = 0;
    int    next  = 0;
    int    skip  = kSettleSamples;
    double last  = 0.0;
};

} // namespace rblead
