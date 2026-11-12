#pragma once
#include <algorithm>
#include <cmath>

// Smooth playhead — estimate the playback position between audio blocks.
//
// Pure and JUCE-free (header-only) so the safeguards are unit-tested without an editor.
//
// While playing, the host reports a new position once per audio block. With a large buffer
// that is ~12 times a second (4096 samples at 48 kHz), so a highway drawn from the reported
// position alone steps along at 12 fps: it shows the last block's position, up to one block
// late, then jumps (found on Linux 2026-11-12; nothing about it is Linux-specific). This
// estimates the in-between positions from a wall clock, and every new block corrects it.
//
// It only ever feeds the *drawing* timeline. Scoring stays on hop timestamps and anything
// written to the project stays raw (CLAUDE.md, the three-timelines gotcha), so an estimate
// error can move the picture by a few milliseconds but can never change a score or an
// exported note.
//
// The rules, each pinned by SmoothPlayheadTests.cpp:
//   - never moves backwards while playing (a correction behind us is waited out, not shown);
//   - never runs more than one block interval past the last reported position, so a stop,
//     a loop end or a seek can overshoot by one block at most before it is seen;
//   - a reported position far from where the last block predicted is a seek or a loop wrap,
//     and snaps immediately rather than being eased or waited out;
//   - falling behind (a late block) is caught up at a bounded speed, not jumped;
//   - stopped, it is exactly the reported position.

namespace rbsmooth
{

// A reported position further than this from where the previous block predicted is a jump
// in the transport, not block jitter. Jitter is a few milliseconds; the smallest deliberate
// jump (an arrow-key seek) is seconds.
constexpr double kSeekSec = 0.15;

// Upper bound on the lead, whatever the measured block interval: a host that stalls for a
// second must not let the picture run a second ahead.
constexpr double kMaxLeadSec = 0.25;

// How fast a picture that fell behind catches up, as a multiple of real time. Fast enough
// to close a one-block gap in a few frames, slow enough not to read as a jump.
constexpr double kCatchUp = 1.5;

// Weight of the newest block interval in the running estimate. Blocks are regular, so the
// average settles quickly; the smoothing only stops one late block from widening the lead.
constexpr double kIntervalWeight = 0.2;

class Estimator
{
public:
    // reported: the position the host last reported (project seconds).
    // nowSec:   a monotonic wall clock (seconds).
    // rate:     the play rate (1.0 = normal speed).
    // playing:  false resets to the reported position.
    // Returns the position to draw.
    double update (double reported, double nowSec, double rate, bool playing) noexcept
    {
        if (! playing)
        {
            active  = false;
            display = reported;
            return display;
        }

        if (! active)
        {
            active        = true;
            anchorPos     = reported;
            anchorWall    = nowSec;
            lastWall      = nowSec;
            blockInterval = 0.0;     // unknown until a second block arrives: no lead yet
            display       = reported;
            return display;
        }

        if (reported != anchorPos)
        {
            const double interval  = nowSec - anchorWall;
            const double predicted = anchorPos + interval * rate;

            if (std::abs (reported - predicted) > kSeekSec)
            {
                // A seek or a loop wrap. Start again from the new position, keeping the
                // measured block interval, which the jump did not change.
                anchorPos  = reported;
                anchorWall = nowSec;
                lastWall   = nowSec;
                display    = reported;
                return display;
            }

            const bool firstInterval = blockInterval <= 0.0;
            blockInterval = firstInterval
                          ? interval
                          : blockInterval + kIntervalWeight * (interval - blockInterval);
            anchorPos  = reported;
            anchorWall = nowSec;

            // Until now there was no interval to lead by, so the picture sat on the first
            // report. That gap is the unsmoothed step, not a late block: take it in one go
            // rather than "catching up" and drawing behind the reported position.
            if (firstInterval)
            {
                lastWall = nowSec;
                display  = std::max (display, reported);
                return display;
            }
        }

        const double maxLead  = std::min (blockInterval, kMaxLeadSec);
        const double elapsed  = std::min (std::max (0.0, nowSec - anchorWall), maxLead);
        const double estimate = anchorPos + elapsed * rate;

        const double frameDt = std::max (0.0, nowSec - lastWall);
        lastWall = nowSec;

        if (estimate > display)
            display = std::min (estimate, display + frameDt * std::max (rate, 0.0) * kCatchUp);
        // estimate <= display: hold (never backwards); the next blocks catch it up.

        return display;
    }

    // Seconds between reported positions, as currently measured (0 = not yet known).
    double measuredBlockInterval() const noexcept { return blockInterval; }

private:
    bool   active        = false;
    double anchorPos     = 0.0;   // last reported position
    double anchorWall    = 0.0;   // wall clock when it was first seen
    double lastWall      = 0.0;   // wall clock at the previous update
    double blockInterval = 0.0;   // running average of the time between reports
    double display       = 0.0;   // what was last returned
};

} // namespace rbsmooth
