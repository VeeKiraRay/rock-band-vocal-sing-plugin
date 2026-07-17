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

        const juce::Font lyricFont (15.0f, juce::Font::bold);
        g.setFont (lyricFont);

        // Reset the width cache when the chart changed (or on first use); entries are
        // then measured lazily as notes scroll into view.
        if (lyricWidthCacheVersion != audioProcessor.getChartVersion()
            || lyricWidthCache.size() != notes.size())
        {
            lyricWidthCache.assign (notes.size(), -1.0f);
            lyricWidthCacheVersion = audioProcessor.getChartVersion();
        }

        g.saveState();
        g.reduceClipRegion (area);

        // Start at the first note that can reach the left cull margin — everything
        // earlier was an O(elapsed-song) `continue` sweep before
        // (PERFORMANCE_IMPROVEMENT.md c1). Pills anchor at startTime, so unlike
        // drawNotesForPart no still-sounding backward extension is needed; the
        // left-margin `continue` below stays as a boundary guard.
        const double leftTime = t + ((double) area.getX() - 120.0 - (double) phX) / pixPerSec;
        const auto   startIt  = std::lower_bound (notes.begin(), notes.end(), leftTime,
            [] (const NoteInfo& n, double v) { return n.startTime < v; });

        for (auto it = startIt; it != notes.end(); ++it)
        {
            const size_t i = (size_t) (it - notes.begin());  // width-cache index
            const auto&  n = *it;
            const float x = phX + (float) (n.startTime - t) * pixPerSec;
            if (x > area.getRight() + 120.0f)         break;
            if (!includePart (n.part))                continue;
            if (n.lyric.isEmpty() || n.lyric == "+") continue;
            if (n.isHidden)                           continue;
            if (x < area.getX() - 120.0f)            continue;

            // effectiveEnd ("+"-chain) and displayLyric are precomputed at chart load
            // (finalizeChart render cache) — no per-frame scan or string allocation.
            const bool isActive = (n.startTime <= t && t < n.effectiveEnd);
            const bool isPast   = (t >= n.effectiveEnd);

            const juce::String& displayLyric = n.displayLyric;
            if (displayLyric.isEmpty()) continue;

            // Background pill
            float tw = lyricWidthCache[i];
            if (tw < 0.0f)
                tw = lyricWidthCache[i] = lyricFont.getStringWidthFloat (displayLyric);
            const float bgX = std::floor (x) - 3.0f;
            const float bgY = (float) area.getY() + 2.0f;
            const float bgH = (float) area.getHeight() - 4.0f;

            if (isActive) g.setColour (juce::Colour (0xff2a2200));
            else          g.setColour (juce::Colour (0xff141428));
            g.fillRoundedRectangle (bgX, bgY, tw + 6.0f, bgH, 3.0f);

            g.setColour (lyricColour (isActive, isPast, isInOverdrive (n.startTime)));

            // Sub-pixel positioning — GlyphArrangement renders at fractional x so the
            // glyph antialiasing shifts smoothly between frames rather than snapping
            const float fracX = x - std::floor (x);
            juce::GlyphArrangement ga;
            ga.addFittedText (lyricFont, displayLyric,
                              std::floor (x), (float) area.getY(),
                              juce::jmax (40.0f, tw + 8.0f), (float) area.getHeight(),
                              juce::Justification::centredLeft, 1);
            ga.draw (g, juce::AffineTransform::translation (fracX, 0.0f));
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

    // Split the strip: top 28 px for current phrase, remainder for preview
    static constexpr int kCurrentH = 28;
    const auto currentArea = area.removeFromTop (kCurrentH);  // area is now previewArea

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
    {
        const juce::Font curFont (15.0f, juce::Font::bold);
        drawParts (getPhraseLayout (harmonyStrip, false, currentIdx,
                                    phrases[currentIdx], curFont),
                   currentArea, curFont, true);
    }

    // Next phrase — dim preview (area is the remaining lower strip)
    if (currentIdx + 1 < (int) phrases.size())
    {
        const juce::Font prevFont (11.0f, juce::Font::bold);
        drawParts (getPhraseLayout (harmonyStrip, true, currentIdx + 1,
                                    phrases[currentIdx + 1], prevFont),
                   area, prevFont, false);
    }
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
