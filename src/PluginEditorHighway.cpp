#include "PluginEditor.h"
#include "HighwayRange.h"
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

// Whether to mark a sample as octave-doubtful. The decision itself is made on the audio
// thread (see kOctaveRunHops) because it depends on how long the rival persisted, which a
// consumer holding one sample cannot reconstruct — and because the correction has to obey
// the same rule. Paint only applies the mode gate on top, so what Warn colours red stays
// exactly the set Warn + correct would have moved.
static bool isOctaveDoubtful (const rbtake::TrailPoint& s, bool guardOn) noexcept
{
    return guardOn && s.octaveDoubt;
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

    // Chart-less capture. With no notes loaded the highway still draws its axis, the
    // playhead and the sung contour, so a reference pass against a stem can be watched as
    // it is captured. Nothing outside this function ever depended on a chart — the pitch
    // trail and Save take have no chart-state gate at all — so the early return that used
    // to sit here was the only thing making capture look blocked. Every note-driven loop
    // below is already inert on an empty list (lower_bound returns end(); the section and
    // separator vectors are empty), so only the pitch axis needs a different source.
    const bool chartLess = notes.empty();

    // Octave wrapping is a scoring concept: it maps a sung pitch onto the octave the chart
    // expects. With no chart there is nothing to expect, so out-of-band samples should break
    // the contour exactly as they do with the setting off. (wrapPitchIntoRange would not
    // misbehave — an empty candidate pool falls through to its modular wrap — but wrapping
    // onto a range nothing chose is a fiction.) Reachable rather than theoretical: the
    // detector band is 60-1200 Hz, MIDI ~34.6-86.1, wider than the drawn 36-84 at both ends.
    const bool wrapOn = octaveWrap && ! chartLess;

    // Compute the visible pitch range for the current section, with a smooth
    // transition on section change driven by playback time (no extra state needed).
    // Smoothstep interpolation over kTransitionDur seconds after each boundary.
    // The pad/clamp/min-span rules live in rbhighway::visibleRange (HighwayRange.h) so
    // they are unit-testable and stated once for both sides of a transition.
    //
    // Chart-less falls back to the whole singable band: nothing can drop off-screen, and the
    // axis stays put while a take is captured. The section loop below is inert there (no
    // sections), so the seed values survive it.
    float fMinP = chartLess ? (float) rbhighway::kVocalLow  : (float) audioProcessor.chartMinPitch;
    float fMaxP = chartLess ? (float) rbhighway::kVocalHigh : (float) audioProcessor.chartMaxPitch;

    const auto& sections = audioProcessor.getChartPitchSections();
    for (int si = 0; si < (int) sections.size(); ++si)
    {
        const auto& sec = sections[si];
        if (t < sec.startTime || t >= sec.endTime) continue;

        const auto tgt = rbhighway::visibleRange (sec.minPitch, sec.maxPitch);

        if (si > 0 && t < sec.startTime + sec.transitionDur)
        {
            const auto& prev = sections[si - 1];
            const auto  src  = rbhighway::visibleRange (prev.minPitch, prev.maxPitch);

            const float a = (float) (t - sec.startTime) / sec.transitionDur;
            const float s = a * a * (3.0f - 2.0f * a);  // smoothstep ease in/out

            fMinP = src.lo + s * (tgt.lo - src.lo);
            fMaxP = src.hi + s * (tgt.hi - src.hi);
        }
        else
        {
            fMinP = tgt.lo;
            fMaxP = tgt.hi;
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
        // Top label: centre on the top note's Y, clamp inside area
        const float topY = pitchY ((float) maxP);
        g.drawText (noteNameFor (maxP),
                    area.getX() + 3, juce::jmax (area.getY(), (int) topY - 5), 28, 10,
                    juce::Justification::centredLeft, false);
        // Bottom label: centre on the bottom note's Y, clamp inside area
        const float botY = pitchY ((float) minP);
        g.drawText (noteNameFor (minP),
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
    const juce::Font labelFont (10.0f, juce::Font::bold);
    g.setFont (labelFont);

    // Note markings — the labels above the bars. Collected here and placed after every part
    // has been drawn, because the yield rule runs Main-first while the bars run Main-last
    // (z-order), and because a label must be able to see bars belonging to parts that have
    // not been drawn yet.
    const bool wantLyricMarks = (noteMarkings == NoteMarkings::Lyric
                                 || noteMarkings == NoteMarkings::Both);
    const bool wantPitchMarks = (noteMarkings == NoteMarkings::Pitch
                                 || noteMarkings == NoteMarkings::Both);

    // Text widths. Needed for the yield rule, not only for drawing: a label's collision
    // extent is the real width of its text, whereas drawText is handed the
    // jmax(40, barW) box the inline lyric has always used.
    if (wantLyricMarks || wantPitchMarks)
    {
        if (hwLabelWidthCacheVersion != audioProcessor.getChartVersion()
            || hwLabelWidthCacheFontH != labelFont.getHeight()
            || hwLabelWidthCache.size() != notes.size())
        {
            hwLabelWidthCache.assign (notes.size(), -1.0f);
            hwLabelWidthCacheVersion = audioProcessor.getChartVersion();
            hwLabelWidthCacheFontH   = labelFont.getHeight();
        }

        if (hwPitchMarkWidthsFontH != labelFont.getHeight())
        {
            for (int p = 0; p < 128; ++p)
                hwPitchMarkWidths[(size_t) p] = labelFont.getStringWidthFloat (pitchMarkFor (p));
            hwPitchMarkWidthsFontH = labelFont.getHeight();
        }
    }

    markScratch.clear();

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
    const double      detLead   = drawingOffsetSec() - detOffset;
    const TrailSample heard     = heardDetection();

    // Octave guard: Off draws exactly what it always did, so every doubt test below
    // collapses to false and no extra path or colour is built.
    const bool guardOn = audioProcessor.octaveGuard.load (std::memory_order_relaxed) != 0;

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
        // without being drawn, so the diagonal still renders. (2026-07 code review, 1.7)
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

            // Note markings: collect the label this note wants, if any. Placement and
            // drawing happen once every part is done — see the pass below the z-order calls.
            //
            // The lyric half keeps its original conditions exactly (Main only; HARM2/HARM3
            // lyrics live in the top strip). The pitch half applies to every part, since the
            // reason harmony lyrics are excluded does not apply to a pitch, and is the point
            // of Harmonies mode: reading the interval off the screen.
            const bool hasLyric = wantLyricMarks
                               && part == VocalPart::Main
                               && !bend && n.lyric.isNotEmpty()
                               && n.displayLyric.isNotEmpty()
                               && x1 >= (float) area.getX() && (x2 - x1) > 4.0f;

            // A talkie has no meaningful pitch — it is spoken, and its bar is a full-height
            // column rather than a lane. It keeps its lyric and gets no pitch mark.
            const bool hasPitch = wantPitchMarks && ! n.isTalkie
                               && x1 >= (float) area.getX() && (x2 - x1) > 4.0f;

            if (hasLyric || hasPitch)
            {
                const auto noteIdx = (size_t) (it - notes.begin());

                float lyricW = 0.0f;
                if (hasLyric)
                {
                    float& cached = hwLabelWidthCache[noteIdx];
                    if (cached < 0.0f) cached = labelFont.getStringWidthFloat (n.displayLyric);
                    lyricW = cached;
                }

                const float pitchW = hasPitch ? hwPitchMarkWidths[(size_t) juce::jlimit (0, 127, n.pitch)]
                                              : 0.0f;
                const float totalW = lyricW + (hasLyric && hasPitch ? kMarkGap : 0.0f) + pitchW;

                const float labelY = n.isTalkie ? (float) area.getY() + 2.0f
                                                : (float) ((int) (cy - barH * 0.5f) - 13.0f);

                // A talkie's obstacle rect is deliberately empty. Its column is a 25 %
                // translucent wash that text reads through perfectly well, and handing over
                // the column itself would suppress every harmony label sharing that stretch
                // of time — which is the talkie's whole width.
                const rbmark::Rect barRect = n.isTalkie
                    ? rbmark::Rect { 0.0f, 0.0f, 0.0f, 0.0f }
                    : rbmark::Rect { drawX1, cy - barH * 0.5f, drawX2, cy + barH * 0.5f };

                markScratch.push_back ({
                    { x1, labelY, x1 + totalW, labelY + kMarkH },
                    barRect,
                    (int) part,
                    (int) noteIdx
                });
            }
        }
    };

    if (harmoniesMode())
    {
        drawNotesForPart (VocalPart::Harm3);
        drawNotesForPart (VocalPart::Harm2);
    }
    drawNotesForPart (VocalPart::Main);

    // ── Note markings: place, then draw ───────────────────────────────────────
    // Bars are drawn Harm3 → Harm2 → Main so the lead lands on top; labels are placed in
    // the opposite order, because the lead's label is the one that must never yield.
    //
    // Deferring every label to here is the one way "Lyric only" differs from the build
    // before note markings existed, where labels drew interleaved with their own part's
    // bars. On a short note the label box overruns the bar (jmax(40, barW)), so the *next*
    // Main note used to paint over the tail of it; now it does not. Labels reading over bars
    // rather than under them is the better picture, and the harmony placement needs the
    // deferral regardless — a label has to be able to see bars not yet drawn.
    if (! markScratch.empty())
    {
#if RB_PAINT_PROFILE
        const juce::int64 lblStart = juce::Time::getHighResolutionTicks();
#endif
        // Stable, and on priority alone: within a part the notes were collected in time
        // order already, and placeMarkings needs that order preserved so the winner of a
        // contested pair does not change as the highway scrolls.
        std::stable_sort (markScratch.begin(), markScratch.end(),
                          [] (const rbmark::Candidate& a, const rbmark::Candidate& b)
                          { return a.priority < b.priority; });

        rbmark::placeMarkings (markScratch, markAccepted);

        for (size_t i = 0; i < markScratch.size(); ++i)
        {
            if (! markAccepted[i]) continue;

            const auto& c = markScratch[i];
            const auto& n = notes[(size_t) c.noteIndex];

            // Recomputed rather than carried on the Candidate: these are drawing details,
            // and keeping them out of the struct is what keeps NoteMarkings.h JUCE-free.
            const bool bend     = (n.displayLyric == "+");
            const bool hasLyric = wantLyricMarks && n.part == VocalPart::Main && !bend
                               && n.lyric.isNotEmpty() && n.displayLyric.isNotEmpty();
            const bool hasPitch = wantPitchMarks && ! n.isTalkie;

            const float x1 = c.label.x1;
            const float x2 = phX + (float) (n.endTime - t) * pixPerSec;
            float       tx = x1;

            if (hasLyric)
            {
                // Unchanged from before note markings existed: same colour, same box, same
                // justification. "Lyric only" has to be indistinguishable from the old build.
                g.setColour (juce::Colour (0xffcce8ff));
                g.drawText  (n.displayLyric,
                             (int) tx,
                             (int) c.label.y1,
                             juce::jmax (40, (int) (x2 - x1)),
                             (int) kMarkH,
                             juce::Justification::centredLeft, false);

                tx += hwLabelWidthCache[(size_t) c.noteIndex] + kMarkGap;
            }

            if (hasPitch)
            {
                g.setColour (kPitchMarkColour);
                g.drawText  (pitchMarkFor (n.pitch),
                             (int) tx,
                             (int) c.label.y1,
                             (int) std::ceil (hwPitchMarkWidths[(size_t) juce::jlimit (0, 127, n.pitch)]) + 4,
                             (int) kMarkH,
                             juce::Justification::centredLeft, false);
            }
        }
#if RB_PAINT_PROFILE
        msText += (juce::Time::getHighResolutionTicks() - lblStart) * msPerTick;
#endif
    }

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

    // Percussion notes: a ringed circle centred vertically (a tap has no pitch) with a
    // timing line through it, as in game. A hit fills the core gold, growing from the
    // centre (rbperc::fillProgress). Drawn over the note bars on purpose: the two only ever
    // share time on a chart the overlap colour is warning about, and then the warning is
    // the thing worth seeing.
    {
        const auto& perc = audioProcessor.getChartPercussion();
        const float r    = juce::jlimit (5.0f, 12.0f, (float) area.getHeight() * 0.0825f);
        const float cy   = (float) area.getCentreY();
        const double pad = (double) r / pixPerSec;

        auto pit = std::lower_bound (perc.begin(), perc.end(), visibleLeft - pad,
            [] (const PercussionNote& p, double v) { return p.startTime < v; });

        for (; pit != perc.end() && pit->startTime <= rightTime + pad; ++pit)
        {
            const float x = phX + (float) (pit->startTime - t) * pixPerSec;
            const auto  i = (size_t) (pit - perc.begin());
            const float fill = i < percHits.size()
                                   ? rbperc::fillProgress (percHits[i], t, lastIsPlaying)
                                   : 0.0f;

            const juce::Colour edge = pit->overlapsSung ? kPercOverlapColour : kPercRingColour;

            g.setColour (pit->overlapsSung ? kPercOverlapColour.withAlpha (0.6f) : kPercLineColour);
            g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());

            g.setColour (kPercCoreColour);
            g.fillEllipse (x - r, cy - r, 2.0f * r, 2.0f * r);

            if (fill > 0.0f)
            {
                const float fr = r * fill;
                g.setColour (kNoteHitColour);
                g.fillEllipse (x - fr, cy - fr, 2.0f * fr, 2.0f * fr);
            }

            g.setColour (edge);
            g.drawEllipse (x - r, cy - r, 2.0f * r, 2.0f * r, 2.0f);
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

        // Two paths from one walk: the ordinary contour and the stretches the octave guard
        // doubts. Where doubt changes between adjacent samples the boundary point is
        // written into both, so the contour stays continuous across the colour change
        // instead of showing a one-sample gap.
        juce::Path trail, trailDoubt;
        bool open = false, curDoubt = false;

        for (; trailIt != pitchTrail.end(); ++trailIt)
        {
            if (trailIt->time > trailRight) break;
            if (trailIt->pitch < 0.0f) { open = false; continue; }

            const int  rounded = juce::jlimit (0, 127, juce::roundToInt (trailIt->pitch));
            const bool inR     = ((float) rounded >= fMinP && (float) rounded <= fMaxP);
            if (! inR && ! wrapOn) { open = false; continue; }

            const float p = inR ? trailIt->pitch
                                : (float) wrapPitchIntoRange (rounded, (int) fMinP, (int) fMaxP,
                                                              audioProcessor.getPitchClassCandidates())
                                      + (trailIt->pitch - (float) rounded);

            const float x = phX + (float) (trailIt->time - detOffset - t) * pixPerSec;
            const float y = pitchY (p);
            const bool  doubt = isOctaveDoubtful (*trailIt, guardOn);

            if (! open)
            {
                (doubt ? trailDoubt : trail).startNewSubPath (x, y);
                open     = true;
                curDoubt = doubt;
            }
            else if (doubt == curDoubt)
            {
                (doubt ? trailDoubt : trail).lineTo (x, y);
            }
            else
            {
                // Close the run at this point, then open the other colour's run at the
                // same point — the shared vertex is what keeps the line unbroken.
                (curDoubt ? trailDoubt : trail).lineTo (x, y);
                (doubt    ? trailDoubt : trail).startNewSubPath (x, y);
                curDoubt = doubt;
            }
        }

        if (! trail.isEmpty() || ! trailDoubt.isEmpty())
        {
            const juce::PathStrokeType stroke (1.5f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded);
            g.saveState();
            g.reduceClipRegion (area);
            // Doubt last, so red reads on top wherever the two runs share a vertex.
            g.setColour (kPitchTrailColour);
            g.strokePath (trail, stroke);
            if (! trailDoubt.isEmpty())
            {
                g.setColour (kOctaveDoubtColour);
                g.strokePath (trailDoubt, stroke);
            }
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
        const bool doWrap   = wrapOn && !inRange;
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

            // Doubt outranks the octave-wrapped amber: if the octave itself is in
            // question then which octave the wrap chose is not the useful thing to say.
            const juce::Colour fillColour =
                  isOctaveDoubtful (heard, guardOn)             ? kOctaveDoubtColour
                : doWrap                                        ? juce::Colour (0xffffaa44)
                                                                : juce::Colours::white;
            g.setColour (kNoteColour.withAlpha (0.35f));
            g.strokePath (arrow, juce::PathStrokeType (6.0f));
            g.setColour  (fillColour);
            g.fillPath   (arrow);
            g.setColour  (kNoteColour);
            g.strokePath (arrow, juce::PathStrokeType (1.5f));
        }
    }

    // Chart-less hint, drawn last so the contour cannot cover it. Demoted from the old
    // full-area banner to a corner label: the highway is showing real content now, and the
    // full reason already lives in the status bar this points at.
    if (chartLess)
    {
        juce::String msg;
        if (audioProcessor.reaperApi.isLoaded())
            msg = audioProcessor.chartStatus.isNotEmpty()
                    ? "No chart - capture only"
                    : "Reaper connected - waiting for project";
        else
            msg = "No MIDI loaded - see Settings";

        g.setColour (juce::Colour (0xaaffcc44));  // amber/attention, dimmed
        g.setFont   (juce::Font (11.0f, juce::Font::bold));
        g.drawText  (msg, area.reduced (6, 4), juce::Justification::topRight, false);
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

    // How far the newest detection sits ahead of the playhead line, both measured on the
    // drawing axis: the newest sample lands at (now - detection offset), the line at
    // (now - drawing offset). Zero or negative means the newest sample *is* what's being
    // heard, which is the live-mic case and every stopped-transport case (nothing is
    // coming out of the speakers to run ahead of).
    const double lead = drawingOffsetSec() - detectionOffsetSec();
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
// Scores at hop resolution (~11 ms, the audio thread's detection cadence) rather
// than once per UI frame: the audio thread queues one HopSample per hop (voiced,
// held, or silent), drained here every call. Point-sampling only the latest atomic
// made hop-level detections invisible whenever they were superseded before the next
// UI frame read them, which could hide notes shorter than one frame entirely (the
// short-note hit-tracking investigation, bug #3).
//
// Every drained hop is used, on its own timestamp: each hop's pitch holds from its
// time until the next hop's (lastHop* carries the open interval across frames). The
// frame clock decides only *whether* to score (playing, no discontinuity), never
// *which* hops count. It used to clip scoring to the frame's [t0, t] and skip any hop
// dated at or before t0 as "already accounted for" - but no hop is ever seen twice,
// and hops are routinely dated before t0: each is filed at its window centre, ~16 ms
// before the block it was computed in, while t comes from that block's start. Measured
// in REAPER (2026-11-10, 128-sample blocks): 68% of hops skipped at 48 fps and 98%
// uncapped, so the trail and Save take saw a third of the detections at best and the
// hit spans fell back to roughly one sample per frame. The cost of the change is that
// the gold now ends at the newest hop rather than at t, ~16-30 ms behind the playhead.

void RBVocalSingAudioProcessorEditor::updateNoteHits()
{
    // Drain the audio thread's hop queue unconditionally, before any early return
    // below, so it never overflows while stopped (hops keep arriving whenever
    // input monitoring feeds the audio thread even when the transport isn't playing).
    hopScratch.clear();
    RBVocalSingAudioProcessor::HopSample hopBuf[RBVocalSingAudioProcessor::kHopQueueCapacity];
    const int hopN = audioProcessor.drainHopSamples (hopBuf, RBVocalSingAudioProcessor::kHopQueueCapacity);
    hopScratch.insert (hopScratch.end(), hopBuf, hopBuf + hopN);

    // Feed the marker history here too — so it keeps rolling while the transport is
    // stopped and while the Pitch Trail setting is off. Raw times, like the trail;
    // heardDetection() applies the lead when it looks a sample up.
    //
    // The one exception is the stem readout: while it is active it writes detHistory from
    // the cursor position instead, and the two sources would otherwise fight at ~94 Hz
    // (input monitoring keeps delivering mic hops even with the transport stopped). The
    // hop queue is still drained above, so nothing overflows — only the history write is
    // skipped, and it resumes the moment playback starts or the mode goes off.
    if (! stemReadoutActive())
    {
        for (int i = 0; i < hopN; ++i)
        {
            detHistory[(size_t) detHistoryHead] = { hopBuf[i].time, hopBuf[i].midiFloat,
                                                    hopBuf[i].octaveRatio, hopBuf[i].octaveDoubt };
            detHistoryHead = (detHistoryHead + 1) % kDetHistorySize;
            if (detHistoryCount < kDetHistorySize) ++detHistoryCount;
        }
    }

    const auto& notes = audioProcessor.getChartNotes();
    const auto& perc  = audioProcessor.getChartPercussion();

    // A chart reload is not a time discontinuity and must not wipe the review state
    // — remap the spans onto the reloaded note list by identity instead (only
    // actually-changed notes lose their gold; the trail references no notes at all).
    if (audioProcessor.getChartVersion() != noteHitVersion)
    {
        noteHitVersion = audioProcessor.getChartVersion();
        remapNoteHitSpans();
        remapPercHits();
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
    if (t0 < 0.0 || t < t0 || t - t0 > 0.5)
    {
        hitResetPending  = true;
        percResolvedUpTo = t;   // nothing before the jump target was played
        return;
    }

    // t == t0 is a frame that landed inside one audio block. On the REAPER path that is
    // fine while playing - the hops drained above still need scoring, and returning here
    // lost them for good - and the play-state check below handles stopped. The file
    // fallback has no play state, so a repeated time is its only "stopped" signal.
    if (t == t0 && ! audioProcessor.reaperApi.isLoaded())
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
        for (auto& h : percHits)
            h = {};
        percResolvedUpTo = t0;  // the new pass starts here; earlier windows stay pending
        pitchTrail.clear();
        lastHopT      = -1.0;
        lastHopMidiF  = -1.0f;
        lastHopVoiced = false;
    }

    // Degenerate-session guard (~19 min of continuous singing at one sample per hop,
    // ~86-94/s) — drop the older half rather than growing without bound.
    if (pitchTrail.size() > 100000)
        pitchTrail.erase (pitchTrail.begin(),
                          pitchTrail.begin() + (std::ptrdiff_t) (pitchTrail.size() / 2));

    // Look-back once for the whole frame — note times don't change mid-frame, so every
    // hop-to-hop interval below reuses this same starting point. The earliest interval
    // starts at the carried hop, or at the first drained one, which can sit well before
    // t0 (see the header comment).
    double scanFrom = t0;
    if (lastHopT >= 0.0)      scanFrom = std::min (scanFrom, lastHopT);
    if (! hopScratch.empty()) scanFrom = std::min (scanFrom, hopScratch.front().time - calibSec);

    auto itStart = std::lower_bound (notes.begin(), notes.end(), scanFrom - kNoteLookback,
        [] (const NoteInfo& n, double v) { return n.startTime < v; });
    while (itStart != notes.begin())
    {
        const auto prev = std::prev (itStart);
        if (prev->endTime <= scanFrom) break;
        itStart = prev;
    }

    // Scores one [segStart, segEnd) hop-to-hop interval — same windowed scan as the
    // traffic light, but per interval instead of per frame.
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

    for (const auto& hop : hopScratch)
    {
        const double hopT = hop.time - calibSec;

        // Two timelines, two ordering checks. Scoring runs on hopT (calibrated), which
        // steps backwards when the Input calib - or, with Live-mic used, the Output calib -
        // is raised mid-pass: a hop that does not move forward there is not scored, so no
        // interval runs backwards and no tap is dated before the last one. The trail runs
        // on raw hop.time, which no calibration touches; its own check only guards the
        // start of a project, where several hops are clamped to time 0 and the take
        // segmenter needs strictly ascending samples. Either way the hop's state is taken
        // below, so the next interval starts from the right pitch.
        const bool scoreable = lastHopT < 0.0 || hopT > lastHopT;

        // Percussion: a tap is the gate opening, i.e. this hop open while the previous one
        // was closed. `voiced` is the raw RMS gate, not "pitch found", so an unpitched mic
        // tap counts. A tap carries its own timestamp; first tap in a window wins.
        if (scoreable && rbperc::isTap (lastHopVoiced, hop.voiced))
        {
            const int pi = rbperc::noteAt (perc, hopT);
            if (pi >= 0 && (size_t) pi < percHits.size()
                && percHits[(size_t) pi].result == rbperc::Result::Pending)
                percHits[(size_t) pi] = { rbperc::Result::Hit, hopT };
        }

        // Trail capture (skipped when the Pitch Trail setting is off): one sample per
        // hop. A held MIDI estimate after the RMS gate has closed is a live-display
        // convenience, not evidence for a saved take, so it adds a gap marker instead.
        // Stored at the raw hop time — drawHighway applies the calibration, and the take
        // export wants true project time so its MIDI lines up with the audio waveform.
        if (pitchTrailOn && (pitchTrail.empty() || hop.time > pitchTrail.back().time))
        {
            if (hop.voiced && hop.midiFloat >= 0.0f)
                pitchTrail.push_back ({ hop.time, hop.midiFloat,
                                        hop.octaveRatio, hop.octaveDoubt });
            else if (! pitchTrail.empty() && pitchTrail.back().pitch >= 0.0f)
                pitchTrail.push_back ({ hop.time, -1.0f });
        }

        // Close the interval the previous hop opened. Capped, so a hole in the hop stream
        // - or the forward jump a lowered calibration makes - cannot stretch one estimate
        // across it.
        if (scoreable)
        {
            if (lastHopT >= 0.0)
                scoreSegment (lastHopT, std::min (hopT, lastHopT + kMaxHopGapSec),
                              lastHopVoiced, lastHopMidiF);
            lastHopT = hopT;
        }

        lastHopMidiF  = hop.midiFloat;
        lastHopVoiced = hop.voiced;
    }

    // Percussion windows that closed without a tap are misses - decided kResolveLagSec
    // late, because a hop can reach us well after the frame whose time it falls in (see
    // the tap check above), and a tap at the end of a window must still be able to land.
    // Only windows past percResolvedUpTo: a seek past a note is a discontinuity, which
    // moves that mark, and must leave the note pending rather than call it missed.
    {
        const double resolveTo = t - rbperc::kResolveLagSec;
        auto pit = std::partition_point (perc.begin(), perc.end(),
            [this] (const PercussionNote& p) { return p.window.hi <= percResolvedUpTo; });
        for (; pit != perc.end() && pit->window.hi <= resolveTo; ++pit)
        {
            const auto pi = (size_t) (pit - perc.begin());
            if (pi < percHits.size() && percHits[pi].result == rbperc::Result::Pending)
                percHits[pi].result = rbperc::Result::Miss;
        }
        percResolvedUpTo = std::max (percResolvedUpTo, resolveTo);
    }
}

// Re-aligns percHits to a freshly reloaded chart, matching on start time — a percussion
// note has no pitch or part to compare, and its length is not meaningful. Parks across a
// transient empty read the same way remapNoteHitSpans does; "empty" means the whole chart,
// since a chart with no percussion at all is the common case and not a failed read.
void RBVocalSingAudioProcessorEditor::remapPercHits()
{
    const auto& perc = audioProcessor.getChartPercussion();

    if (perc.empty() && audioProcessor.getChartNotes().empty())
    {
        if (! percHitKeys.empty() && parkedPercHits.empty())
            parkedPercHits = std::move (percHits);
        percHits.clear();
        return;
    }

    auto& oldHits = parkedPercHits.empty() ? percHits : parkedPercHits;

    std::vector<double> newKeys (perc.size());
    for (size_t i = 0; i < perc.size(); ++i)
        newKeys[i] = perc[i].startTime;

    percHits = rbperc::remapByStart (percHitKeys, oldHits, newKeys);
    parkedPercHits.clear();
    percHitKeys = std::move (newKeys);
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
    const TrailSample heard = heardDetection();
    const float midiF = heard.pitch;
    const int   note  = midiF >= 0.0f ? juce::jlimit (0, 127, juce::roundToInt (midiF)) : -1;
    const float hz    = midiF >= 0.0f ? 440.0f * std::pow (2.0f, (midiF - 69.0f) / 12.0f) : 0.0f;

    // Content is capped at kWindowW so it doesn't stretch with the window. The band is
    // painted output only — the toggles and buttons that used to sit in a reserved row
    // above this one live in controls row 2 now.
    const int contentW = std::min (area.getWidth(), kWindowW);
    auto row = area.withWidth (contentW).reduced (8, 6);

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
    // display, and is unaffected by the calibration offset. The exception is Smooth
    // playhead while playing: then it follows the drawn estimate so it ticks with the
    // highway, and drops the milliseconds - the real value steps by a whole block and the
    // estimate can be a few ms off, so neither is worth showing at that precision. Stopped,
    // it is always the exact position, which is when the precision is actually used.
    juce::String timeStr = smoothingNow()
        ? juce::String::formatted ("%02d:%02d", (int) displayPlayTime / 60,
                                                (int) displayPlayTime % 60)
        : fmtTime (cachedPlayTime);
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
    // the song every frame (2026-07 paint phase, c3): jump to the earliest note
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
    else
    {
        // Percussion drives the light only when nothing sung is active — a sung note is
        // scored continuously and the singer is singing it. Only the two extremes exist for
        // a tap, so it reuses the vocal labels: GREAT or MISS, and "---" while a window is
        // still open. Latched results rather than live presence (unlike a talkie), because a
        // tap is over in a few ms and would otherwise never be readable.
        switch (rbperc::lightAt (audioProcessor.getChartPercussion(), percHits, t, lastIsPlaying))
        {
            case rbperc::Light::Hit:  lightColour = juce::Colours::limegreen; lightLabel = "GREAT"; break;
            case rbperc::Light::Miss: lightColour = juce::Colours::red;       lightLabel = "MISS";  break;
            case rbperc::Light::Pending:
            case rbperc::Light::None: break;
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

    // ── Octave-guard chip ────────────────────────────────────────────────────
    // Reads the *heard* sample, like everything else on this row, so it describes the
    // audio at the playhead rather than the newest hop analysed.
    //
    // The column is taken only when the guard is on: switching the guard off restores the
    // pre-feature layout exactly, while leaving it on keeps the note name still as the
    // chip comes and goes. The number is the point — the rival strength is what tells the
    // user whether the thresholds suit their material, and a bare flag would hide the
    // near-misses that decide it.
    //
    // Two states, because a rival alone is common and mostly benign (glides and
    // strong-2nd-harmonic vowels raise one): "OCT? nn" in red is the confirmed decision,
    // matching what the highway marks and what Correct mode would move; dim "oct nn" is a
    // rival that was seen but did not persist, shown only as evidence.
    if (audioProcessor.octaveGuard.load (std::memory_order_relaxed) != 0)
    {
        auto octCol = row.removeFromRight (74);

        // The reading is latched in onFrame() and held for kOctChipHoldSec — see the
        // comment there. It outranks the "--" branch below, so a brief unvoiced hop mid-
        // phrase cannot blank a number that is still current enough to read.
        if (octChipVisible)
        {
            const bool doubt = heldOctDoubt;
            const int  pct   = juce::roundToInt (heldOctRatio * 100.0f);

            g.setColour (doubt ? kOctaveDoubtColour : juce::Colours::grey);
            g.setFont   (juce::Font (12.0f, juce::Font::bold));
            g.drawText  ((doubt ? "OCT? " : "oct ") + juce::String (pct),
                         octCol, juce::Justification::centred, false);
        }
        else if (octChipDashVisible)
        {
            // Voiced, guard on, but no rival peak was found at all. Blank would be
            // ambiguous with "guard off" and "not singing", and that ambiguity is what
            // made a missed flag need a conversation to diagnose: a number means the
            // threshold is the thing to adjust, "--" means no rival was located and
            // adjusting it would change nothing.
            g.setColour (juce::Colours::darkgrey);
            g.setFont   (juce::Font (12.0f, juce::Font::bold));
            g.drawText  ("oct --", octCol, juce::Justification::centred, false);
        }
    }

    // ── Note display ─────────────────────────────────────────────────────────
    if (note >= 0)
    {
        g.setColour (juce::Colours::limegreen);
        g.setFont   (juce::Font (26.0f, juce::Font::bold));
        g.drawText  (noteNameFor (note),
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

    // Frames actually drawn per second, or "--" when nothing is asking for any: on a parked
    // transport the idle gate means no frames are wanted, so a number there would describe
    // nothing, and leaving the last busy second's figure up reads as a fault that isn't
    // there. One number is what a user needs — how badly is it lagging.
    const auto text = area.reduced (8, 0);
    juce::String fpsStr = fpsIdle ? juce::String ("--")
                                  : juce::String (juce::roundToInt (displayFps));

#if RB_PAINT_PROFILE
    // Profile builds get the whole accounting: drawn/wanted/ticks. That triple is the
    // diagnosis for a stalling picture — 3/19/58 means we ran 58 times, asked for 19 frames
    // and got 3, so Windows is withholding WM_PAINT; 4/4/4 means REAPER stopped dispatching
    // to us and there was never a frame to withhold; 50/50/60 means our own throttle is
    // rejecting frames. Shown unconditionally rather than only when they disagree, because
    // reading a healthy 60/60/60 is how you confirm the measurement itself is sound.
    // Behind RB_PAINT_PROFILE and not a Debug guard on purpose: Debug's rasterizer is ~9x
    // slower per pixel (Performance lessons), so frame timing has to be measured in Release.
    // The fourth figure is how often the playback position moved (0 while stopped): when it
    // sits below "wanted", the audio buffer is what caps the picture, e.g. 12/12/43/12.
    if (! fpsIdle)
        fpsStr = juce::String (juce::roundToInt (displayFps))
               + "/" + juce::String (juce::roundToInt (requestedFps))
               + "/" + juce::String (juce::roundToInt (tickFps))
               + "/" + juce::String (juce::roundToInt (positionFps));
#endif
    g.setFont (juce::Font (12.0f, juce::Font::bold));
    g.setColour (juce::Colour (0xff556070));  // always faint
    // Reads "out/in", the two calibrations that actually shape the picture. The
    // live-mic/playback mode only matters when the output offset is non-zero (that is the
    // only thing it redirects), so it is appended then and left off rather than shown as
    // dead text. "(playing)" marks when the output offset is live: it is suppressed while
    // stopped, and the field would otherwise claim a shift that isn't being applied.
    const auto signed_ = [] (int ms) { return (ms > 0 ? "+" : "") + juce::String (ms); };
    juce::String calibStr = signed_ (latencyOffsetMs) + "/" + signed_ (inputOffsetMs) + " ms";
    if (latencyOffsetMs != 0)
    {
        calibStr += liveMicUsed() ? " live" : " playback";
        if (audioProcessor.reaperApi.isLoaded() && ! lastIsPlaying)
            calibStr += " (stopped)";
    }
    juce::String rateStr = juce::String (cachedPlayRate, 2);
    if (! audioProcessor.reaperApi.CSurf_OnPlayRateChange)
        rateStr += " [N/A]";

    juce::String status = fpsStr + " fps   |   REAPER priority: "
                              + (reaperPriority ? "on" : "off")
                              + "   |   renderer: " + (glContext.isAttached() ? "GPU" : "CPU")
                              + "   |   calib: " + calibStr
                              + "   |   rate: " + rateStr;

    // How far the drawn position runs ahead of what is heard (measured, not yet applied).
    // Kept after a stop so it can be read; "--" until a pass has produced one.
    if (audioProcessor.reaperApi.isLoaded())
        status << "   |   lead: "
               << (leadFilter.hasValue() ? juce::String (juce::roundToInt (leadFilter.value() * 1000.0))
                                         : juce::String ("--"))
#if RB_PAINT_PROFILE
               // The newest accepted sample beside the median, to judge how noisy it is.
               << " (raw " << juce::String (juce::roundToInt (leadFilter.lastSample() * 1000.0)) << ")"
#endif
               << " ms";

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
    else if (frameWarnActive)
    {
        // Two faults, two remedies. "Paint too slow" is about what this plugin can manage,
        // so it quotes the achievable rate and the things that reduce our own cost.
        // "Starved" is about frames going missing between asking and drawing, so it quotes
        // the shortfall - and its advice is renderer-aware, because telling someone already
        // on the GPU renderer to switch to the GPU renderer is nonsense, which is exactly
        // what shipped for a day (2026-08-29).
        //
        // Every figure comes from the latched warn* copies, not the live rates. The warning
        // is held for kFrameWarnHoldSec after the fault clears so it can actually be read,
        // and by then the live numbers have recovered and would contradict the message.
        juce::String msg;
        if (warnVerdict == rbperf::FrameVerdict::Starved)
        {
            msg = "[!] Redraws starved - only " + juce::String (juce::roundToInt (warnDelivered))
                + " of " + juce::String (juce::roundToInt (warnRequested))
                + " frames drawn; REAPER's UI thread is busy. ";
            msg += glContext.isAttached() ? "Try a smaller window or a lower Frame cap"
                                          : "Switch Renderer to OpenGL in Settings";
        }
        else
        {
            msg = "[!] Can't keep up (~" + juce::String ((int) std::lround (warnAchievable))
                + " fps) - ";
            msg += reaperPriority ? "shrink window or lower Frame cap"
                                  : "REAPER may lag; shrink window, lower Frame cap, or enable REAPER priority";
        }
        g.setColour (juce::Colour (0xffffb030));
        g.drawText (msg, text, juce::Justification::centredLeft, true);
    }
    else if (bufferHintActive)
    {
        // Advice, not a fault: nothing is broken, the host is reporting the position only
        // this often. Below the frame warning (a real fault outranks it) and above the chart
        // status. A calmer colour than the warning's amber for the same reason. Figures are
        // the latched copies, as with the warning. The block size is quoted because it is
        // the setting to change; it was never part of the decision.
        juce::String msg = "Highway limited to " + juce::String (juce::roundToInt (hintPositionFps))
                         + " fps by the audio buffer";
        if (hintBlockSize > 0)
            msg += " (" + juce::String (hintBlockSize) + " samples)";
        msg += " - lower the block size in REAPER's audio settings, or turn on Smooth playhead";
        g.setColour (juce::Colour (0xff7fa7c8));
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
