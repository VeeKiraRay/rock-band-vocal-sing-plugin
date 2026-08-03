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

// Fractional scoring pitch shared by the traffic light and note-hit tracking: the
// detected pitch octave-wrapped into [minP, maxP] with the sub-semitone offset carried
// across the wrap. Passes through unchanged when wrapping is off, the rounded note is
// already in range, or nothing is detected (negative input).
static float scoringPitchFor (float midiF, bool octaveWrap, int minP, int maxP,
                              const std::array<std::vector<int>, 12>& candsByPc)
{
    if (midiF < 0.0f || ! octaveWrap)
        return midiF;

    const int rounded = juce::jlimit (0, 127, juce::roundToInt (midiF));
    if (rounded >= minP && rounded <= maxP)
        return midiF;

    return (float) wrapPitchIntoRange (rounded, minP, maxP, candsByPc)
         + (midiF - (float) rounded);
}

// ─── Highway ──────────────────────────────────────────────────────────────────

void RBVocalSingAudioProcessorEditor::drawHighway (juce::Graphics& g,
                                                    juce::Rectangle<int> area,
                                                    double t)
{
#if RB_PAINT_PROFILE
    // Sub-section lap timer (profile builds only): hwLap(acc) adds the time since the
    // previous lap to acc. Folded into the profHw* EMAs at the end of the function.
    juce::int64  hwSecStart = juce::Time::getHighResolutionTicks();
    const double msPerTick  = 1000.0 / (double) juce::Time::getHighResolutionTicksPerSecond();
    double msBg = 0.0, msBars = 0.0, msText = 0.0, msOv = 0.0;
    auto hwLap = [&] (double& accum)
    {
        const juce::int64 now = juce::Time::getHighResolutionTicks();
        accum += (now - hwSecStart) * msPerTick;
        hwSecStart = now;
    };
#endif

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

    auto pitchY = [&] (float p) -> float {
        return (float) area.getBottom() - (p - fMinP + 0.5f) * noteH;
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
        g.setFont (juce::Font (10.0f, juce::Font::bold));
        g.setColour (juce::Colour (0x99ffffff));
        const int maxP = (int) std::round (fMaxP);
        const int minP = (int) std::round (fMinP);
        const auto makeName = [&] (int p) -> juce::String {
            return juce::String (kNoteNames[p % 12]) + juce::String (p / 12 - 1);
        };
        // Top label: centre on the top note's Y, clamp inside area
        const float topY = pitchY ((float) maxP);
        g.drawText (makeName (maxP),
                    area.getX() + 3, juce::jmax (area.getY(), (int) topY - 5), 28, 10,
                    juce::Justification::centredLeft, false);
        // Bottom label: centre on the bottom note's Y, clamp inside area
        const float botY = pitchY ((float) minP);
        g.drawText (makeName (minP),
                    area.getX() + 3, juce::jmin (area.getBottom() - 10, (int) botY - 5), 28, 10,
                    juce::Justification::centredLeft, false);
    }

    // Playhead
    g.setColour (kPlayheadCol);
    g.drawVerticalLine ((int) phX, (float) area.getY(), (float) area.getBottom());

#if RB_PAINT_PROFILE
    hwLap (msBg);
#endif

    // Note bars, bend connectors, and lyric labels — clipped to highway area.
    // Rendered one part at a time so bend-connector tracking stays per-part and
    // the z-order (Harm3 → Harm2 → Main) keeps the lead on top.
    g.saveState();
    g.reduceClipRegion (area);
    g.setFont (juce::Font (10.0f, juce::Font::bold));

    // Visible time window. Notes are sorted by startTime, so each part can binary-search
    // to the first potentially-visible note and stop once notes scroll past the right
    // edge — O(visible) per frame instead of O(all notes), independent of song length.
    const double visibleLeft = t + (double) (area.getX()    - phX) / pixPerSec;
    const double leftTime    = visibleLeft - kNoteLookback;
    const double rightTime   = t + (double) (area.getRight() - phX) / pixPerSec;

    // Detection placement (see detectionOffsetSec): detOffset moves a raw sample time onto
    // the chart, detLead is how far the detector runs ahead of the listener's ears — zero
    // with a live mic, calibration-wide for playback, whose audio has been analysed but is
    // not yet audible. `heard` is the sample being heard now; the marker shows it and the
    // contour stops there while playing, so the picture matches the ears in both modes.
    const double      detOffset = detectionOffsetSec();
    const double      detLead   = latencyOffsetMs / 1000.0 - detOffset;
    const TrailSample heard     = heardDetection();

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
            const float cy   = pitchY ((float) n.pitch);
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

            // Hit highlight: fill the portions of this note that were sung within
            // the scoring tolerance (updateNoteHits) in gold — one universal "hit"
            // colour, solid on bars; talkie columns get a translucent wash to match
            // their translucency.
            const auto hitIdx = (size_t) (it - notes.begin());
            if (hitIdx < noteHitSpans.size() && ! noteHitSpans[hitIdx].empty())
            {
                g.setColour (n.isTalkie ? kNoteHitColour.withAlpha (0.18f) : kNoteHitColour);
                for (const auto& sp : noteHitSpans[hitIdx])
                {
                    const float hx1 = std::max (drawX1, phX + (float) (sp.first  - t) * pixPerSec);
                    float       hx2 = std::min (drawX2, phX + (float) (sp.second - t) * pixPerSec);
                    // Same rule as the pitch trail: while playing, nothing is shown ahead
                    // of the playhead line. Spans run to the end of the scoring window,
                    // which in playback mode is a calibration ahead of the listener's
                    // ears — so a note would light up gold while still approaching the
                    // line. Inert with a live mic (spans end at the playhead already) and
                    // lifted when stopped, so a whole reviewed pass stays visible.
                    if (lastIsPlaying) hx2 = std::min (hx2, phX);
                    if (hx2 <= hx1) continue;
                    if (n.isTalkie)
                        g.fillRect (hx1, (float) area.getY(), hx2 - hx1, (float) area.getHeight());
                    else
                        g.fillRect (hx1, cy - barH * 0.5f, hx2 - hx1, barH);
                }
            }

            // Inline bar labels: Main part only — HARM2/HARM3 lyrics live in the top strip.
            if (part == VocalPart::Main
                && !bend && n.lyric.isNotEmpty()
                && x1 >= (float) area.getX() && (x2 - x1) > 4.0f)
            {
#if RB_PAINT_PROFILE
                const juce::int64 lblStart = juce::Time::getHighResolutionTicks();
#endif
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
#if RB_PAINT_PROFILE
                msText += (juce::Time::getHighResolutionTicks() - lblStart) * msPerTick;
#endif
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

#if RB_PAINT_PROFILE
    hwLap (msBars);   // includes label text time — subtracted at the EMA fold below
#endif

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

    // Pitch trail: the sung contour of the current pass, drawn behind the
    // arrowhead at raw (unsnapped) pitch. Samples are time-ascending — binary-search
    // to the first visible one and walk. Octave wrapping mirrors the arrowhead (wrap
    // the rounded note into the section range, carry the fractional offset); with
    // wrapping off, out-of-range samples break the contour like gap markers do.
    if (pitchTrailOn && ! pitchTrail.empty())
    {
        // Samples are stored on the raw project timeline (see updateNoteHits) and shifted
        // onto the chart by detOffset here — so the window bounds move the opposite way
        // from the sample times.
        //
        // While playing, the contour stops at the sample being heard: anything past it is
        // audio already analysed but not yet audible, and drawing it would run the contour
        // ahead of the marker. When stopped, draw the lot — the pass is being reviewed and
        // REAPER may have snapped the cursor back to the start of it, which would
        // otherwise hide the whole trail. Inert with a live mic, where the heard sample is
        // the newest one.
        const double trailRight = (lastIsPlaying && detHistoryCount > 0)
                                      ? std::min (rightTime + detOffset, heard.time)
                                      : rightTime + detOffset;

        auto trailIt = std::lower_bound (pitchTrail.begin(), pitchTrail.end(),
                                         visibleLeft + detOffset,
            [] (const TrailSample& s, double v) { return s.time < v; });

        juce::Path trail;
        bool open = false;
        for (; trailIt != pitchTrail.end(); ++trailIt)
        {
            if (trailIt->time > trailRight) break;
            if (trailIt->pitch < 0.0f) { open = false; continue; }

            const int  rounded = juce::jlimit (0, 127, juce::roundToInt (trailIt->pitch));
            const bool inR     = ((float) rounded >= fMinP && (float) rounded <= fMaxP);
            if (! inR && ! octaveWrap) { open = false; continue; }

            const float p = inR ? trailIt->pitch
                                : (float) wrapPitchIntoRange (rounded, (int) fMinP, (int) fMaxP,
                                                              audioProcessor.getPitchClassCandidates())
                                      + (trailIt->pitch - (float) rounded);

            const float x = phX + (float) (trailIt->time - detOffset - t) * pixPerSec;
            const float y = pitchY (p);
            if (open) trail.lineTo (x, y);
            else      { trail.startNewSubPath (x, y); open = true; }
        }

        if (! trail.isEmpty())
        {
            g.saveState();
            g.reduceClipRegion (area);
            g.setColour (kPitchTrailColour);
            g.strokePath (trail, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
            g.restoreState();
        }
    }

    // Arrowhead for live pitch — drawn at the fractional pitch (cents precision) so it
    // glides through slides instead of stepping by semitone. White when in range;
    // amber when octave-wrapped out of range (the wrap moves the rounded note and
    // carries the sub-semitone offset across, like the traffic light).
    const float userPitch = heard.pitch;
    if (userPitch >= 0.0f)
    {
        const int  rounded  = juce::jlimit (0, 127, juce::roundToInt (userPitch));
        const bool inRange  = ((float) rounded >= fMinP && (float) rounded <= fMaxP);
        const bool doWrap   = octaveWrap && !inRange;
        float drawPitch = doWrap
                              ? (float) wrapPitchIntoRange (rounded, (int) fMinP, (int) fMaxP,
                                                            audioProcessor.getPitchClassCandidates())
                                    + (userPitch - (float) rounded)
                              : userPitch;

        if (inRange || doWrap)
        {
            // Visual snap: when the drawn pitch is
            // within the scoring tolerance of an active note, lock the arrow onto that
            // note's lane — hides natural jitter while the traffic light shows GREAT.
            // Same windowed scan as the traffic light (notes are sorted, non-overlapping),
            // at t: the pitch shown is the one belonging there, so the notes must be too.
            {
                auto snapIt = std::lower_bound (notes.begin(), notes.end(), t - kNoteLookback,
                    [] (const NoteInfo& n, double v) { return n.startTime < v; });
                while (snapIt != notes.begin())
                {
                    const auto prev = std::prev (snapIt);
                    if (prev->endTime <= t) break;
                    snapIt = prev;
                }
                float bestDiff = -1.0f, bestPitch = 0.0f;
                for (; snapIt != notes.end(); ++snapIt)
                {
                    const auto& n = *snapIt;
                    if (n.startTime > t) break;
                    if (t >= n.endTime || n.isTalkie) continue;
                    const float diff = std::abs (drawPitch - (float) n.pitch);
                    if (bestDiff < 0.0f || diff < bestDiff) { bestDiff = diff; bestPitch = (float) n.pitch; }
                }
                if (bestDiff >= 0.0f && bestDiff * 100.0f <= (float) pitchTolCents)
                    drawPitch = bestPitch;
            }

            // On the playhead line: the sample being shown is the one whose chart position
            // is there, in both modes. The exception is a negative lead (negative
            // Calibration while in playback mode), where that sample has not been analysed
            // yet — nothing to pin to, so the marker falls back to following the newest.
            const float cx = phX + (float) std::min (0.0, detLead) * pixPerSec;
            const float cy = pitchY (drawPitch);

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

#if RB_PAINT_PROFILE
    hwLap (msOv);
    profHwBgMs   = 0.5 * profHwBgMs   + 0.5 * msBg;
    profHwBarsMs = 0.5 * profHwBarsMs + 0.5 * (msBars - msText);
    profHwTextMs = 0.5 * profHwTextMs + 0.5 * msText;
    profHwOvMs   = 0.5 * profHwOvMs   + 0.5 * msOv;
#endif
}

// ─── The detection being heard ────────────────────────────────────────────────
// Everything on the playhead line describes the audio the listener is hearing, which is
// not always the newest thing analysed: with a live mic the singer's voice has already
// made the round trip the calibration measures, so the newest detection *is* what they
// hear; with playback the newest detection is calibration-wide ahead of their ears. The
// lead is measured against the newest sample rather than the transport clock, so live-mic
// mode returns exactly the newest entry (no jitter from where a hop fell inside a block).

RBVocalSingAudioProcessorEditor::TrailSample
RBVocalSingAudioProcessorEditor::heardDetection() const
{
    if (detHistoryCount == 0) return { 0.0, -1.0f };

    const auto at = [this] (int stepsBack) -> const TrailSample&
    {
        return detHistory[(size_t) ((detHistoryHead - stepsBack + kDetHistorySize) % kDetHistorySize)];
    };

    const TrailSample& newest = at (1);
    const double lead = latencyOffsetMs / 1000.0 - detectionOffsetSec();
    if (lead <= 0.0) return newest;

    const double target = newest.time - lead;
    for (int i = 2; i <= detHistoryCount; ++i)
        if (at (i).time <= target) return at (i);

    // The window doesn't reach that far back — only right after playback starts, or with a
    // large calibration and dropped hops. The oldest entry is the closest answer we have.
    return at (detHistoryCount);
}

// ─── Sung-pass tracking (note hits + pitch trail) ─────────────────────────────
// Called once per frame from onFrame(). Accumulates, per chart note, the spans of
// playback time the singer was within the scoring tolerance (talkie = presence),
// merging contiguous frames into one span — drawHighway overlays the spans on the
// bars, and the coverage ratio is the future phrase-scoring input. Also appends
// the sung fractional pitch to the pass's pitch trail. Both clear together when the
// next pass starts (hitResetPending); chart reloads remap the spans by note identity
// (remapNoteHitSpans) and leave the trail untouched.
//
// Scores at hop resolution (~16 ms, the audio thread's detection cadence) rather
// than once per UI frame: the audio thread queues one HopSample per hop (voiced,
// held, or silent), drained here every call. A UI frame can span several hops —
// point-sampling only the latest atomic made hop-level detections invisible
// whenever they were superseded before the next UI frame happened to read them,
// which could hide notes shorter than one frame interval entirely (see
// _upcoming/SHORT_NOTE_HIT_TRACKING.md, bug #3). lastHopMidiF/lastHopVoiced carry
// the pitch "in effect" at the end of one frame into the start of the next, so the
// [t0, t] window is reconstructed as a sequence of hop-bounded sub-intervals
// instead of one instantaneous sample.

void RBVocalSingAudioProcessorEditor::updateNoteHits()
{
    // Drain the audio thread's hop queue unconditionally, before any early return
    // below, so it never overflows while stopped (hops keep arriving whenever
    // input monitoring feeds the audio thread even when the transport isn't playing).
    hopScratch.clear();
    RBVocalSingAudioProcessor::HopSample hopBuf[RBVocalSingAudioProcessor::kHopQueueCapacity];
    const int hopN = audioProcessor.drainHopSamples (hopBuf, RBVocalSingAudioProcessor::kHopQueueCapacity);
    hopScratch.insert (hopScratch.end(), hopBuf, hopBuf + hopN);

    // Feed the marker history here too — unconditionally, so it keeps rolling while the
    // transport is stopped and while the Pitch Trail setting is off. Raw times, like the
    // trail; heardDetection() applies the lead when it looks a sample up.
    for (int i = 0; i < hopN; ++i)
    {
        detHistory[(size_t) detHistoryHead] = { hopBuf[i].time, hopBuf[i].midiFloat };
        detHistoryHead = (detHistoryHead + 1) % kDetHistorySize;
        if (detHistoryCount < kDetHistorySize) ++detHistoryCount;
    }

    const auto& notes = audioProcessor.getChartNotes();

    // A chart reload is not a time discontinuity and must not wipe the review state
    // — remap the spans onto the reloaded note list by identity instead (only
    // actually-changed notes lose their gold; the trail references no notes at all).
    if (audioProcessor.getChartVersion() != noteHitVersion)
    {
        noteHitVersion = audioProcessor.getChartVersion();
        remapNoteHitSpans();
    }

    // Hop timestamps arrive from the audio thread as raw project time. Everything in this
    // function — t, t0, hopT — is the *detection* timeline (detectionOffsetSec), which
    // equals paint's drawing timeline only with a live mic: a singer's voice arrives late
    // by the round trip the calibration measures and must be shifted back onto the chart,
    // while playback audio is already in position. The trail is stored raw and shifted at
    // draw instead, which keeps the take export on true project time and lets a
    // calibration change re-align an already-captured trail. (Found 2026-08-03: before
    // the split the trail was drawn latencyOffsetMs right of the playhead, and every
    // sub-interval below fell outside [t0, t] whenever calibration was non-zero.)
    const double calibSec = detectionOffsetSec();

    const double t  = cachedPlayTime - calibSec;
    const double t0 = lastHitTrackTime;
    lastHitTrackTime = t;

    // Only a small forward step counts as sung time. A discontinuity — the first
    // frame, or a jump in either direction (user seek, REAPER's snap-back to the edit
    // cursor on stop, loop wrap) — arms a deferred reset instead of clearing at once,
    // so the gold stays reviewable while stopped and clears when the next pass starts.
    // t == t0 is just a stalled frame (stopped transport, or the playback-time atomic
    // repeating between audio blocks): neither sung time nor a discontinuity.
    if (t0 < 0.0 || t < t0 || t - t0 > 0.5)
    {
        hitResetPending = true;
        return;
    }
    if (t == t0)
        return;

    // While stopped, the tracked time follows the edit cursor — a small forward
    // nudge (clicking/dragging in REAPER's arrange view) must neither count as sung
    // time nor fire a pending reset. Only the REAPER path has a play-state signal;
    // the file fallback keeps the pure time heuristic (its host playhead only
    // advances during playback).
    if (audioProcessor.reaperApi.isLoaded() && ! lastIsPlaying)
        return;

    if (hitResetPending)
    {
        hitResetPending = false;
        for (auto& spans : noteHitSpans)
            spans.clear();
        pitchTrail.clear();
        lastHopMidiF  = -1.0f;
        lastHopVoiced = false;
    }

    // Degenerate-session guard (~25 min of continuous singing at 60 fps) — drop the
    // older half rather than growing without bound.
    if (pitchTrail.size() > 100000)
        pitchTrail.erase (pitchTrail.begin(),
                          pitchTrail.begin() + (std::ptrdiff_t) (pitchTrail.size() / 2));

    // Look-back once for the whole frame — note times don't change mid-frame, so
    // every hop-bounded sub-interval below reuses this same starting point.
    auto itStart = std::lower_bound (notes.begin(), notes.end(), t0 - kNoteLookback,
        [] (const NoteInfo& n, double v) { return n.startTime < v; });
    while (itStart != notes.begin())
    {
        const auto prev = std::prev (itStart);
        if (prev->endTime <= t0) break;
        itStart = prev;
    }

    // Scores one [segStart, segEnd) sub-interval — same windowed scan as the
    // traffic light, but per hop-bounded slice instead of the whole [t0, t] frame.
    auto scoreSegment = [&] (double segStart, double segEnd, bool segVoiced, float segMidiF)
    {
        if (segEnd <= segStart) return;

        const float scoringPitch = scoringPitchFor (segMidiF, octaveWrap,
                                                    audioProcessor.chartMinPitch,
                                                    audioProcessor.chartMaxPitch,
                                                    audioProcessor.getPitchClassCandidates());

        for (auto it = itStart; it != notes.end(); ++it)
        {
            const auto& n = *it;
            if (n.startTime > segEnd) break;
            if (n.endTime <= segStart) continue;

            const bool hit = n.isTalkie
                                 ? segVoiced
                                 : (scoringPitch >= 0.0f
                                    && std::abs (scoringPitch - (float) n.pitch) * 100.0f
                                           <= (float) pitchTolCents);
            if (! hit) continue;

            const double s = std::max (segStart, n.startTime);
            const double e = std::min (segEnd,   n.endTime);
            if (e <= s) continue;

            auto& spans = noteHitSpans[(size_t) (it - notes.begin())];
            if (! spans.empty() && s <= spans.back().second + 1e-3)
                spans.back().second = std::max (spans.back().second, e);
            else
                spans.emplace_back (s, e);
        }
    };

    double segStart  = t0;
    float  segMidiF  = lastHopMidiF;
    bool   segVoiced = lastHopVoiced;

    for (const auto& hop : hopScratch)
    {
        const double hopT = hop.time - calibSec;

        if (hopT <= t0)
        {
            // Stale relative to this frame's window (already accounted for by a
            // previous frame) — just fold it into the carried starting state.
            segMidiF  = hop.midiFloat;
            segVoiced = hop.voiced;
            continue;
        }

        // Trail capture (skipped when the Pitch Trail setting is off): one sample
        // per hop instead of per frame; an unvoiced hop after voiced ones adds a
        // single gap marker so the drawn contour breaks across silences. Stored at
        // the raw hop time — drawHighway applies calibSec, and the take export wants
        // true project time so its MIDI lines up with the audio waveform.
        if (pitchTrailOn)
        {
            if (hop.midiFloat >= 0.0f)
                pitchTrail.push_back ({ hop.time, hop.midiFloat });
            else if (! pitchTrail.empty() && pitchTrail.back().pitch >= 0.0f)
                pitchTrail.push_back ({ hop.time, -1.0f });
        }

        scoreSegment (segStart, std::min (hopT, t), segVoiced, segMidiF);

        segStart  = hopT;
        segMidiF  = hop.midiFloat;
        segVoiced = hop.voiced;
    }

    // Tail: the most recent hop's value (or the carried-over value, if no hops
    // arrived this frame) still holds through to this frame's t.
    scoreSegment (segStart, t, segVoiced, segMidiF);

    lastHopMidiF  = segMidiF;
    lastHopVoiced = segVoiced;
}

// Re-aligns noteHitSpans to a freshly reloaded chart. Both the old identity snapshot
// (noteHitKeys) and the reloaded chartNotes are sorted by startTime, so a two-pointer
// walk matches each new note to its old self on (startTime, endTime, part); the gold
// carries over only when the pitch is also unchanged (the spans measured tolerance
// against the old pitch). Unmatched notes — timing edits, insertions, deletions —
// start with no spans, and everything else survives the reload untouched.

void RBVocalSingAudioProcessorEditor::remapNoteHitSpans()
{
    // Matching needs a tolerance, not exact equality: whenever REAPER rewrites the
    // take (a MIDI editor flushing its buffer on stop, MIDI_SetNote +
    // MarkTrackItemsDirty re-serialising after a pitch edit) the PPQ→time recompute
    // can drift by last-bit ULPs, and exact comparison then silently dropped every
    // span. 0.1 ms is far above any serialisation drift, far below any real edit.
    static constexpr double kNoteMatchEps = 1e-4;

    const auto& notes = audioProcessor.getChartNotes();

    // Transient empty read (e.g. a reload racing the take flush): park the spans and
    // keep the identity snapshot, so a recovered chart gets its gold back on the
    // next non-empty reload. Nothing is drawn while the chart is empty anyway.
    if (notes.empty())
    {
        if (! noteHitKeys.empty() && parkedHitSpans.empty())
            parkedHitSpans = std::move (noteHitSpans);
        noteHitSpans.clear();
        return;
    }

    auto& oldSpans = parkedHitSpans.empty() ? noteHitSpans : parkedHitSpans;

    std::vector<std::vector<std::pair<double, double>>> newSpans (notes.size());

    size_t oldIdx = 0;
    for (size_t newIdx = 0; newIdx < notes.size(); ++newIdx)
    {
        const auto& n = notes[newIdx];
        while (oldIdx < noteHitKeys.size()
               && noteHitKeys[oldIdx].start < n.startTime - kNoteMatchEps)
            ++oldIdx;
        // Scan the run of near-equal start times (Harmonies parts can share one) for
        // the matching part + end time.
        for (size_t k = oldIdx; k < noteHitKeys.size()
                                && noteHitKeys[k].start <= n.startTime + kNoteMatchEps; ++k)
        {
            const auto& key = noteHitKeys[k];
            if (std::abs (key.end - n.endTime) < kNoteMatchEps && key.part == n.part)
            {
                if (key.pitch == n.pitch && k < oldSpans.size())
                    newSpans[newIdx] = std::move (oldSpans[k]);
                break;
            }
        }
    }

    noteHitSpans = std::move (newSpans);
    parkedHitSpans.clear();

    noteHitKeys.resize (notes.size());
    for (size_t i = 0; i < notes.size(); ++i)
        noteHitKeys[i] = { notes[i].startTime, notes[i].endTime,
                           notes[i].part, notes[i].pitch };
}

// ─── Stats panel ──────────────────────────────────────────────────────────────

// `t` is paint's drawn time: the pitch, note name, Hz and traffic light all describe the
// audio being heard at the playhead line (heardDetection), so the notes they are scored
// against are the ones drawn there. The time readout alone is the raw transport position.
void RBVocalSingAudioProcessorEditor::drawStats (juce::Graphics& g,
                                                  juce::Rectangle<int> area,
                                                  double t)
{
    g.setColour (kStatsBg);
    g.fillRect  (area);

    g.setColour (juce::Colour (0xff222233));
    g.drawHorizontalLine (area.getY(), (float) area.getX(), (float) area.getRight());

    // One detection drives the whole band, so the light can never disagree with the note
    // name beside it. Hz is recomputed from the fractional pitch instead of carrying the
    // frequency across the hop queue: it is the exact inverse of the processor's own
    // conversion, so the displayed value is unchanged at the precision shown.
    const float midiF = heardDetection().pitch;
    const int   note  = midiF >= 0.0f ? juce::jlimit (0, 127, juce::roundToInt (midiF)) : -1;
    const float hz    = midiF >= 0.0f ? 440.0f * std::pow (2.0f, (midiF - 69.0f) / 12.0f) : 0.0f;

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
    // The readout is the real transport position (raw), so it matches REAPER's own time
    // display, and is unaffected by the calibration offset.
    juce::String timeStr = fmtTime (cachedPlayTime);
    const double endTime = audioProcessor.songEndTime;
    if (endTime > 0.0)
        timeStr += " / " + fmtTime (endTime);

    g.setColour (juce::Colours::white);
    g.setFont   (juce::Font (11.0f, juce::Font::bold));
    g.drawText  (timeStr, timeCol, juce::Justification::centredLeft, false);

    // ── Traffic light ────────────────────────────────────────────────────────
    auto lightCol = row.removeFromRight (70);

    // Score against the closest active note of ANY part: green if the singer
    // matches any harmony line currently sounding. Scoring uses the fractional
    // pitch (cents precision); `note` above keeps
    // driving the note display below. When octave wrapping is on, an out-of-range
    // pitch is mapped to its in-range pitch-class equivalent first, carrying the
    // sub-semitone offset across the wrap (scoringPitchFor).
    const float scoringPitch = scoringPitchFor (midiF, octaveWrap,
                                                audioProcessor.chartMinPitch,
                                                audioProcessor.chartMaxPitch,
                                                audioProcessor.getPitchClassCandidates());

    // Scan all active notes unconditionally (even when no audio is detected) so that
    // silence during any active note correctly scores as OFF (red), not --- (grey).
    bool  activeTalkie  = false;
    bool  activeRegular = false;
    float bestDiffCents = -1.0f;

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
                if (scoringPitch >= 0.0f)
                {
                    const float diff = std::abs (scoringPitch - (float) n.pitch) * 100.0f;
                    if (bestDiffCents < 0.0f || diff < bestDiffCents) bestDiffCents = diff;
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
        if (bestDiffCents >= 0.0f)
        {
            // GREAT within ± pitchTolCents of the note, CLOSE within twice that.
            const float tol = (float) pitchTolCents;
            if      (bestDiffCents <= tol)        { lightColour = juce::Colours::limegreen; lightLabel = "GREAT"; }
            else if (bestDiffCents <= 2.0f * tol) { lightColour = juce::Colours::yellow;    lightLabel = "CLOSE"; }
            else                                  { lightColour = juce::Colours::red;       lightLabel = "MISS";  }
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
    g.setFont   (juce::Font (10.0f, juce::Font::bold));
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
        g.setFont   (juce::Font (12.0f, juce::Font::bold));
        g.drawText  ("MIDI " + juce::String (note) + "   " + juce::String (hz, 1) + " Hz",
                     row.withTrimmedTop (38), juce::Justification::centred, false);
    }
    else
    {
        g.setColour (juce::Colours::darkgrey);
        g.setFont   (juce::Font (14.0f, juce::Font::bold));
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
    g.setFont (juce::Font (12.0f, juce::Font::bold));
    g.setColour (juce::Colour (0xff556070));  // always faint
    // The live-mic/playback mode only matters when there is an offset to apply, so it is
    // appended when there is one and left off at 0 ms rather than shown as dead text.
    const juce::String calibStr = (latencyOffsetMs == 0)
                                      ? "0 ms"
                                      : (latencyOffsetMs > 0 ? "+" : "") + juce::String (latencyOffsetMs)
                                            + (liveMicInput ? " ms live" : " ms playback");
    juce::String rateStr = juce::String (cachedPlayRate, 2);
    if (! audioProcessor.reaperApi.CSurf_OnPlayRateChange)
        rateStr += " [N/A]";

    juce::String status = juce::String (fps) + " fps   |   REAPER priority: "
                              + (reaperPriority ? "on" : "off")
                              + "   |   renderer: " + (glContext.isAttached() ? "GPU" : "CPU")
                              + "   |   calib: " + calibStr
                              + "   |   rate: " + rateStr;

#if RB_PITCH_PROFILE
    // Audio-thread cost (see the profDetect* comment in PluginProcessor.h): cost of one
    // detectPitch call, processBlock's share of its deadline, and the achieved hop rate.
    // det × hops/1000 is the detector's own share of a core; load is the whole audio
    // thread including the host-rate front-end filters.
    status << "   |   det:"  << juce::String (audioProcessor.profDetectMs.load(), 3) << " ms"
           << "  load:"      << juce::String (audioProcessor.profBlockLoad.load() * 100.0, 2) << "%"
           << "  hops:"      << juce::String (displayHopsPerSec, 1) << "/s";
#endif

#if RB_PAINT_PROFILE
    // Per-section paint EMAs (see paint()), the drawHighway sub-split (bg/bar/txt/ov,
    // see drawHighway), and the window size, so each grid reading self-documents its
    // geometry ('S' cycles presets). May clip at small widths — diagnostics only.
    status << "   |   hw:"  << juce::String (profHighwayMs, 2)
           << " [bg:"       << juce::String (profHwBgMs, 2)
           << " bar:"       << juce::String (profHwBarsMs, 2)
           << " txt:"       << juce::String (profHwTextMs, 2)
           << " ov:"        << juce::String (profHwOvMs, 2) << "]"
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
// frames are dropping the warning overrides the status message, and a transient
// flash (take-export feedback) overrides both while active.

void RBVocalSingAudioProcessorEditor::drawMessageBar (juce::Graphics& g,
                                                       juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xff101018));
    g.fillRect  (area);
    g.setColour (juce::Colour (0xff222233));
    g.drawHorizontalLine (area.getY(), (float) area.getX(), (float) area.getRight());

    const auto text = area.reduced (8, 0);
    g.setFont (juce::Font (12.0f, juce::Font::bold));

    if (juce::Time::getMillisecondCounterHiRes() * 0.001 < flashUntil)
    {
        // Transient action feedback (take export). Outranks even the frame-skip
        // warning for its ~4 s — the result of an explicit click must not be hidden,
        // and chartStatus alone would be overwritten by the post-export chart reload.
        g.setColour (juce::Colour (0xffffb030));
        g.drawText (flashMessage, text, juce::Justification::centredLeft, true);
    }
    else if (framesDropping)
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
