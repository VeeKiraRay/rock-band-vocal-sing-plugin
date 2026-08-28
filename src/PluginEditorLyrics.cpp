#include "PluginEditor.h"
#include "EditorVisuals.h"
#include <algorithm>

// The scrolling / static lyric strips (bottom = Main/HARM1, top = HARM2+HARM3 in
// Harmonies mode). Shared colours and the lyric-suffix helpers live in
// EditorVisuals.h. The highway and stats panels are in PluginEditorHighway.cpp.

// ─── Lyric strip ──────────────────────────────────────────────────────────────

void RBVocalSingAudioProcessorEditor::drawLyricStrip (juce::Graphics& g,
                                                       juce::Rectangle<int> area,
                                                       double t,
                                                       bool   harmonyStrip)
{
    g.setColour (kLyricBg);
    g.fillRect  (area);

    const auto& notes = audioProcessor.getChartNotes();
    if (notes.empty()) return;

    // Rebuild the strip fonts when the Lyrics size setting changed — they stay
    // members so per-frame typeface-cache lookups are avoided
    // (2026-07 paint phase, c6).
    const float s = lyricScale;
    if (lyricFontsScale != s)
    {
        lyricFontCurrent = juce::Font (15.0f * s, juce::Font::bold);
        lyricFontPreview = juce::Font (11.0f * s, juce::Font::bold);
        lyricFontsScale  = s;
    }

    // Bottom strip shows Main (HARM1/Solo); top strip shows HARM2+HARM3.
    auto includePart = [harmonyStrip] (VocalPart p)
    {
        return harmonyStrip ? (p != VocalPart::Main) : (p == VocalPart::Main);
    };

    // Overdrive (Star Power) ranges come from HARM1/Solo only; apply to all parts.
    const auto& overdrives = audioProcessor.getChartOverdrives();
    auto isInOverdrive = [&] (double noteTime) -> bool {
        for (const auto& od : overdrives)
            if (noteTime >= od.startTime && noteTime < od.endTime) return true;
        return false;
    };

    // Text colour for one syllable. Both strips share the gold/white/grey scheme
    // (note bars on the highway already carry the per-part colour coding).
    auto lyricColour = [] (bool isActive, bool isPast, bool inOverdrive) -> juce::Colour
    {
        if (isActive)    return juce::Colour (0xff00e5b0);  // cyan-green, distinct from overdrive gold
        if (isPast)      return juce::Colour (0xff44445a);
        if (inOverdrive) return kOverdriveTextColour;
        return juce::Colours::white;
    };

    // ── Scrolling mode ───────────────────────────────────────────────────────
    if (lyricMode == LyricMode::Scrolling)
    {
        const float phX = area.getX() + area.getWidth() * 0.3f;

        g.setColour (juce::Colour (0xff2a2a44));
        g.drawVerticalLine ((int) phX, (float) area.getY(), (float) area.getBottom());

        const juce::Font& lyricFont = lyricFontCurrent;
        g.setFont (lyricFont);

        // Reset the width cache when the chart or the strip font changed (or on
        // first use); entries are then measured lazily as notes scroll into view.
        if (lyricWidthCacheVersion != audioProcessor.getChartVersion()
            || lyricWidthCacheFontH != lyricFont.getHeight()
            || lyricWidthCache.size() != notes.size())
        {
            lyricWidthCache.assign (notes.size(), -1.0f);
            lyricWidthCacheVersion = audioProcessor.getChartVersion();
            lyricWidthCacheFontH   = lyricFont.getHeight();
        }

        g.saveState();
        g.reduceClipRegion (area);

        // Start at the first note that can reach the left cull margin — everything
        // earlier was an O(elapsed-song) `continue` sweep before
        // (2026-07 paint phase, c1). Pills anchor at startTime, so unlike
        // drawNotesForPart no still-sounding backward extension is needed; the
        // left-margin `continue` below stays as a boundary guard. The margin scales
        // with the font — scaled-up text is wider, and a fixed margin would pop
        // pills in/out at the strip edges.
        const float  cull     = 120.0f * s;
        const double leftTime = t + ((double) area.getX() - (double) cull - (double) phX) / pixPerSec;
        const auto   startIt  = std::lower_bound (notes.begin(), notes.end(), leftTime,
            [] (const NoteInfo& n, double v) { return n.startTime < v; });

        // ── Pass 1: collect visible pills (note index, anchor x, text width) ─────
        pillScratch.clear();
        for (auto it = startIt; it != notes.end(); ++it)
        {
            const size_t i = (size_t) (it - notes.begin());  // width-cache index
            const auto&  n = *it;
            const float x = phX + (float) (n.startTime - t) * pixPerSec;
            if (x > area.getRight() + cull)           break;
            if (!includePart (n.part))                continue;
            if (n.lyric.isEmpty() || n.lyric == "+") continue;
            if (n.isHidden)                           continue;
            if (x < area.getX() - cull)              continue;
            // displayLyric is precomputed at chart load (finalizeChart render cache).
            if (n.displayLyric.isEmpty())             continue;

            float tw = lyricWidthCache[i];
            if (tw < 0.0f)
                tw = lyricWidthCache[i] = lyricFont.getStringWidthFloat (n.displayLyric);

            pillScratch.push_back ({ (int) i, x, x, tw, tw });
        }

        // ── Pass 2: squash guard — de-overlap per the selected mode ──────────────
        // (2026-07 squash-guard work). Pills are in time order and per-strip, so neighbours in
        // pillScratch are the only collision candidates. Text boxes must sit 8·s
        // apart: 3·s bg padding each side plus a 2·s visible gap.
        const float pad = 8.0f * s;
        if (squashGuard == SquashMode::Shrink || squashGuard == SquashMode::Hybrid)
        {
            // Cap each pill's drawn width at the space before the next pill's anchor —
            // fully for Shrink, to at most a 20% squash for Hybrid (the spacing pass
            // below pushes whatever still overlaps).
            for (size_t k = 0; k + 1 < pillScratch.size(); ++k)
            {
                auto&       p     = pillScratch[k];
                const float avail = pillScratch[k + 1].anchorX - p.anchorX - pad;
                float capped = std::min (p.tw, avail);
                if (squashGuard == SquashMode::Hybrid)
                    capped = std::max (capped, 0.8f * p.tw);
                p.drawW = std::max (capped, 12.0f * s);  // keep a legible stub even when
                                                         // the gap is only a few px
            }
        }
        if (squashGuard == SquashMode::Spacing || squashGuard == SquashMode::Hybrid)
        {
            // Greedy push-right: max of two functions continuous in t, so pills glide
            // back onto their anchors as space opens — no popping.
            for (size_t k = 1; k < pillScratch.size(); ++k)
                pillScratch[k].x = std::max (pillScratch[k].anchorX,
                                             pillScratch[k - 1].x + pillScratch[k - 1].drawW + pad);
        }

        // ── Pass 3: draw ─────────────────────────────────────────────────────────
        for (const auto& p : pillScratch)
        {
            // effectiveEnd ("+"-chain) is precomputed at chart load.
            const auto& n = notes[(size_t) p.noteIdx];
            const bool isActive = (n.startTime <= t && t < n.effectiveEnd);
            const bool isPast   = (t >= n.effectiveEnd);

            // Background pill
            const float bgX = std::floor (p.x) - 3.0f * s;
            const float bgY = (float) area.getY() + 2.0f * s;
            const float bgH = (float) area.getHeight() - 4.0f * s;

            if (isActive) g.setColour (juce::Colour (0xff2a2200));
            else          g.setColour (juce::Colour (0xff141428));
            g.fillRoundedRectangle (bgX, bgY, p.drawW + 6.0f * s, bgH, 3.0f * s);

            g.setColour (lyricColour (isActive, isPast, isInOverdrive (n.startTime)));

            juce::GlyphArrangement ga;
            if (p.drawW < p.tw)
            {
                // Capped pill: shrink proportionally, not by condensing. Lay the text
                // out at natural size into a box widened by 1/f, then draw through a
                // uniform scale of f — visually a smaller font at f × the set Lyrics
                // Size, without any per-frame Font construction (PERF c6 rule). f is
                // stable while scrolling (anchor gaps are time-distances × pixPerSec).
                // Below the 0.5 floor the box stops matching the text width, so the
                // fitted-text horizontal squash (to 0.25) + ellipsis are the last
                // resort. The scaled draw centres vertically because the box height
                // is pre-divided by f; float translation keeps sub-pixel positioning.
                const float f = juce::jlimit (0.5f, 1.0f, p.drawW / p.tw);
                ga.addFittedText (lyricFont, n.displayLyric,
                                  0.0f, 0.0f,
                                  p.drawW / f, (float) area.getHeight() / f,
                                  juce::Justification::centredLeft, 1, 0.25f);
                ga.draw (g, juce::AffineTransform::scale (f)
                                 .translated (p.x, (float) area.getY()));
            }
            else
            {
                // Sub-pixel positioning — GlyphArrangement renders at fractional x so
                // the glyph antialiasing shifts smoothly between frames rather than
                // snapping. Same call as pre-squash-guard: byte-identical rendering.
                const float fracX = p.x - std::floor (p.x);
                ga.addFittedText (lyricFont, n.displayLyric,
                                  std::floor (p.x), (float) area.getY(),
                                  juce::jmax (40.0f * s, p.tw + 8.0f * s), (float) area.getHeight(),
                                  juce::Justification::centredLeft, 1);
                ga.draw (g, juce::AffineTransform::translation (fracX, 0.0f));
            }
        }

        g.restoreState();
        return;
    }

    // ── Static mode ──────────────────────────────────────────────────────────
    // Bottom strip uses the HARM1/Solo phrase grid; the top harmony strip uses
    // HARM2's own phrases (HARM2/HARM3 can be phrased differently from the lead).
    const auto& phrases = harmonyStrip ? audioProcessor.getChartHarm2Phrases()
                                       : audioProcessor.getChartPhrases();
    if (phrases.empty()) return;

    // Find current phrase, with up to 2-second pre-roll when the gap from the
    // previous phrase is > 100 ms (prevents jumping over a phrase that just ended).
    int currentIdx = -1;
    for (int i = 0; i < (int) phrases.size(); ++i)
    {
        const double prevEnd      = (i > 0) ? phrases[i - 1].endTime : -1.0e9;
        const double gap          = phrases[i].startTime - prevEnd;
        const double displayStart = (gap > 0.1)
                                    ? std::max (phrases[i].startTime - 2.0, prevEnd)
                                    : phrases[i].startTime;
        if (t >= displayStart && t < phrases[i].endTime)
            { currentIdx = i; break; }
    }
    if (currentIdx < 0) return;

    // Split the strip: top 28-px-scaled row for the current phrase, remainder for the
    // preview. Only the current row is rounded — the preview takes whatever is left of
    // the strip, so the two rows always sum to lyricStripH() at every scale.
    const int currentH = juce::roundToInt (28.0f * s);
    const auto currentArea = area.removeFromTop (currentH);  // area is now previewArea

    g.setColour (juce::Colour (0xff161626));
    g.drawHorizontalLine (currentArea.getBottom(),
                          (float) area.getX(), (float) area.getRight());

    // Render a cached phrase layout into rect, optionally with active/past
    // highlighting. All collecting/measuring happened in getPhraseLayout — this only
    // computes per-frame colour and draws.
    auto drawParts = [&] (const PhraseLayoutCache& layout,
                          juce::Rectangle<int>     rect,
                          const juce::Font&        font,
                          bool                     highlight)
    {
        if (layout.parts.empty()) return;
        g.setFont (font);

        float x = std::max ((float) rect.getX() + 4.0f,
                            rect.getCentreX() - layout.totalW * 0.5f);

        g.saveState();
        g.reduceClipRegion (rect);

        for (const auto& p : layout.parts)
        {
            juce::Colour col;
            if (highlight)
            {
                const bool isActive = (t >= p.start && t < p.end);
                const bool isPast   = (t >= p.end);
                col = lyricColour (isActive, isPast, isInOverdrive (p.start));
            }
            else
            {
                col = juce::Colour (0xff55556a);  // dim preview
            }
            g.setColour (col);

            g.drawText (p.text, (int) x, rect.getY(),
                        juce::jmax (1, (int) p.textW + 1), rect.getHeight(),
                        juce::Justification::centredLeft, false);
            x += p.textW;

            if (!p.glue.isEmpty())
            {
                g.drawText (p.glue, (int) x, rect.getY(),
                            juce::jmax (1, (int) p.glueW + 1), rect.getHeight(),
                            juce::Justification::centredLeft, false);
                x += p.glueW;
            }
        }

        g.restoreState();
    };

    // Current phrase — full highlighting
    drawParts (getPhraseLayout (harmonyStrip, false, currentIdx,
                                phrases[currentIdx], lyricFontCurrent),
               currentArea, lyricFontCurrent, true);

    // Next phrase — dim preview (area is the remaining lower strip)
    if (currentIdx + 1 < (int) phrases.size())
        drawParts (getPhraseLayout (harmonyStrip, true, currentIdx + 1,
                                    phrases[currentIdx + 1], lyricFontPreview),
                   area, lyricFontPreview, false);
}

// Rebuilds (on chart/phrase/font change) or returns the cached syllable layout for one
// (strip, role) slot. The collect logic is the old per-frame collectParts lambda, now
// run only on rebuild; widths are measured here once so drawing is measurement-free.
const RBVocalSingAudioProcessorEditor::PhraseLayoutCache&
RBVocalSingAudioProcessorEditor::getPhraseLayout (bool harmonyStrip, bool preview,
                                                  int phraseIdx, const PhraseInfo& phrase,
                                                  const juce::Font& font)
{
    auto& slot = phraseLayouts[(harmonyStrip ? 2 : 0) + (preview ? 1 : 0)];

    const int version = audioProcessor.getChartVersion();
    if (slot.chartVersion == version && slot.phraseIdx == phraseIdx
        && slot.fontHeight == font.getHeight())
        return slot;

    slot.chartVersion = version;
    slot.phraseIdx    = phraseIdx;
    slot.fontHeight   = font.getHeight();
    slot.parts.clear();
    slot.totalW = 0.0f;

    const auto& notes = audioProcessor.getChartNotes();
    auto it = std::lower_bound (notes.begin(), notes.end(), phrase.startTime - 0.01,
        [] (const NoteInfo& n, double v) { return n.startTime < v; });
    for (; it != notes.end(); ++it)
    {
        const auto& n = *it;
        if (n.startTime >= phrase.endTime)       break;
        // Bottom strip shows Main (HARM1/Solo); top strip shows HARM2+HARM3.
        if (harmonyStrip ? (n.part == VocalPart::Main)
                         : (n.part != VocalPart::Main)) continue;
        if (n.lyric.isEmpty() || n.lyric == "+") continue;
        if (n.isHidden)                          continue;

        // syllable/glue/effectiveEnd precomputed at chart load (finalizeChart).
        slot.parts.push_back ({ n.syllable, n.glue, n.startTime, n.effectiveEnd,
                                n.part, 0.0f, 0.0f });
    }
    if (! slot.parts.empty()) slot.parts.back().glue = "";  // no trailing glue on a phrase

    for (auto& p : slot.parts)
    {
        p.textW = font.getStringWidthFloat (p.text);
        p.glueW = p.glue.isEmpty() ? 0.0f : font.getStringWidthFloat (p.glue);
        slot.totalW += p.textW + p.glueW;
    }

    return slot;
}
