#include "PluginEditor.h"
#include "EditorVisuals.h"

// The scrolling note highway plus the stats and status-bar panels below it. Shared
// colours and the lyric-suffix helper live in EditorVisuals.h. The lyric strips are
// in PluginEditorLyrics.cpp; the frame loop and paint dispatcher in PluginEditor.cpp.

// Pitch-class-aware wrap: maps an out-of-range MIDI note back into [minP, maxP].
// Cycles through in-range chart notes that share the same pitch class (by octave index),
// so C4 maps to C2 and C5 maps to C3 when both C2 and C3 are in the chart — matching
// the "same note name in the next octave" intuition. Falls back to a raw modular-semitone
// wrap if no same-pitch-class note exists in the visible range. The candidate pool is
// precomputed at chart load (getPitchClassCandidates); the [minP, maxP] clip must stay
// per-call because the two callers pass different ranges (the arrowhead uses the
// animated section range, the traffic light the whole chart range).
static int wrapPitchIntoRange (int P, int minP, int maxP,
                                const std::array<std::vector<int>, 12>& candsByPc)
{
    const int pc = ((P % 12) + 12) % 12;

    const auto& all = candsByPc[(size_t) pc];
    const auto  lo  = std::lower_bound (all.begin(), all.end(), minP);
    const auto  hi  = std::upper_bound (lo, all.end(), maxP);

    if (lo != hi)
    {
        const int N    = (int) (hi - lo);
        const int base = *lo;
        const int idx  = (((P - base) / 12) % N + N) % N;
        return *(lo + idx);
    }

    // Fallback: modular semitone wrap
    const int range  = maxP - minP + 1;
    const int offset = (((P - minP) % range) + range) % range;
    return minP + offset;
}

// ─── Highway ──────────────────────────────────────────────────────────────────

void RBVocalSingAudioProcessorEditor::drawHighway (juce::Graphics& g,
                                                    juce::Rectangle<int> area,
                                                    double t)
{
    g.setColour (kHighwayBg);
    g.fillRect  (area);

    const auto& notes = audioProcessor.getChartNotes();

    if (notes.empty())
    {
        g.setColour (juce::Colour (0xffffcc44));  // amber/attention
        g.setFont   (juce::Font (15.0f, juce::Font::bold));
        const bool apiReady = audioProcessor.reaperApi.isLoaded();
        const auto& status  = audioProcessor.chartStatus;
        juce::String msg;
        if (apiReady)
            msg = status.isNotEmpty()
                    ? "No notes found for selected mode - check status bar"
                    : "Reaper connected - waiting for project to load";
        else
            msg = "No MIDI loaded - click \"Load MIDI File\" in Settings";
        g.drawText (msg, area, juce::Justification::centred, false);
        return;
    }

    // Compute the visible pitch range for the current section, with a smooth
    // transition on section change driven by playback time (no extra state needed).
    // Smoothstep interpolation over kTransitionDur seconds after each boundary.
    static constexpr int kPitchPad = 5;

    float fMinP = (float) audioProcessor.chartMinPitch;
    float fMaxP = (float) audioProcessor.chartMaxPitch;

    const auto& sections = audioProcessor.getChartPitchSections();
    for (int si = 0; si < (int) sections.size(); ++si)
    {
        const auto& sec = sections[si];
        if (t < sec.startTime || t >= sec.endTime) continue;

        const float tgtMin = (float) std::max (0,   sec.minPitch - kPitchPad);
        const float tgtMax = (float) std::min (127, sec.maxPitch + kPitchPad);

        if (si > 0 && t < sec.startTime + sec.transitionDur)
        {
            const auto& prev   = sections[si - 1];
            const float srcMin = (float) std::max (0,   prev.minPitch - kPitchPad);
            const float srcMax = (float) std::min (127, prev.maxPitch + kPitchPad);

            const float a = (float) (t - sec.startTime) / sec.transitionDur;
            const float s = a * a * (3.0f - 2.0f * a);  // smoothstep ease in/out

            fMinP = srcMin + s * (tgtMin - srcMin);
            fMaxP = srcMax + s * (tgtMax - srcMax);
        }
        else
        {
            fMinP = tgtMin;
            fMaxP = tgtMax;
        }
        break;
    }

    const float phX   = (float) area.getX() + (float) area.getWidth() * 0.3f;
    const float noteH = (float) area.getHeight() / (fMaxP - fMinP + 1.0f);

    auto pitchY = [&] (int p) -> float {
        return (float) area.getBottom() - ((float) p - fMinP + 0.5f) * noteH;
    };

    // Cache this frame's geometry so mouse hit-testing (hitTestNote) matches exactly
    // what's on screen — fMinP/fMaxP animate on section changes, so we must not recompute.
    lastHighwayArea = area;
    lastFMinP = fMinP;
    lastFMaxP = fMaxP;
    lastNoteH = noteH;

    // Lane grid
    g.setColour (juce::Colour (0xff111126));
    for (int p = (int) std::floor (fMinP); p <= (int) std::ceil (fMaxP); p += 2)
    {
        const float y = (float) area.getBottom() - ((float) p - fMinP) * noteH;
        g.drawHorizontalLine ((int) y, (float) area.getX(), (float) area.getRight());
    }

    // Pitch range labels (top = highest, bottom = lowest)
    {
        g.setFont (10.0f);
        g.setColour (juce::Colour (0x99ffffff));
        const int maxP = (int) std::round (fMaxP);
        const int minP = (int) std::round (fMinP);
        const auto makeName = [&] (int p) -> juce::String {
            return juce::String (kNoteNames[p % 12]) + juce::String (p / 12 - 1);
        };
        // Top label: centre on the top note's Y, clamp inside area
        const float topY = pitchY (maxP);
        g.drawText (makeName (maxP),
                    area.getX() + 3, juce::jmax (area.getY(), (int) topY - 5), 28, 10,
                    juce::Justification::centredLeft, false);
        // Bottom label: centre on the bottom note's Y, clamp inside area
        const float botY = pitchY (minP);
        g.drawText (makeName (minP),
                    area.getX() + 3, juce::jmin (area.getBottom() - 10, (int) botY - 5), 28, 10,
                    juce::Justification::centredLeft, false);
    }

    // Playhead
    g.setColour (kPlayheadCol);
    g.drawVerticalLine ((int) phX, (float) area.getY(), (float) area.getBottom());

    // Note bars, bend connectors, and lyric labels — clipped to highway area.
    // Rendered one part at a time so bend-connector tracking stays per-part and
    // the z-order (Harm3 → Harm2 → Main) keeps the lead on top.
    g.saveState();
    g.reduceClipRegion (area);
    g.setFont (10.0f);

    // Visible time window. Notes are sorted by startTime, so each part can binary-search
    // to the first potentially-visible note and stop once notes scroll past the right
    // edge — O(visible) per frame instead of O(all notes), independent of song length.
    const double visibleLeft = t + (double) (area.getX()    - phX) / pixPerSec;
    const double leftTime    = visibleLeft - kNoteLookback;
    const double rightTime   = t + (double) (area.getRight() - phX) / pixPerSec;

    auto drawNotesForPart = [&] (VocalPart part)
    {
        const juce::Colour partCol = colourForPart (part);
        float prevX2 = 0.0f, prevCy = 0.0f, prevBarH = 0.0f;
        bool  prevValid = false;

        auto startIt = std::lower_bound (notes.begin(), notes.end(), leftTime,
            [] (const NoteInfo& n, double v) { return n.startTime < v; });

        // Extend backward for any note that started before the lookback window but is
        // still sounding (endTime extends past the visible left edge). Vocal notes don't
        // overlap, so once a note's endTime is before visibleLeft all earlier ones are
        // too — the walk exits in O(1) in the normal case.
        while (startIt != notes.begin())
        {
            auto prev = std::prev (startIt);
            if (prev->endTime <= visibleLeft)
                break;
            startIt = prev;
        }

        // Include one extra earlier note of this part: a "+" bend note needs its
        // predecessor as the connector's left anchor even after that predecessor has
        // fully scrolled past the visible edge — the anchor's geometry is tracked
        // without being drawn, so the diagonal still renders. (CODE_REVIEW.md 1.7)
        for (auto extra = startIt; extra != notes.begin(); )
        {
            --extra;
            if (extra->part == part)
            {
                startIt = extra;
                break;
            }
        }

        for (auto it = startIt; it != notes.end(); ++it)
        {
            const auto& n = *it;
            if (n.startTime > rightTime) break;
            if (n.part != part) continue;

            const float x1   = phX + (float) (n.startTime - t) * pixPerSec;
            const float x2   = phX + (float) (n.endTime   - t) * pixPerSec;
            const float cy   = pitchY (n.pitch);
            const float barH = (noteH - 1.0f) * n.thickScale;
            // "+" is a pitch-bend connector; "+$" (hidden) must bend identically —
            // displayLyric has the suffixes pre-stripped (finalizeChart render cache).
            const bool  bend = (n.displayLyric == "+");

            // Diagonal connector bridging the previous note to this bend note
            if (bend && prevValid)
            {
                juce::Path connector;
                connector.startNewSubPath (prevX2, prevCy - prevBarH * 0.5f);
                connector.lineTo          (x1,     cy     - barH     * 0.5f);
                connector.lineTo          (x1,     cy     + barH     * 0.5f);
                connector.lineTo          (prevX2, prevCy + prevBarH * 0.5f);
                connector.closeSubPath();
                g.setColour (partCol);
                g.fillPath  (connector);
            }

            // Track geometry for the next potential bend connector (always, even if off-screen)
            prevX2 = x2;  prevCy = cy;  prevBarH = barH;  prevValid = true;

            if (x2 < (float) area.getX() || x1 > (float) area.getRight()) continue;

            const float drawX1 = std::max (x1, (float) area.getX());
            const float drawX2 = std::min (x2, (float) area.getRight());

            if (n.isTalkie)
            {
                g.setColour (partCol.withAlpha (0.25f));
                g.fillRect (drawX1, (float) area.getY(), drawX2 - drawX1, (float) area.getHeight());
            }
            else
            {
                g.setColour (partCol);
                g.fillRect (drawX1, cy - barH * 0.5f, drawX2 - drawX1, barH);
            }

            // Inline bar labels: Main part only — HARM2/HARM3 lyrics live in the top strip.
            if (part == VocalPart::Main
                && !bend && n.lyric.isNotEmpty()
                && x1 >= (float) area.getX() && (x2 - x1) > 4.0f)
            {
                const juce::String& displayLyric = n.displayLyric;
                if (displayLyric.isNotEmpty())
                {
                    const float labelY = n.isTalkie ? (float) area.getY() + 2.0f
                                                     : (int) (cy - barH * 0.5f) - 13.0f;
                    g.setColour (juce::Colour (0xffcce8ff));
                    g.drawText  (displayLyric,
                                 (int) x1,
                                 (int) labelY,
                                 juce::jmax (40, (int) (x2 - x1)),
                                 12,
                                 juce::Justification::centredLeft, false);
                }
            }
        }
    };

    if (harmoniesMode())
    {
        drawNotesForPart (VocalPart::Harm3);
        drawNotesForPart (VocalPart::Harm2);
    }
    drawNotesForPart (VocalPart::Main);

    g.restoreState();

    // Range Shift boundary markers — always visible so section detection can be verified
    {
        for (int si = 1; si < (int) sections.size(); ++si)
        {
            const float bx = phX + (float) (sections[si].startTime - t) * pixPerSec;
            if (bx < (float) area.getX() || bx > (float) area.getRight()) continue;

            g.setColour (juce::Colour (0x88ff8800));
            g.drawVerticalLine ((int) bx, (float) area.getY(), (float) area.getBottom());

            const float prevMid = (sections[si - 1].minPitch + sections[si - 1].maxPitch) * 0.5f;
            const float nextMid = (sections[si].minPitch     + sections[si].maxPitch)     * 0.5f;
            const bool  goUp    = (nextMid > prevMid + 0.5f);
            const bool  goDown  = (nextMid < prevMid - 0.5f);

            g.setColour (juce::Colour (0xffff8800));
            const float ty = (float) area.getY() + 6.0f;
            if (goUp || (!goUp && !goDown))
            {
                juce::Path tri;
                tri.addTriangle (bx, ty, bx - 5.0f, ty + 9.0f, bx + 5.0f, ty + 9.0f);
                g.fillPath (tri);
            }
            if (goDown || (!goUp && !goDown))
            {
                const float dy = (!goUp && !goDown) ? ty + 12.0f : ty;
                juce::Path tri;
                tri.addTriangle (bx, dy + 9.0f, bx - 5.0f, dy, bx + 5.0f, dy);
                g.fillPath (tri);
            }
        }
    }

    // Phrase separator lines
    {
        const auto& separators = audioProcessor.getChartPhraseSeparators();
        g.setColour (kPhraseSepColour);
        for (const double sepTime : separators)
        {
            const float x = phX + (float) (sepTime - t) * pixPerSec;
            if (x < (float) area.getX() || x > (float) area.getRight()) continue;
            g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());
        }
    }

    // Arrowhead for live pitch — white when in range; amber when octave-wrapped out of range
    const int  userNote = audioProcessor.currentMidiNote.load (std::memory_order_relaxed);
    if (userNote >= 0)
    {
        const bool inRange  = ((float) userNote >= fMinP && (float) userNote <= fMaxP);
        const bool doWrap   = octaveWrap && !inRange;
        const int  drawNote = doWrap
                                  ? wrapPitchIntoRange (userNote, (int) fMinP, (int) fMaxP,
                                                        audioProcessor.getPitchClassCandidates())
                                  : userNote;

        if (inRange || doWrap)
        {
            const float cx = phX;
            const float cy = pitchY (drawNote);

            // Tip on the playhead line, body trailing left — matches how the game
            // renders the sung-pitch arrow. The 6 px glow stroke below is centred on
            // the path, and its mitered corner at this sharp tip spikes to
            // halfWidth / sin(tip half-angle) = 3 / sin(atan(10/20)) ≈ 6.7 px past the
            // geometric tip — pull the path back by that much to stay behind the line.
            const float tipX = cx - 6.0f;

            juce::Path arrow;
            arrow.startNewSubPath (tipX,          cy);
            arrow.lineTo          (tipX - 20.0f,  cy - 10.0f);
            arrow.lineTo          (tipX - 20.0f,  cy + 10.0f);
            arrow.closeSubPath();

            const juce::Colour fillColour = doWrap ? juce::Colour (0xffffaa44)
                                                   : juce::Colours::white;
            g.setColour (kNoteColour.withAlpha (0.35f));
            g.strokePath (arrow, juce::PathStrokeType (6.0f));
            g.setColour  (fillColour);
            g.fillPath   (arrow);
            g.setColour  (kNoteColour);
            g.strokePath (arrow, juce::PathStrokeType (1.5f));
        }
    }

}

// ─── Stats panel ──────────────────────────────────────────────────────────────

void RBVocalSingAudioProcessorEditor::drawStats (juce::Graphics& g,
                                                  juce::Rectangle<int> area,
                                                  double t)
{
    g.setColour (kStatsBg);
    g.fillRect  (area);

    g.setColour (juce::Colour (0xff222233));
    g.drawHorizontalLine (area.getY(), (float) area.getX(), (float) area.getRight());

    const int   note = audioProcessor.currentMidiNote  .load (std::memory_order_relaxed);
    const float hz   = audioProcessor.currentFrequency .load (std::memory_order_relaxed);

    // Content is capped at kWindowW so it doesn't stretch with the window. The top
    // kEditRowH is reserved for the "Allow edits" toggle component (laid out in resized()),
    // so the time/pitch/light row paints below it.
    const int contentW = std::min (area.getWidth(), kWindowW);
    auto row = area.withTrimmedTop (kEditRowH).withWidth (contentW).reduced (8, 6);

    // ── Time ────────────────────────────────────────────────────────────────
    auto timeCol = row.removeFromLeft (200);
    const auto fmtTime = [] (double sec) -> juce::String {
        const int totalMs = (int) (sec * 1000.0);
        return juce::String::formatted ("%02d:%02d.%03d",
                                        totalMs / 60000,
                                        (totalMs % 60000) / 1000,
                                        totalMs % 1000);
    };
    juce::String timeStr = fmtTime (t);
    const double endTime = audioProcessor.songEndTime;
    if (endTime > 0.0)
        timeStr += " / " + fmtTime (endTime);

    g.setColour (juce::Colours::white);
    g.setFont   (11.0f);
    g.drawText  (timeStr, timeCol, juce::Justification::centredLeft, false);

    // ── Traffic light ────────────────────────────────────────────────────────
    auto lightCol = row.removeFromRight (70);

    // Score against the closest active note of ANY part: green if the singer
    // matches any harmony line currently sounding. When octave wrapping is on,
    // an out-of-range note is mapped to its in-range pitch-class equivalent first.
    const int minP = audioProcessor.chartMinPitch;
    const int maxP = audioProcessor.chartMaxPitch;
    const int scoringNote = (note >= 0 && octaveWrap
                             && (note < minP || note > maxP))
                                ? wrapPitchIntoRange (note, minP, maxP,
                                                      audioProcessor.getPitchClassCandidates())
                                : note;

    // Scan all active notes unconditionally (even when no audio is detected) so that
    // silence during any active note correctly scores as OFF (red), not --- (grey).
    bool activeTalkie  = false;
    bool activeRegular = false;
    int  bestDiff      = -1;

    // Window the scan like drawNotesForPart instead of walking from the first note of
    // the song every frame (PERFORMANCE_IMPROVEMENT.md c3): jump to the earliest note
    // within kNoteLookback, then extend backwards while the previous note is still
    // sounding — same non-overlap assumption drawNotesForPart documents.
    const auto& chartNotes = audioProcessor.getChartNotes();
    auto scanIt = std::lower_bound (chartNotes.begin(), chartNotes.end(), t - kNoteLookback,
        [] (const NoteInfo& n, double v) { return n.startTime < v; });
    while (scanIt != chartNotes.begin())
    {
        const auto prev = std::prev (scanIt);
        if (prev->endTime <= t) break;
        scanIt = prev;
    }

    for (auto it = scanIt; it != chartNotes.end(); ++it)
    {
        const auto& n = *it;
        if (n.startTime > t + 0.1) break;
        if (n.startTime <= t && t < n.endTime)
        {
            if (n.isTalkie)
            {
                activeTalkie = true;
            }
            else
            {
                activeRegular = true;
                if (scoringNote >= 0)
                {
                    const int diff = std::abs (scoringNote - n.pitch);
                    if (bestDiff < 0 || diff < bestDiff) bestDiff = diff;
                }
            }
        }
    }

    juce::Colour lightColour = juce::Colours::darkgrey;
    juce::String lightLabel  = "---";

    if (activeTalkie && !activeRegular)
    {
        // Talkie only: presence detection — any audible sound is a hit
        if (audioProcessor.audioAboveGate.load (std::memory_order_relaxed))
            { lightColour = juce::Colours::limegreen; lightLabel = "GREAT";  }
        else
            { lightColour = juce::Colours::red;       lightLabel = "MISS"; }
    }
    else if (activeRegular)
    {
        if (bestDiff >= 0)
        {
            if      (bestDiff == 0) { lightColour = juce::Colours::limegreen; lightLabel = "GREAT";    }
            else if (bestDiff <= 2) { lightColour = juce::Colours::yellow;    lightLabel = "CLOSE"; }
            else                    { lightColour = juce::Colours::red;       lightLabel = "MISS";   }
        }
        else
        {
            // No pitch detected while a note is active → silent = miss
            lightColour = juce::Colours::red;
            lightLabel  = "MISS";
        }
    }

    const float r  = 12.0f;
    const float lx = lightCol.getCentreX() - r;
    const float ly = (float) lightCol.getY() + 4.0f;

    g.setColour (lightColour.darker (0.4f));
    g.fillEllipse (lx, ly, r * 2.0f, r * 2.0f);
    g.setColour (lightColour);
    g.fillEllipse (lx + 1.5f, ly + 1.5f, r * 2.0f - 3.0f, r * 2.0f - 3.0f);

    g.setColour (juce::Colours::lightgrey);
    g.setFont   (10.0f);
    g.drawText  (lightLabel, lightCol.withTrimmedTop (30), juce::Justification::centred, false);

    // ── Note display ─────────────────────────────────────────────────────────
    if (note >= 0)
    {
        const int   oct  = (note / 12) - 1;
        const char* name = kNoteNames[note % 12];

        g.setColour (juce::Colours::limegreen);
        g.setFont   (juce::Font (26.0f, juce::Font::bold));
        g.drawText  (juce::String (name) + juce::String (oct),
                     row.withHeight (38), juce::Justification::centred, false);

        g.setColour (juce::Colours::grey);
        g.setFont   (12.0f);
        g.drawText  ("MIDI " + juce::String (note) + "   " + juce::String (hz, 1) + " Hz",
                     row.withTrimmedTop (38), juce::Justification::centred, false);
    }
    else
    {
        g.setColour (juce::Colours::darkgrey);
        g.setFont   (14.0f);
        g.drawText  ("Waiting for audio...", row, juce::Justification::centred, false);
    }
}

// ─── Status bar ─────────────────────────────────────────────────────────────────
// Bottom strip: warns when the highway can't be drawn fast enough to keep up, so the
// user knows to lower the Frame cap or shrink the window. The fps/policy is also shown
// faintly when healthy.

void RBVocalSingAudioProcessorEditor::drawStatusBar (juce::Graphics& g,
                                                      juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xff101018));
    g.fillRect  (area);
    g.setColour (juce::Colour (0xff222233));
    g.drawHorizontalLine (area.getY(), (float) area.getX(), (float) area.getRight());

    const int  fps  = (int) std::lround (displayFps);
    const auto text = area.reduced (8, 0);
    g.setFont (12.0f);
    g.setColour (juce::Colour (0xff556070));  // always faint
    const juce::String calibStr = (latencyOffsetMs == 0)
                                      ? "0 ms"
                                      : (latencyOffsetMs > 0 ? "+" : "") + juce::String (latencyOffsetMs) + " ms";
    juce::String rateStr = juce::String (cachedPlayRate, 2);
    if (! audioProcessor.reaperApi.CSurf_OnPlayRateChange)
        rateStr += " [N/A]";

    juce::String status = juce::String (fps) + " fps   |   REAPER priority: "
                              + (reaperPriority ? "on" : "off")
                              + "   |   renderer: " + (glContext.isAttached() ? "GPU" : "CPU")
                              + "   |   calib: " + calibStr
                              + "   |   rate: " + rateStr;

#if RB_PAINT_PROFILE
    // Per-section paint EMAs (see paint()) + window size, so each grid reading
    // self-documents its geometry ('S' cycles presets). May clip at small widths —
    // diagnostics only.
    status << "   |   hw:"  << juce::String (profHighwayMs, 2)
           << " ly:"        << juce::String (profLyricsMs, 2)
           << " st:"        << juce::String (profStatsMs, 2)
           << " bar:"       << juce::String (profBarsMs, 2)
           << " tot:"       << juce::String (lastPaintSeconds * 1000.0, 2) << " ms"
           << "   |   "     << getWidth() << "x" << getHeight();
#endif

    g.drawText (status, text, juce::Justification::centredLeft, false);
}

// ─── Message bar ──────────────────────────────────────────────────────────────
// Chart status ("Loaded N notes from …") is shown here at all times. When
// frames are dropping the warning overrides the status message.

void RBVocalSingAudioProcessorEditor::drawMessageBar (juce::Graphics& g,
                                                       juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xff101018));
    g.fillRect  (area);
    g.setColour (juce::Colour (0xff222233));
    g.drawHorizontalLine (area.getY(), (float) area.getX(), (float) area.getRight());

    const auto text = area.reduced (8, 0);
    g.setFont (12.0f);

    if (framesDropping)
    {
        const int fps = (int) std::lround (displayFps);
        juce::String msg = "[!] Can't keep up (~" + juce::String (fps) + " fps) - ";
        msg += reaperPriority ? "shrink window or lower Frame cap"
                              : "REAPER may lag; shrink window, lower Frame cap, or enable REAPER priority";
        g.setColour (juce::Colour (0xffffb030));
        g.drawText (msg, text, juce::Justification::centredLeft, true);
    }
    else
    {
        const auto& status = audioProcessor.chartStatus;
        if (status.isNotEmpty())
        {
            g.setColour (juce::Colour (0xff556070));
            g.drawText (status, text, juce::Justification::centredLeft, false);
        }
    }
}
