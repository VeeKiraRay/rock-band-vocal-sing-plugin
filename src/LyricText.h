#pragma once
#include <JuceHeader.h>

// Lyric-text helpers shared between the chart loader (processor side) and the
// editor's paint code. Lives in its own header because processor TUs must not
// include editor headers (EditorVisuals.h) by convention.

// Strips only the trailing notational suffixes (#, ^, $) — leaves connector chars intact.
inline juce::String stripNotationSuffixes (const juce::String& s)
{
    juce::String r = s;
    while (!r.isEmpty())
    {
        const juce::juce_wchar c = r[r.length() - 1];
        if (c == '#' || c == '^' || c == '$') r = r.dropLastCharacters (1);
        else break;
    }
    return r;
}

// Classifies the full trailing suffix run: '#'/'^' → talkie, '$' → hidden. Walking the
// whole run (the same run stripNotationSuffixes strips) means "word#$" and "word$#"
// both set both flags — a last-character check would miss one of them each.
// (CODE_REVIEW.md 2.1)
inline void classifyNotationSuffixes (const juce::String& s, bool& isTalkie, bool& isHidden)
{
    isTalkie = isHidden = false;
    for (int i = s.length(); --i >= 0;)
    {
        const juce::juce_wchar c = s[i];
        if      (c == '#' || c == '^') isTalkie = true;
        else if (c == '$')             isHidden = true;
        else break;
    }
}
