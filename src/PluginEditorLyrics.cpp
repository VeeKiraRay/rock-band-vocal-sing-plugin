#include "PluginEditor.h"
#include "EditorVisuals.h"

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
        g.saveState();
        g.reduceClipRegion (area);

        for (const auto& n : notes)
        {
            const float x = phX + (float) (n.startTime - t) * pixPerSec;
            if (x > area.getRight() + 120.0f)         break;
            if (!includePart (n.part))                continue;
            if (n.lyric.isEmpty() || n.lyric == "+") continue;
            if (n.isHidden)                           continue;
            if (x < area.getX() - 120.0f)            continue;

            const bool isActive = (n.startTime <= t && t < n.endTime);
            const bool isPast   = (t >= n.endTime);

            const juce::String displayLyric = stripNotationSuffixes (n.lyric);
            if (displayLyric.isEmpty()) continue;

            // Background pill
            const float tw  = lyricFont.getStringWidthFloat (displayLyric);
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

    // ── Shared helpers ───────────────────────────────────────────────────────

    struct Part { juce::String text; juce::String glue; double start; double end; VocalPart part; };

    // Build syllable part list for a phrase (filtered to this strip's parts)
    auto collectParts = [&] (const PhraseInfo& phrase) -> std::vector<Part>
    {
        std::vector<Part> parts;
        for (const auto& n : notes)
        {
            if (n.startTime < phrase.startTime - 0.01) continue;
            if (n.startTime >= phrase.endTime)          break;
            if (!includePart (n.part))                  continue;
            if (n.lyric.isEmpty() || n.lyric == "+")    continue;
            if (n.isHidden)                              continue;
            juce::String glue;
            parts.push_back ({ stripLyricSuffix (n.lyric, glue), glue,
                               n.startTime, n.endTime, n.part });
        }
        if (!parts.empty()) parts.back().glue = "";
        return parts;
    };

    // Render a part list into rect, optionally with active/past highlighting
    auto drawParts = [&] (const std::vector<Part>& parts,
                          juce::Rectangle<int>     rect,
                          const juce::Font&        font,
                          bool                     highlight)
    {
        if (parts.empty()) return;
        g.setFont (font);

        float totalW = 0.0f;
        for (const auto& p : parts)
            totalW += font.getStringWidthFloat (p.text) + font.getStringWidthFloat (p.glue);

        float x = std::max ((float) rect.getX() + 4.0f,
                            rect.getCentreX() - totalW * 0.5f);

        g.saveState();
        g.reduceClipRegion (rect);

        for (const auto& p : parts)
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

            const float tw = font.getStringWidthFloat (p.text);
            g.drawText (p.text, (int) x, rect.getY(),
                        juce::jmax (1, (int) tw + 1), rect.getHeight(),
                        juce::Justification::centredLeft, false);
            x += tw;

            if (!p.glue.isEmpty())
            {
                const float gw = font.getStringWidthFloat (p.glue);
                g.drawText (p.glue, (int) x, rect.getY(),
                            juce::jmax (1, (int) gw + 1), rect.getHeight(),
                            juce::Justification::centredLeft, false);
                x += gw;
            }
        }

        g.restoreState();
    };

    // Current phrase — full highlighting
    drawParts (collectParts (phrases[currentIdx]),
               currentArea,
               juce::Font (15.0f, juce::Font::bold),
               true);

    // Next phrase — dim preview (area is the remaining lower strip)
    if (currentIdx + 1 < (int) phrases.size())
        drawParts (collectParts (phrases[currentIdx + 1]),
                   area,
                   juce::Font (11.0f, juce::Font::bold),
                   false);
}
