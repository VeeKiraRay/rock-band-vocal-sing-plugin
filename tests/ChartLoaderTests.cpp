#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>

namespace
{
    // JUCE's MidiFile::convertTimestampTicksToSeconds() assumes 120 BPM
    // (secsPerTick = 0.5 * (1 / ticksPerQuarterNote)) whenever no tempo meta event is
    // present. With ticksPerQuarterNote = 500 that gives exactly 1000 ticks/sec, i.e.
    // 1 tick = 1 ms — no tempo event needed in any fixture below.
    constexpr int    kTicksPerQuarterNote = 500;
    constexpr double kTicksPerSecond      = 1000.0;

    double secToTick (double seconds) { return seconds * kTicksPerSecond; }

    void addNote (juce::MidiMessageSequence& seq, double startSec, double endSec, int pitch)
    {
        seq.addEvent (juce::MidiMessage::noteOn (1, pitch, (juce::uint8) 100)
                          .withTimeStamp (secToTick (startSec)));
        seq.addEvent (juce::MidiMessage::noteOff (1, pitch)
                          .withTimeStamp (secToTick (endSec)));
    }

    // Type 5 = the MIDI "lyric" meta event, what this project's own charts use.
    void addLyric (juce::MidiMessageSequence& seq, double atSec, const juce::String& text)
    {
        seq.addEvent (juce::MidiMessage::textMetaEvent (5, text).withTimeStamp (secToTick (atSec)));
    }

    // Type 1 = a generic "text" meta event. This is what real RB exports use for lyrics
    // (RBN/Magma convention), so both types must attach. Bracketed text on this same type
    // is animation/marker data and must NOT attach.
    void addTextLyric (juce::MidiMessageSequence& seq, double atSec, const juce::String& text)
    {
        seq.addEvent (juce::MidiMessage::textMetaEvent (1, text).withTimeStamp (secToTick (atSec)));
    }

    void addEndMarker (juce::MidiMessageSequence& seq, double atSec, const juce::String& text = "[end]")
    {
        // Generic text meta event (type 1) — matches isTextMetaEvent()'s 1..15 range.
        seq.addEvent (juce::MidiMessage::textMetaEvent (1, text).withTimeStamp (secToTick (atSec)));
    }

    // Writes each (track name, events) pair to its own track (prefixed with a
    // track-name meta event) in an in-memory MIDI file, then to a TemporaryFile, and
    // returns that file. No binary fixtures are committed to the repo (TEST_PLAN.md sec 4).
    juce::File writeMidiFixture (juce::TemporaryFile& tempFile,
                                  std::vector<std::pair<juce::String, juce::MidiMessageSequence>> tracks)
    {
        juce::MidiFile midi;
        midi.setTicksPerQuarterNote (kTicksPerQuarterNote);

        for (auto& track : tracks)
        {
            juce::MidiMessageSequence seq;
            seq.addEvent (juce::MidiMessage::textMetaEvent (3, track.first).withTimeStamp (0.0));
            for (int i = 0; i < track.second.getNumEvents(); ++i)
                seq.addEvent (track.second.getEventPointer (i)->message);
            seq.updateMatchedPairs();
            midi.addTrack (seq);
        }

        const juce::File file = tempFile.getFile();
        {
            juce::FileOutputStream stream (file);
            midi.writeTo (stream);
        }
        return file;
    }
}

// ─── Solo fixture cases ────────────────────────────────────────────────────────

TEST_CASE ("chartloader solo: notes survive with correct start/end/pitch", "[chartloader][solo]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote (vocals, 1.0, 1.5, 60);
    addNote (vocals, 2.0, 2.7, 64);
    addNote (vocals, 3.0, 3.2, 67);

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    REQUIRE (notes.size() == 3);
    REQUIRE (notes[0].startTime == Catch::Approx (1.0).margin (0.001));
    REQUIRE (notes[0].endTime   == Catch::Approx (1.5).margin (0.001));
    REQUIRE (notes[0].pitch == 60);
    REQUIRE (notes[1].pitch == 64);
    REQUIRE (notes[2].pitch == 67);
}

TEST_CASE ("chartloader solo: lyric attaches within 50ms, not beyond", "[chartloader][solo]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote  (vocals, 5.0, 5.5, 60);
    addLyric (vocals, 5.03, "near");   // 30ms inside the window -> attaches

    addNote  (vocals, 10.0, 10.5, 62);
    addLyric (vocals, 10.06, "far");   // 60ms outside the window -> does not attach

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    REQUIRE (notes.size() == 2);
    REQUIRE (notes[0].lyric == "near");
    REQUIRE (notes[1].lyric.isEmpty());
}

TEST_CASE ("chartloader solo: type-1 text events attach as lyrics", "[chartloader][solo]")
{
    // Regression: accepting only type 5 made every real RB export load with a full note
    // chart and no lyrics at all — the RBN/Magma convention writes them as type 1.
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote      (vocals, 1.0, 1.5, 60); addTextLyric (vocals, 1.0, "Don't");
    addNote      (vocals, 2.0, 2.5, 62); addTextLyric (vocals, 2.0, "you");
    addNote      (vocals, 3.0, 3.5, 64); addLyric     (vocals, 3.0, "still");  // type 5 still works

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    REQUIRE (notes.size() == 3);
    REQUIRE (notes[0].lyric == "Don't");
    REQUIRE (notes[1].lyric == "you");
    REQUIRE (notes[2].lyric == "still");
}

TEST_CASE ("chartloader solo: bracketed marker text never becomes a lyric", "[chartloader][solo]")
{
    // The other half of type-1 support: animation/venue cues share the event type with
    // lyrics, and only the bracket separates them. Accepting type 1 naively would print
    // stage directions in the lyric strip.
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote      (vocals, 1.0, 1.5, 60); addTextLyric (vocals, 1.0, "[idle]");
    addNote      (vocals, 2.0, 2.5, 62); addTextLyric (vocals, 2.0, "  [tambourine_start]");
    addNote      (vocals, 3.0, 3.5, 64); addTextLyric (vocals, 3.0, "real");

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    REQUIRE (notes.size() == 3);
    REQUIRE (notes[0].lyric.isEmpty());   // [idle] is not a lyric
    REQUIRE (notes[1].lyric.isEmpty());   // leading whitespace must not defeat the check
    REQUIRE (notes[2].lyric == "real");
}

TEST_CASE ("chartloader solo: an [end] marker on the vocal track is not a lyric",
           "[chartloader][solo]")
{
    // [end] lives on EVENTS, but a stray copy on PART VOCALS must not attach to the note
    // beside it now that type 1 is accepted for lyrics.
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote      (vocals, 1.0, 1.5, 60);
    addEndMarker (vocals, 1.0);

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    REQUIRE (notes.size() == 1);
    REQUIRE (notes[0].lyric.isEmpty());
}

TEST_CASE ("chartloader solo: suffix flags classify talkie/hidden lyrics", "[chartloader][solo]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote  (vocals, 1.0, 1.5, 60); addLyric (vocals, 1.0, "word#");
    addNote  (vocals, 2.0, 2.5, 61); addLyric (vocals, 2.0, "word^");
    addNote  (vocals, 3.0, 3.5, 62); addLyric (vocals, 3.0, "word$");

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    REQUIRE (notes.size() == 3);
    REQUIRE (notes[0].isTalkie == true);
    REQUIRE (notes[0].isHidden == false);
    REQUIRE (notes[1].isTalkie == true);
    REQUIRE (notes[1].isHidden == false);
    REQUIRE (notes[2].isTalkie == false);
    REQUIRE (notes[2].isHidden == true);
}

TEST_CASE ("chartloader solo: mixed suffix sets both flags (CODE_REVIEW.md 2.1)", "[chartloader][solo]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote  (vocals, 1.0, 1.5, 60); addLyric (vocals, 1.0, "word#$");
    addNote  (vocals, 2.0, 2.5, 61); addLyric (vocals, 2.0, "word$#");  // order-swapped run

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    // Classification walks the whole trailing suffix run, so each character sets its
    // own flag regardless of order within the run.
    const auto& notes = proc.getChartNotes();
    REQUIRE (notes.size() == 2);
    REQUIRE (notes[0].isTalkie == true);
    REQUIRE (notes[0].isHidden == true);
    REQUIRE (notes[1].isTalkie == true);
    REQUIRE (notes[1].isHidden == true);
}

TEST_CASE ("chartloader solo: marker pitches route to sections/phrases/overdrives, never become notes", "[chartloader][solo]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote (vocals, 1.0,  1.5,  60);   // real note before the Range Shift marker
    addNote (vocals, 10.0, 10.2, 0);    // pitch 0: Range Shift boundary
    addNote (vocals, 20.0, 20.5, 62);   // real note after the marker
    addNote (vocals, 30.0, 30.5, 105);  // pitch 105: phrase marker
    addNote (vocals, 40.0, 40.5, 116);  // pitch 116: overdrive marker
    addNote (vocals, 50.0, 50.5, 100);  // pitch >=96: dropped entirely, no side effect

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    REQUIRE (notes.size() == 2);
    REQUIRE (notes[0].pitch == 60);
    REQUIRE (notes[1].pitch == 62);

    REQUIRE (proc.getChartPitchSections().size() == 2);
    REQUIRE (proc.getChartPhrases().size() == 1);
    REQUIRE (proc.getChartPhrases()[0].startTime == Catch::Approx (30.0).margin (0.001));
    REQUIRE (proc.getChartOverdrives().size() == 1);
    REQUIRE (proc.getChartOverdrives()[0].startTime == Catch::Approx (40.0).margin (0.001));
}

TEST_CASE ("chartloader solo: chartMinPitch/chartMaxPitch pad real notes by 2 semitones", "[chartloader][solo]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote (vocals, 1.0, 1.5, 40);
    addNote (vocals, 2.0, 2.5, 70);

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    REQUIRE (proc.chartMinPitch == 38);
    REQUIRE (proc.chartMaxPitch == 72);
}

TEST_CASE ("chartloader solo: songEndTime from [end] event on EVENTS track, trimmed", "[chartloader][solo]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote (vocals, 1.0, 5.0, 60);

    juce::MidiMessageSequence events;
    addEndMarker (events, 180.0, " [end] ");  // proves the production .trim() call works

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", events } });
    proc.loadMidiFile (file);

    REQUIRE (proc.songEndTime == Catch::Approx (180.0).margin (0.001));
}

TEST_CASE ("chartloader solo: songEndTime fallback uses max end time, not last-starting note (CODE_REVIEW.md 1.4)",
           "[chartloader][solo]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote (vocals, 10.0, 50.0, 60);  // starts first, ends latest
    addNote (vocals, 20.0, 25.0, 62);  // starts later, ends earlier -> sorts last by start time

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    // No [end] event, so songEndTime falls back to the true max end time — not
    // chartNotes.back().endTime, which on a start-time-sorted vector would be the
    // last-STARTING note's end (25.0 here).
    REQUIRE (proc.songEndTime == Catch::Approx (50.0).margin (0.001));
}

TEST_CASE ("chartloader solo: missing PART VOCALS track reports status with track listing", "[chartloader][solo]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence drums;
    addNote (drums, 1.0, 1.5, 40);

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART DRUMS", drums } });
    proc.loadMidiFile (file);

    REQUIRE (proc.getChartNotes().empty());
    REQUIRE (proc.chartStatus.contains ("No 'PART VOCALS' track found"));
    REQUIRE (proc.chartStatus.contains ("PART DRUMS"));
}

// ─── Harmonies fixture cases ───────────────────────────────────────────────────

TEST_CASE ("chartloader harmonies: notes tagged with the correct VocalPart", "[chartloader][harmonies]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Harmonies, std::memory_order_relaxed);

    juce::MidiMessageSequence harm1, harm2, harm3;
    addNote (harm1, 1.0, 1.5, 60);
    addNote (harm2, 2.0, 2.5, 64);
    addNote (harm3, 3.0, 3.5, 67);

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "HARM1", harm1 }, { "HARM2", harm2 }, { "HARM3", harm3 } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    REQUIRE (notes.size() == 3);
    REQUIRE (notes[0].part == VocalPart::Main);   // HARM1 maps to VocalPart::Main
    REQUIRE (notes[1].part == VocalPart::Harm2);
    REQUIRE (notes[2].part == VocalPart::Harm3);
}

TEST_CASE ("chartloader harmonies: HARM2 unison with Main gets thickScale 1.3", "[chartloader][harmonies]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Harmonies, std::memory_order_relaxed);

    juce::MidiMessageSequence harm1, harm2;
    addNote (harm1, 10.0, 12.0, 60);
    addNote (harm2, 10.5, 11.5, 60);  // same pitch, overlapping time range

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "HARM1", harm1 }, { "HARM2", harm2 } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    const auto  it = std::find_if (notes.begin(), notes.end(),
                                    [] (const NoteInfo& n) { return n.part == VocalPart::Harm2; });
    REQUIRE (it != notes.end());
    REQUIRE (it->thickScale == Catch::Approx (1.3f));
}

TEST_CASE ("chartloader harmonies: HARM3 unison with Main gets thickScale 1.6", "[chartloader][harmonies]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Harmonies, std::memory_order_relaxed);

    juce::MidiMessageSequence harm1, harm3;
    addNote (harm1, 10.0, 12.0, 60);
    addNote (harm3, 10.5, 11.5, 60);

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "HARM1", harm1 }, { "HARM3", harm3 } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    const auto  it = std::find_if (notes.begin(), notes.end(),
                                    [] (const NoteInfo& n) { return n.part == VocalPart::Harm3; });
    REQUIRE (it != notes.end());
    REQUIRE (it->thickScale == Catch::Approx (1.6f));
}

TEST_CASE ("chartloader harmonies: HARM3 unison with Harm2 also gets thickScale 1.6", "[chartloader][harmonies]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Harmonies, std::memory_order_relaxed);

    // A HARM1 track must be present (even empty) or loadMidiFile bails out early with
    // "No 'HARM1' track found" before HARM2/HARM3 are ever read.
    juce::MidiMessageSequence harm1, harm2, harm3;
    addNote (harm2, 10.0, 12.0, 60);
    addNote (harm3, 10.5, 11.5, 60);

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "HARM1", harm1 }, { "HARM2", harm2 }, { "HARM3", harm3 } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    const auto  it = std::find_if (notes.begin(), notes.end(),
                                    [] (const NoteInfo& n) { return n.part == VocalPart::Harm3; });
    REQUIRE (it != notes.end());
    REQUIRE (it->thickScale == Catch::Approx (1.6f));
}

TEST_CASE ("chartloader harmonies: diagonal (different-pitch) overlap keeps thickScale 1.0", "[chartloader][harmonies]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Harmonies, std::memory_order_relaxed);

    juce::MidiMessageSequence harm1, harm2;
    addNote (harm1, 10.0, 12.0, 60);
    addNote (harm2, 10.0, 12.0, 64);  // same time range, different pitch -> not a unison

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "HARM1", harm1 }, { "HARM2", harm2 } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    const auto  it = std::find_if (notes.begin(), notes.end(),
                                    [] (const NoteInfo& n) { return n.part == VocalPart::Harm2; });
    REQUIRE (it != notes.end());
    REQUIRE (it->thickScale == Catch::Approx (1.0f));
}

TEST_CASE ("chartloader harmonies: HARM3 lyric deduped against a matching nearby HARM2 lyric", "[chartloader][harmonies]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Harmonies, std::memory_order_relaxed);

    // A HARM1 track must be present (even empty) or loadMidiFile bails out early with
    // "No 'HARM1' track found" before HARM2/HARM3 are ever read.
    juce::MidiMessageSequence harm1, harm2, harm3;
    addNote  (harm2, 20.0, 20.5, 60); addLyric (harm2, 20.0, "hello");
    addNote  (harm3, 20.02, 20.5, 64); addLyric (harm3, 20.02, "hello");   // 20ms apart, matching text -> hidden
    addNote  (harm3, 40.0, 40.5, 65); addLyric (harm3, 40.0, "different"); // no match -> not hidden

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "HARM1", harm1 }, { "HARM2", harm2 }, { "HARM3", harm3 } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    const auto  dupIt = std::find_if (notes.begin(), notes.end(),
                                       [] (const NoteInfo& n) { return n.part == VocalPart::Harm3 && n.lyric == "hello"; });
    const auto  uniqIt = std::find_if (notes.begin(), notes.end(),
                                        [] (const NoteInfo& n) { return n.part == VocalPart::Harm3 && n.lyric == "different"; });
    REQUIRE (dupIt  != notes.end());
    REQUIRE (uniqIt != notes.end());
    REQUIRE (dupIt->isHidden  == true);
    REQUIRE (uniqIt->isHidden == false);
}

TEST_CASE ("chartloader harmonies: each part keeps its own phrase grid", "[chartloader][harmonies]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Harmonies, std::memory_order_relaxed);

    juce::MidiMessageSequence harm1, harm2;
    addNote (harm1, 30.0, 30.5, 105);  // HARM1 phrase marker
    addNote (harm2, 60.0, 60.5, 105);  // HARM2 phrase marker

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "HARM1", harm1 }, { "HARM2", harm2 } });
    proc.loadMidiFile (file);

    REQUIRE (proc.getChartPhrases().size() == 1);
    REQUIRE (proc.getChartPhrases()[0].startTime == Catch::Approx (30.0).margin (0.001));
    REQUIRE (proc.getChartHarm2Phrases().size() == 1);
    REQUIRE (proc.getChartHarm2Phrases()[0].startTime == Catch::Approx (60.0).margin (0.001));
}

// ─── Render cache (finalizeChart post-pass, PERFORMANCE_IMPROVEMENT.md c1/c2) ───

TEST_CASE ("render cache: effectiveEnd extends through a '+' chain, breaks on other lyrics",
           "[chartloader][rendercache]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote  (vocals, 1.0, 1.5, 60); addLyric (vocals, 1.0, "bend");  // word with a 2-note "+" chain
    addNote  (vocals, 1.6, 2.0, 62); addLyric (vocals, 1.6, "+");
    addNote  (vocals, 2.1, 2.5, 64); addLyric (vocals, 2.1, "+");
    addNote  (vocals, 3.0, 3.5, 65); addLyric (vocals, 3.0, "plain"); // no chain
    addNote  (vocals, 5.0, 5.5, 60); addLyric (vocals, 5.0, "sing");  // chain broken by hidden "+$"
    addNote  (vocals, 5.6, 6.0, 62); addLyric (vocals, 5.6, "+$");

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    REQUIRE (notes.size() == 6);
    REQUIRE (notes[0].effectiveEnd == Catch::Approx (2.5).margin (0.001));  // through both "+"
    REQUIRE (notes[3].effectiveEnd == Catch::Approx (3.5).margin (0.001));  // == endTime
    // Raw-"+" semantics, matching the editor's original per-frame scan: a hidden "+$"
    // breaks the chain (it only bends on the highway, via displayLyric == "+").
    REQUIRE (notes[4].effectiveEnd == Catch::Approx (5.5).margin (0.001));
}

TEST_CASE ("render cache: effectiveEnd chain skips interleaved other-part notes",
           "[chartloader][rendercache][harmonies]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Harmonies, std::memory_order_relaxed);

    juce::MidiMessageSequence harm1, harm2;
    addNote  (harm1, 10.0, 10.5, 60); addLyric (harm1, 10.0, "hey");
    addNote  (harm2, 10.6, 10.8, 64); addLyric (harm2, 10.6, "yo");  // interleaves in the sorted list
    addNote  (harm1, 11.0, 11.5, 62); addLyric (harm1, 11.0, "+");

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "HARM1", harm1 }, { "HARM2", harm2 } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    const auto  it = std::find_if (notes.begin(), notes.end(),
                                    [] (const NoteInfo& n) { return n.lyric == "hey"; });
    REQUIRE (it != notes.end());
    REQUIRE (it->effectiveEnd == Catch::Approx (11.5).margin (0.001));  // chained across HARM2's note
}

TEST_CASE ("render cache: displayLyric strips notational suffixes only",
           "[chartloader][rendercache]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote  (vocals, 1.0, 1.5, 60); addLyric (vocals, 1.0, "word#$");
    addNote  (vocals, 2.0, 2.5, 61); addLyric (vocals, 2.0, "+$");
    addNote  (vocals, 3.0, 3.5, 62); addLyric (vocals, 3.0, "syl-");  // connector chars stay

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    REQUIRE (notes.size() == 3);
    REQUIRE (notes[0].displayLyric == "word");
    REQUIRE (notes[1].displayLyric == "+");
    REQUIRE (notes[2].displayLyric == "syl-");
}

TEST_CASE ("render cache: syllable/glue split hyphen, equals and word boundary",
           "[chartloader][rendercache]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote  (vocals, 1.0, 1.5, 60); addLyric (vocals, 1.0, "syl-");   // hyphen: joined, no glue
    addNote  (vocals, 2.0, 2.5, 61); addLyric (vocals, 2.0, "syl=");   // equals: rendered hyphen
    addNote  (vocals, 3.0, 3.5, 62); addLyric (vocals, 3.0, "word");   // word boundary: space
    addNote  (vocals, 4.0, 4.5, 63); addLyric (vocals, 4.0, "syl-#");  // suffix stripped first

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    const auto& notes = proc.getChartNotes();
    REQUIRE (notes.size() == 4);
    REQUIRE (notes[0].syllable == "syl");   REQUIRE (notes[0].glue == "");
    REQUIRE (notes[1].syllable == "syl");   REQUIRE (notes[1].glue == "-");
    REQUIRE (notes[2].syllable == "word");  REQUIRE (notes[2].glue == " ");
    REQUIRE (notes[3].syllable == "syl");   REQUIRE (notes[3].glue == "");
}

TEST_CASE ("render cache: pitch-class candidate lists are sorted, deduped, per class",
           "[chartloader][rendercache]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote (vocals, 1.0, 1.5, 60);   // pc 0
    addNote (vocals, 2.0, 2.5, 72);   // pc 0 (out of start order vs 48 below)
    addNote (vocals, 3.0, 3.5, 48);   // pc 0
    addNote (vocals, 4.0, 4.5, 60);   // pc 0 duplicate -> collapses
    addNote (vocals, 5.0, 5.5, 62);   // pc 2

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });
    proc.loadMidiFile (file);

    const auto& cands = proc.getPitchClassCandidates();
    REQUIRE (cands[0] == std::vector<int> { 48, 60, 72 });
    REQUIRE (cands[2] == std::vector<int> { 62 });
    for (const size_t pc : { (size_t) 1, (size_t) 3, (size_t) 7, (size_t) 11 })
        REQUIRE (cands[pc].empty());

    // A reload replaces the lists rather than accumulating into them.
    juce::MidiMessageSequence vocals2;
    addNote (vocals2, 1.0, 1.5, 50);  // pc 2 only
    juce::TemporaryFile tempFile2 (".mid");
    auto file2 = writeMidiFixture (tempFile2, { { "PART VOCALS", vocals2 }, { "EVENTS", {} } });
    proc.loadMidiFile (file2);

    REQUIRE (cands[0].empty());
    REQUIRE (cands[2] == std::vector<int> { 50 });
}

TEST_CASE ("render cache: chartVersion strictly increases across loads",
           "[chartloader][rendercache]")
{
    RBVocalSingAudioProcessor proc;
    proc.vocalMode.store ((int) VocalMode::Solo, std::memory_order_relaxed);

    juce::MidiMessageSequence vocals;
    addNote (vocals, 1.0, 1.5, 60);

    juce::TemporaryFile tempFile (".mid");
    auto file = writeMidiFixture (tempFile, { { "PART VOCALS", vocals }, { "EVENTS", {} } });

    const int v0 = proc.getChartVersion();
    proc.loadMidiFile (file);
    const int v1 = proc.getChartVersion();
    proc.loadMidiFile (file);
    const int v2 = proc.getChartVersion();

    REQUIRE (v1 > v0);
    REQUIRE (v2 > v1);
}
