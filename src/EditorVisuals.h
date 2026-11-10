#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"   // VocalPart
#include "LyricText.h"         // stripNotationSuffixes / stripLyricSuffix
#include <vector>

// Shared colours and small text helpers used across the editor's paint translation
// units (PluginEditor.cpp, PluginEditorHighway.cpp, PluginEditorLyrics.cpp). Declared
// inline so every TU that includes this header gets one definition without duplicate
// symbols.

static const char* const kNoteNames[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// Octave numbering: middle C (MIDI 60) is C3, so the RB vocal band 36-84 reads C1-C5.
//
// This is the Yamaha/Roland convention, and it is the one Rock Band charting works in — the
// point of these names is to be read next to REAPER's MIDI editor, so they have to agree
// with it. Scientific pitch notation is the other convention in common use: it puts middle C
// at C4 and would name the same band C2-C6, one digit higher everywhere. Both describe
// identical MIDI numbers; only the printed digit differs, and nothing functional reads these
// strings.
//
// Deliberately a constant rather than a setting. There is one DAW beside this plugin and it
// shows one convention, so a second display-convention row in Settings would be a control
// nobody moves. Every note name in the plugin is formatted here, so making it a setting
// later is a small, contained change if that ever stops being true.
constexpr int kOctaveOfMidiZero = -2;

// A MIDI pitch as a note name ("D#4"), and the bracketed form the highway's note
// markings draw ("[D#4]").
//
// Both hand back a reference into a table built on first use, because these are called once
// per visible note per frame and paint must not allocate a juce::String — the same rule the
// finalizeChart render cache exists to uphold. 128 entries is the whole MIDI range, so the
// table is tiny and never needs invalidating.
//
// The brackets are not decoration: a lyric can be a bare note letter ("A", "Be"), so an
// unbracketed pitch sitting next to one would be genuinely ambiguous. They are display-only
// and unrelated to the chart loader's bracketed-text rule, which concerns MIDI text events.
inline const juce::String& noteNameFor (int midiPitch)
{
    static const std::vector<juce::String> names = []
    {
        std::vector<juce::String> v;
        v.reserve (128);
        for (int p = 0; p < 128; ++p)
            v.push_back (juce::String (kNoteNames[p % 12])
                             + juce::String (p / 12 + kOctaveOfMidiZero));
        return v;
    }();

    return names[(size_t) juce::jlimit (0, 127, midiPitch)];
}

inline const juce::String& pitchMarkFor (int midiPitch)
{
    static const std::vector<juce::String> marks = []
    {
        std::vector<juce::String> v;
        v.reserve (128);
        for (int p = 0; p < 128; ++p)
            v.push_back ("[" + noteNameFor (p) + "]");
        return v;
    }();

    return marks[(size_t) juce::jlimit (0, 127, midiPitch)];
}

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
// Sung-within-tolerance note fill (hit highlighting). Gold on every part — the RB
// "spark trail" language. A white overlay was tried first and read too faint on cyan
// (green+blue already maxed, only the red channel could move).
inline const juce::Colour kNoteHitColour       { 0xffffd000 };
// Sung-pitch contour of the current pass (pitch trail) — faint white so it reads
// over the dark background and every part colour without competing with the bars.
inline const juce::Colour kPitchTrailColour    { 0x66ffffff };
// Octave-guard doubt: the stretch of trail (and the arrowhead) where the detector may
// have landed an octave too low. Deliberately the same red family as the MISS traffic
// light — they never appear on the same element, and "red = do not trust this" is one
// idea worth stating once. Opaque, unlike the trail it overlays, so a doubtful stretch
// reads at a glance against the faint white contour.
inline const juce::Colour kOctaveDoubtColour   { 0xffff3b30 };
inline const juce::Colour kPhraseSepColour     { 0x55aaaacc };  // muted blue-grey phrase separator
// Percussion notes. The game draws a black core in a white ring; pure black disappears on
// kHighwayBg, so the core is lifted to a grey that still reads as "empty" and the ring
// carries the edge. A hit fills the core with kNoteHitColour, the same gold as a hit note.
inline const juce::Colour kPercRingColour      { 0xffffffff };
inline const juce::Colour kPercCoreColour      { 0xff34343f };
inline const juce::Colour kPercLineColour      { 0x88ffffff };  // timing line through the circle
// A percussion note whose window overlaps singing. An authoring hint, not a detection
// problem, so it avoids every colour that already means something on the highway: the
// Range Shift orange, the octave-doubt red, the hit gold and the three part colours.
inline const juce::Colour kPercOverlapColour   { 0xffff4fd8 };
// Note-marking pitch text ("[D#4]"), drawn beside the inline lyric. The same blue-white
// family as the lyric it sits next to, dimmed, so the lyric stays the primary read and the
// pitch is clearly an annotation of it rather than a second word.
inline const juce::Colour kPitchMarkColour     { 0xaa8fb6d4 };

inline juce::Colour colourForPart (VocalPart part)
{
    switch (part)
    {
        case VocalPart::Harm2: return kHarm2Colour;
        case VocalPart::Harm3: return kHarm3Colour;
        default:               return kNoteColour;
    }
}
