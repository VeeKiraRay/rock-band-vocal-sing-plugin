#pragma once
#include <JuceHeader.h>

// Lyric-text helpers shared between the chart loader (processor side) and the
// editor's paint code. Lives in its own header because processor TUs must not
// include editor headers (EditorVisuals.h) by convention.

// Rock Band charts carry non-lyric text — animation cues ([idle], [mellow], [play]),
// mix/practice markers ([mix 0 drums4], [prc_intro]) and [end] — as MIDI text events on
// the same tracks and the same meta-event types as lyrics. The bracket is the convention
// that separates the two, and it is the only thing that does: both kinds appear as type 1
// on PART VOCALS in real exports, so the loader cannot tell them apart by type.
inline bool isBracketedMarkerText (const juce::String& s)
{
    return s.trimStart().startsWithChar ('[');
}

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

// Removes connector chars from a raw MIDI lyric syllable and returns the glue that
// should follow it: "" (hyphen removed), "-" (equals converted), " " (word boundary).
// Trailing #/^/$ notational suffixes are stripped first.
inline juce::String stripLyricSuffix (const juce::String& raw, juce::String& outGlue)
{
    juce::String s = stripNotationSuffixes (raw);

    if (!s.isEmpty())
    {
        const juce::juce_wchar last = s[s.length() - 1];
        if (last == '-') { outGlue = "";  return s.dropLastCharacters (1); }
        if (last == '=') { outGlue = "-"; return s.dropLastCharacters (1); }
    }

    outGlue = " ";
    return s;
}
