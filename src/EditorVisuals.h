#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"   // VocalPart
#include "LyricText.h"         // stripNotationSuffixes / stripLyricSuffix

// Shared colours and small text helpers used across the editor's paint translation
// units (PluginEditor.cpp, PluginEditorHighway.cpp, PluginEditorLyrics.cpp). Declared
// inline so every TU that includes this header gets one definition without duplicate
// symbols.

static const char* const kNoteNames[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

inline const juce::Colour kHighwayBg   { 0xff080818 };
inline const juce::Colour kNoteColour  { 0xff00d4ff };  // Main / HARM1 — cyan
inline const juce::Colour kHarm2Colour { 0xffd99000 };  // HARM2 — dark amber
inline const juce::Colour kHarm3Colour { 0xffc46a3a };  // HARM3 — burnt brown
// Clearly lighter than kPhraseSepColour so the "now" line the sung-pitch arrow
// points at stands apart from the phrase separators.
inline const juce::Colour kPlayheadCol { 0xff8888aa };
inline const juce::Colour kLyricBg     { 0xff0d0d22 };
inline const juce::Colour kStatsBg     { 0xff0d0d1a };
inline const juce::Colour kControlsBg       { 0xff1a1a1a };
inline const juce::Colour kOverdriveTextColour { 0xffffd000 };  // golden yellow for overdrive lyrics
inline const juce::Colour kPhraseSepColour     { 0x55aaaacc };  // muted blue-grey phrase separator

inline juce::Colour colourForPart (VocalPart part)
{
    switch (part)
    {
        case VocalPart::Harm2: return kHarm2Colour;
        case VocalPart::Harm3: return kHarm3Colour;
        default:               return kNoteColour;
    }
}
