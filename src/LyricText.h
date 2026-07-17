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
