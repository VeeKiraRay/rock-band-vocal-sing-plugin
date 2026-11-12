#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

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
// The estimate is a straight line at the play rate, fitted through the last few reports
// (the mean of their position-minus-wall-clock offsets) - not a re-anchor on each report.
// Re-anchoring assumes every report is seen the moment its block starts, and a host that
// delivers blocks in bursts breaks that: on PulseAudio REAPER rendered 2048-sample blocks
// in pairs ~15 ms apart, so every other report arrived "late" and put the estimate level
// with what was already drawn. Never-backwards then held the picture for a tick, and
// 5-10 ticks a second drew nothing new (measured on an Intel N100, 2026-11-13: 37-41
// drawn of 46 ticks, uncapped). The fitted line averages the burst pattern out, so the
// picture moves on every tick and frame-to-frame unevenness roughly halves.
//
// It only ever feeds the *drawing* timeline. Scoring stays on hop timestamps and anything
// written to the project stays raw (CLAUDE.md, the three-timelines gotcha), so an estimate
// error can move the picture by a few milliseconds but can never change a score or an
// exported note.
//
// The rules, each pinned by SmoothPlayheadTests.cpp:
//   - never moves backwards while playing (a correction behind us is waited out, not shown);
//   - never runs further past the last reported position than the longest recent gap
//     between reports, so a stop, a loop end or a seek overshoots by about one block (one
//     burst, where the host bursts) at most before it is seen;
//   - a reported position far from the fitted line is a seek or a loop wrap, and snaps
//     immediately rather than being eased or waited out;
//   - falling behind (a late block) is caught up at a bounded speed, not jumped;
//   - stopped, it is exactly the reported position.
// Because the line follows the average of recent reports rather than the newest, the
// picture can sit up to about a frame behind the newest report. The audible-lead
// measurement is taken against the drawn position, so that constant is compensated with it.

namespace rbsmooth
{

// A reported position further than this from the fitted line is a jump in the transport,
// not block jitter. Jitter is a few milliseconds; the smallest deliberate jump (an
// arrow-key seek) is seconds.
constexpr double kSeekSec = 0.15;

// Upper bound on the lead, whatever the measured gaps: a host that stalls for a second
// must not let the picture run a second ahead.
constexpr double kMaxLeadSec = 0.25;

// How fast a picture that fell behind catches up, as a multiple of real time. Fast enough
// to close a one-block gap in a few frames, slow enough not to read as a jump.
constexpr double kCatchUp = 1.5;

// Reports the line is fitted through. Enough to span a few bursts of up to four blocks,
// few enough that a real change (a different buffer size) is followed within a moment.
constexpr int kFitReports = 8;

// Per-report decay of the longest-gap measure behind the lead limit. It jumps up to a
// long gap at once (the safe direction) and lets go of it over a few dozen reports.
constexpr double kGapDecay = 0.95;

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
            active   = true;
            gap      = 0.0;      // unknown until a second report arrives: no lead yet
            display  = reported;
            lastWall = nowSec;
            restart (reported, nowSec, rate);
            return display;
        }

        // The offsets are only comparable at one rate; a rate change starts a new line from
        // the current report, which is continuous with what is drawn.
        if (rate != fitRate)
            restart (anchorPos, anchorWall, rate);

        if (reported != anchorPos)
        {
            if (std::abs (reported - lineAt (nowSec)) > kSeekSec)
            {
                // A seek or a loop wrap. Start again from the new position, keeping the
                // measured gap, which the jump did not change.
                display  = reported;
                lastWall = nowSec;
                restart (reported, nowSec, rate);
                return display;
            }

            const bool firstGap = gap <= 0.0;
            gap = std::max (nowSec - anchorWall, gap * kGapDecay);
            anchorPos  = reported;
            anchorWall = nowSec;
            addOffset (reported, nowSec);

            // Until now there was no gap to lead by, so the picture sat on the first
            // report. That is the unsmoothed step, not a late block: take it in one go
            // rather than "catching up" and drawing behind the reported position.
            if (firstGap)
            {
                lastWall = nowSec;
                display  = std::max (display, reported);
                return display;
            }
        }

        const double ceiling  = anchorPos + std::min (gap, kMaxLeadSec) * std::max (rate, 0.0);
        const double estimate = std::min (lineAt (nowSec), ceiling);

        const double frameDt = std::max (0.0, nowSec - lastWall);
        lastWall = nowSec;

        if (estimate > display)
            display = std::min (estimate, display + frameDt * std::max (rate, 0.0) * kCatchUp);
        // estimate <= display: hold (never backwards); the line catches it up.

        return display;
    }

    // How far past the last report the picture may currently run (seconds of wall clock):
    // the longest recent gap between reports, capped at kMaxLeadSec. 0 = not yet known.
    double leadLimit() const noexcept { return std::min (gap, kMaxLeadSec); }

private:
    // Start a new line through one report. The wall clock is taken relative to it so the
    // offsets stay small numbers whatever the clock's epoch.
    // Leaves lastWall alone: the frame clock is not reset by a new line, so a rate change
    // cannot hand the catch-up rule an oversized frame.
    void restart (double pos, double wall, double rate) noexcept
    {
        anchorPos  = pos;
        anchorWall = wall;
        fitRate    = rate;
        origin     = wall;
        count = 0;
        next  = 0;
        addOffset (pos, wall);
    }

    void addOffset (double pos, double wall) noexcept
    {
        offsets[(std::size_t) next] = pos - (wall - origin) * fitRate;
        next  = (next + 1) % kFitReports;
        count = std::min (count + 1, kFitReports);
    }

    double lineAt (double wall) const noexcept
    {
        double sum = 0.0;
        for (int i = 0; i < count; ++i)
            sum += offsets[(std::size_t) i];
        return (wall - origin) * fitRate + sum / (double) count;
    }

    bool   active     = false;
    double anchorPos  = 0.0;   // last reported position
    double anchorWall = 0.0;   // wall clock when it was first seen
    double lastWall   = 0.0;   // wall clock at the previous update
    double gap        = 0.0;   // longest recent gap between reports, decaying
    double display    = 0.0;   // what was last returned

    // The fitted line: position = (wall - origin) * fitRate + mean(offsets).
    std::array<double, kFitReports> offsets {};
    int    count   = 0;
    int    next    = 0;
    double origin  = 0.0;
    double fitRate = 1.0;
};

} // namespace rbsmooth
