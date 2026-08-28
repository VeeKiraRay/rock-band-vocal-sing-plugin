#define NOMINMAX
#include "PluginProcessor.h"
#include "LyricText.h"   // stripNotationSuffixes (shared with the editor)
#include "HighwayRange.h" // kVocalLow/kVocalHigh — one definition of the singable band
#include <cmath>
#include <algorithm>

// Chart loading and parsing — both the REAPER-API path (readChartFromReaper) and the
// .mid-file fallback (loadMidiFile), plus the per-note parser and the file-local
// helpers they share (MIDI text decoding, pitch-section building, harmony post-passes).
// All members of RBVocalSingAudioProcessor, called on the message thread only.

// ─── MIDI text decoding ───────────────────────────────────────────────────────
// MIDI text meta events (type 5 = lyric) carry bytes whose encoding is
// unspecified.  Reaper's API returns UTF-8.  Raw .mid files commonly use
// Latin-1.  We validate first: if the bytes are well-formed UTF-8 use them
// directly; otherwise re-encode each byte as Latin-1 → UTF-8.

static juce::String decodeMidiText (const char* bytes, int len)
{
    // Validate UTF-8
    bool valid = true;
    for (int i = 0; i < len && valid; )
    {
        const auto c = (unsigned char) bytes[i];
        int extra = 0;
        if      (c < 0x80)                   extra = 0;
        else if (c < 0xC2 || c >= 0xF5)      { valid = false; break; }
        else if (c < 0xE0)                   extra = 1;
        else if (c < 0xF0)                   extra = 2;
        else                                 extra = 3;
        for (int j = 0; j < extra && valid; ++j)
        {
            if (++i >= len || ((unsigned char) bytes[i] & 0xC0) != 0x80)
                valid = false;
        }
        if (valid) ++i;
    }

    if (valid)
        return juce::String::fromUTF8 (bytes, len);

    // Fall back: interpret as Latin-1
    juce::MemoryOutputStream utf8;
    utf8.preallocate ((size_t) len * 2);
    for (int i = 0; i < len; ++i)
    {
        const auto c = (unsigned char) bytes[i];
        if (c < 0x80)
            utf8.writeByte ((char) c);
        else
        {
            utf8.writeByte ((char) (0xC0 | (c >> 6)));
            utf8.writeByte ((char) (0x80 | (c & 0x3F)));
        }
    }
    return juce::String::fromUTF8 ((const char*) utf8.getData(), (int) utf8.getDataSize());
}

// ─── Phrase separator builder ─────────────────────────────────────────────────
// Precomputes one or two vertical-line times per phrase gap, matching the timing
// logic used by the static lyric strip's phrase-switch display start.

static void buildPhraseSeparators (const std::vector<PhraseInfo>& phrases,
                                    std::vector<double>&            out)
{
    out.clear();
    for (int i = 0; i + 1 < (int) phrases.size(); ++i)
    {
        const double E   = phrases[i].endTime;
        const double S   = phrases[i + 1].startTime;
        const double gap = S - E;

        if (gap < 0.2)
        {
            out.push_back (E + gap * 0.5);
        }
        else if (gap <= 5.0)
        {
            out.push_back (std::max (S - 2.0, E));
        }
        else
        {
            out.push_back (E + 0.2);
            out.push_back (S - 2.0);
        }
    }
}

// ─── Section builder ──────────────────────────────────────────────────────────
// Splits the note list into pitch sections defined by Range Shift markers (pitch 0).
// Each section stores the raw min/max pitch of the notes it contains.
// Sections with no singable notes are omitted.

static void buildPitchSections (const std::vector<NoteInfo>&           notes,
                                  std::vector<std::pair<double,double>> shiftMarkers,
                                  std::vector<PitchSection>&             out)
{
    std::sort (shiftMarkers.begin(), shiftMarkers.end());

    std::vector<double> bounds = { 0.0 };
    for (const auto& m : shiftMarkers) bounds.push_back (m.first);
    bounds.push_back (1.0e9);

    out.clear();
    for (int i = 0; i + 1 < (int) bounds.size(); ++i)
    {
        const double sStart = bounds[i];
        const double sEnd   = bounds[i + 1];
        // transitionDur = length of the Range Shift marker that begins this section.
        // i==0 is before the first marker; i>0 uses shiftMarkers[i-1].
        const float tDur = (i > 0)
                         ? std::max (0.05f, (float) shiftMarkers[i - 1].second)
                         : 0.0f;
        int sMin = 127, sMax = 0;
        for (const auto& n : notes)
        {
            if (n.startTime >= sEnd)  break;
            if (n.startTime < sStart) continue;
            sMin = std::min (sMin, n.pitch);
            sMax = std::max (sMax, n.pitch);
        }
        if (sMin <= sMax)
            out.push_back ({ sStart, sEnd, tDur, sMin, sMax });
    }
}

// ─── Harmony post-processing ──────────────────────────────────────────────────
// Two notes overlap if they share a pitch and their time ranges intersect (a
// sustained unison). Diagonal slides are different pitches, so they never trigger.

static bool notesUnison (const NoteInfo& a, const NoteInfo& b)
{
    return a.pitch == b.pitch
        && a.startTime < b.endTime
        && b.startTime < a.endTime;
}

// Lower parts get thicker bars where they sit on the same sustained note as a
// higher-priority part, so all overlapping parts stay visible (concentric).
static void applyHarmonyOverlapThickness (std::vector<NoteInfo>& notes)
{
    for (auto& n : notes)
    {
        if (n.part == VocalPart::Harm2)
        {
            for (const auto& m : notes)
                if (m.part == VocalPart::Main && notesUnison (n, m)) { n.thickScale = 1.3f; break; }
        }
        else if (n.part == VocalPart::Harm3)
        {
            for (const auto& m : notes)
                if ((m.part == VocalPart::Main || m.part == VocalPart::Harm2) && notesUnison (n, m))
                    { n.thickScale = 1.6f; break; }
        }
    }
}

// Hide a HARM3 lyric in the strip when HARM2 already shows the same word at the
// same time (saves a redundant draw). Differing text/timing keeps both so the
// author can spot a mismatch to fix. The HARM3 note bar still draws.
static void dedupHarmonyLyrics (std::vector<NoteInfo>& notes)
{
    for (auto& n : notes)
    {
        if (n.part != VocalPart::Harm3 || n.lyric.isEmpty()) continue;
        const juce::String txt = stripNotationSuffixes (n.lyric);
        if (txt.isEmpty()) continue;

        for (const auto& m : notes)
        {
            if (m.part != VocalPart::Harm2 || m.lyric.isEmpty()) continue;
            if (std::abs (m.startTime - n.startTime) < 0.03
                && stripNotationSuffixes (m.lyric) == txt)
            {
                n.isHidden = true;
                break;
            }
        }
    }
}

// ─── Shared loader bookends (2026-07 code review, 3.2) ───────────────────────

// Resets all chart state to defaults. Both loaders call this first so a failed load
// never leaves the previous chart half-visible.
void RBVocalSingAudioProcessor::clearChart()
{
    chartNotes.clear();
    chartPhrases.clear();
    chartHarm2Phrases.clear();
    chartOverdrives.clear();
    chartPhraseSeparators.clear();
    chartPitchSections.clear();
    for (auto& v : pitchClassCandidates) v.clear();
    chartMinPitch = 48;
    chartMaxPitch = 84;
    songEndTime   = -1.0;
    chartStatus   = {};
    ++chartVersion;  // even a failed load (clear with no finalize) must invalidate editor caches
}

// The identical tail of both loaders — everything between parsing and the status
// string. Assumes clearChart() ran first and the caller has already written any
// explicit [end]-event time into songEndTime (the fallback here only fires when it
// is still unset).
void RBVocalSingAudioProcessor::finalizeChart (std::vector<NoteInfo>&&                 notes,
                                               std::vector<std::pair<double,double>>&& shiftMarkers,
                                               int minP, int maxP)
{
    std::sort (notes.begin(), notes.end(),
               [] (const NoteInfo& a, const NoteInfo& b) { return a.startTime < b.startTime; });

    chartNotes = std::move (notes);

    if (vocalMode.load (std::memory_order_relaxed) == (int) VocalMode::Harmonies)
    {
        applyHarmonyOverlapThickness (chartNotes);
        dedupHarmonyLyrics (chartNotes);
    }

    // Content-derived render cache (2026-07 paint phase, c1/c2): precompute what
    // paint() used to re-derive per frame per visible note. Runs after the harmony
    // passes (they never mutate lyric text). effectiveEnd replicates the editor's old
    // per-frame scan exactly: same-part raw-"+" notes chain the end time forward, any
    // other same-part lyric — including a hidden "+$" — breaks the chain.
    for (size_t i = 0; i < chartNotes.size(); ++i)
    {
        auto& n = chartNotes[i];
        n.displayLyric = stripNotationSuffixes (n.lyric);
        n.syllable     = stripLyricSuffix (n.lyric, n.glue);
        n.effectiveEnd = n.endTime;
        for (size_t j = i + 1; j < chartNotes.size(); ++j)
        {
            const auto& nx = chartNotes[j];
            if (nx.part != n.part) continue;
            if (nx.lyric != "+")   break;
            n.effectiveEnd = nx.endTime;
        }
    }

    // Octave-wrap candidate pool (2026-07 paint phase, c4): chart pitches grouped
    // by pitch class, sorted + deduped once — replaces wrapPitchIntoRange's per-call
    // full-note scan. Chart pitches are 1..95, so plain % 12 is safe.
    for (const auto& n : chartNotes)
        pitchClassCandidates[(size_t) (n.pitch % 12)].push_back (n.pitch);
    for (auto& v : pitchClassCandidates)
    {
        std::sort (v.begin(), v.end());
        v.erase (std::unique (v.begin(), v.end()), v.end());
    }

    ++chartVersion;

    if (minP <= maxP)
    {
        // Octave-wrap scoring range only — the highway draws the section min/max through
        // rbhighway::visibleRange instead. Clamped into the same vocal band so a stray
        // out-of-range note cannot widen the wrap target either; the clamp only ever
        // narrows, so a well-formed chart is unaffected. The minimum-span rule is
        // deliberately NOT applied here: widening the wrap range would send a sung pitch
        // to a different octave than the one on screen.
        chartMinPitch = std::max (rbhighway::kVocalLow,  minP - 2);
        chartMaxPitch = std::min (rbhighway::kVocalHigh, maxP + 2);
        if (chartMinPitch > chartMaxPitch)   // content entirely outside the band
        {
            chartMinPitch = rbhighway::kVocalLow;
            chartMaxPitch = rbhighway::kVocalHigh;
        }
    }

    buildPitchSections    (chartNotes, std::move (shiftMarkers), chartPitchSections);
    buildPhraseSeparators (chartPhrases, chartPhraseSeparators);

    // Fallback: the latest note end. chartNotes is sorted by START time, so back()
    // is not necessarily the last-ending note (a long sustain can outlast a short
    // final stab) — take the max end explicitly. (2026-07 code review, 1.4)
    if (songEndTime <= 0.0 && !chartNotes.empty())
        songEndTime = std::max_element (chartNotes.begin(), chartNotes.end(),
                          [] (const NoteInfo& a, const NoteInfo& b) { return a.endTime < b.endTime; })
                          ->endTime;
}

// ─── readChartFromReaper (UI thread only) ─────────────────────────────────────

void RBVocalSingAudioProcessor::reloadChart()
{
    if (reaperApi.isLoaded())
        readChartFromReaper();
    else if (lastMidiFile.existsAsFile())
        loadMidiFile (lastMidiFile);
}

void RBVocalSingAudioProcessor::readChartFromReaper()
{
    clearChart();

    if (!reaperApi.isLoaded())
    {
        chartStatus = "Reaper API not available";
        return;
    }

    const bool harmonies = (vocalMode.load (std::memory_order_relaxed) == (int) VocalMode::Harmonies);

    // Find tracks. Solo: one "PART VOCALS". Harmonies: HARM1/HARM2/HARM3.
    const int numTracks = reaperApi.CountTracks (nullptr);
    void* mainTrack   = nullptr;   // PART VOCALS or HARM1
    void* harm2Track  = nullptr;
    void* harm3Track  = nullptr;
    void* eventsTrack = nullptr;
    juce::StringArray trackNames;
    char nameBuf[256];

    for (int t = 0; t < numTracks; ++t)
    {
        void* track = reaperApi.GetTrack (nullptr, t);
        if (!track) continue;

        nameBuf[0] = '\0';
        reaperApi.GetTrackName (track, nameBuf, (int) sizeof (nameBuf));
        const juce::String name (nameBuf);
        trackNames.add ("  [" + juce::String (t) + "] " + (name.isEmpty() ? "(unnamed)" : name));

        if (harmonies)
        {
            if (name.contains ("HARM1") && mainTrack  == nullptr) mainTrack  = track;
            if (name.contains ("HARM2") && harm2Track == nullptr) harm2Track = track;
            if (name.contains ("HARM3") && harm3Track == nullptr) harm3Track = track;
        }
        else if (name.contains ("PART VOCALS") && mainTrack == nullptr)
        {
            mainTrack = track;
        }
        if (name.contains ("EVENTS") && eventsTrack == nullptr)
            eventsTrack = track;
    }

    if (mainTrack == nullptr)
    {
        chartStatus = (harmonies ? "No 'HARM1' track found."
                                 : "No 'PART VOCALS' track found.")
                    + juce::String ("\n\nTracks in project:\n")
                    + trackNames.joinIntoString ("\n");
        return;
    }

    std::vector<NoteInfo>                 notes;
    std::vector<std::pair<double,double>> shiftMarkers;
    int minP = 127, maxP = 0;

    // Parse every MIDI item on a track, tagging notes with their part. Only the
    // lead (Main) contributes Range Shift sections; phrases go to a per-part list.
    auto parseTrack = [&] (void* track, VocalPart part, bool collectSections,
                           std::vector<PhraseInfo>* phraseList,
                           std::vector<PhraseInfo>* overdriveList)
    {
        if (track == nullptr) return;
        const int numItems = reaperApi.CountTrackMediaItems (track);
        for (int i = 0; i < numItems; ++i)
        {
            void* item = reaperApi.GetTrackMediaItem (track, i);
            if (!item) continue;
            void* take = reaperApi.GetActiveTake (item);
            if (!take || !reaperApi.TakeIsMIDI (take)) continue;

            parseChartNotes (take, item, track, part, collectSections, notes, phraseList, overdriveList, shiftMarkers, minP, maxP);
        }
    };

    parseTrack (mainTrack, VocalPart::Main, true, &chartPhrases, &chartOverdrives);
    if (harmonies)
    {
        parseTrack (harm2Track, VocalPart::Harm2, false, &chartHarm2Phrases, nullptr);
        parseTrack (harm3Track, VocalPart::Harm3, false, nullptr, nullptr);
    }

    // Read [end] event from EVENTS track for total song duration
    if (eventsTrack != nullptr)
    {
        const int numEvItems = reaperApi.CountTrackMediaItems (eventsTrack);
        char evBuf[256];
        bool foundEnd = false;
        for (int i = 0; i < numEvItems && !foundEnd; ++i)
        {
            void* item = reaperApi.GetTrackMediaItem (eventsTrack, i);
            if (!item) continue;
            void* take = reaperApi.GetActiveTake (item);
            if (!take || !reaperApi.TakeIsMIDI (take)) continue;

            int notecnt = 0, cccnt = 0, txtsyxcnt = 0;
            reaperApi.MIDI_CountEvts (take, &notecnt, &cccnt, &txtsyxcnt);

            for (int s = 0; s < txtsyxcnt && !foundEnd; ++s)
            {
                bool   sel = false, mute = false;
                double ppq = 0.0;
                int    type = 0, msgLen = (int) sizeof (evBuf) - 1;
                evBuf[0] = '\0';
                if (reaperApi.MIDI_GetTextSysexEvt (take, s, &sel, &mute, &ppq, &type, evBuf, &msgLen)
                    && (type == 1 || type == 5) && msgLen > 0)
                {
                    evBuf[msgLen] = '\0';
                    if (juce::String (evBuf).trim() == "[end]")
                    {
                        songEndTime = reaperApi.MIDI_GetProjTimeFromPPQPos (take, ppq);
                        foundEnd = true;
                    }
                }
            }
        }
    }
    finalizeChart (std::move (notes), std::move (shiftMarkers), minP, maxP);

    if (harmonies)
    {
        juce::StringArray got;
        if (mainTrack)  got.add ("HARM1");
        if (harm2Track) got.add ("HARM2");
        if (harm3Track) got.add ("HARM3");
        chartStatus = "Loaded " + juce::String ((int) chartNotes.size())
                    + " notes from " + got.joinIntoString (" + ");
    }
    else
    {
        chartStatus = "Loaded " + juce::String ((int) chartNotes.size()) + " notes from PART VOCALS";
    }
}

void RBVocalSingAudioProcessor::parseChartNotes (void* take,
                                                  void*                                  srcItem,
                                                  void*                                  srcTrack,
                                                  VocalPart                              part,
                                                  bool                                   collectSections,
                                                  std::vector<NoteInfo>&                 out,
                                                  std::vector<PhraseInfo>*               outPhrases,
                                                  std::vector<PhraseInfo>*               outOverdrives,
                                                  std::vector<std::pair<double,double>>& outShiftMarkers,
                                                  int& minP, int& maxP)
{
    int notecnt = 0, cccnt = 0, txtsyxcnt = 0;
    reaperApi.MIDI_CountEvts (take, &notecnt, &cccnt, &txtsyxcnt);

    // Collect lyric text events. Both meta types count: type 5 is the MIDI "lyric" event,
    // but the RBN/Magma convention writes PART VOCALS lyrics as type 1 "text" events, and
    // real exports use type 1 exclusively. Accepting only type 5 loaded every note with no
    // lyrics at all on imported charts. Bracketed text on those same types is animation and
    // marker data, never a lyric — see isBracketedMarkerText.
    struct Lyric { double time; juce::String text; };
    std::vector<Lyric> lyrics;

    char msgBuf[1024];
    for (int s = 0; s < txtsyxcnt; ++s)
    {
        bool   sel = false, mute = false;
        double ppq = 0.0;
        int    type = 0, msgLen = (int) sizeof (msgBuf) - 1;
        msgBuf[0] = '\0';

        if (reaperApi.MIDI_GetTextSysexEvt (take, s, &sel, &mute, &ppq, &type, msgBuf, &msgLen)
            && (type == 1 || type == 5) && msgLen > 0)
        {
            msgBuf[msgLen] = '\0';
            const juce::String text = decodeMidiText (msgBuf, msgLen);
            if (isBracketedMarkerText (text)) continue;

            const double t = reaperApi.MIDI_GetProjTimeFromPPQPos (take, ppq);
            lyrics.push_back ({ t, text });
        }
    }

    // Collect notes
    for (int n = 0; n < notecnt; ++n)
    {
        bool   sel = false, mute = false;
        double sppq = 0.0, eppq = 0.0;
        int    chan = 0, pitch = 0, vel = 0;

        if (!reaperApi.MIDI_GetNote (take, n, &sel, &mute, &sppq, &eppq, &chan, &pitch, &vel))
            continue;
        if (vel == 0) continue;

        const double startSec = reaperApi.MIDI_GetProjTimeFromPPQPos (take, sppq);
        const double endSec   = reaperApi.MIDI_GetProjTimeFromPPQPos (take, eppq);

        if (pitch == kRangeShiftPitch)   // records a section boundary, not a singable note
        {
            // Sections come from the lead (HARM1/Solo) only.
            if (collectSections)
                outShiftMarkers.push_back ({ startSec, endSec - startSec });
            continue;
        }
        if (pitch == kLyricShiftPitch)   // RB3 lyric shift: a marker, nothing consumes it yet
            continue;
        if (pitch == kPhraseMarkerPitch)
        {
            // Each part keeps its own phrase grid (bottom strip uses HARM1's,
            // top strip uses HARM2's). outPhrases is null for parts we don't track.
            if (outPhrases != nullptr)
                outPhrases->push_back ({ startSec, endSec });
            continue;
        }
        if (pitch == kOverdrivePitch)    // collected from HARM1/Solo only
        {
            if (outOverdrives != nullptr)
                outOverdrives->push_back ({ startSec, endSec });
            continue;
        }
        if (pitch >= kFirstReservedPitch) continue;  // other special notes

        // Match nearest lyric within 50 ms
        juce::String lyric;
        double bestDist = 0.05;
        for (const auto& l : lyrics)
        {
            const double dist = std::abs (l.time - startSec);
            if (dist < bestDist) { bestDist = dist; lyric = l.text; }
        }

        bool isTalkie = false, isHidden = false;
        classifyNotationSuffixes (lyric, isTalkie, isHidden);
        out.push_back ({ startSec, endSec, pitch, lyric, isTalkie, isHidden, part, 1.0f,
                         take, srcItem, srcTrack, n });
        minP = std::min (minP, pitch);
        maxP = std::max (maxP, pitch);
    }
}

// ─── MIDI file loading (fallback for non-Reaper hosts) ───────────────────────

void RBVocalSingAudioProcessor::loadMidiFile (const juce::File& file)
{
    clearChart();

    lastMidiFile = file;  // remembered so reloadChart() can re-parse on a mode switch

    if (!file.existsAsFile())
    {
        chartStatus = "File not found:\n" + file.getFullPathName();
        return;
    }

    juce::MidiFile midi;
    {
        juce::FileInputStream stream (file);
        if (!stream.openedOk() || !midi.readFrom (stream))
        {
            chartStatus = "Failed to parse MIDI file:\n" + file.getFileName();
            return;
        }
    }

    midi.convertTimestampTicksToSeconds();

    const bool harmonies = (vocalMode.load (std::memory_order_relaxed) == (int) VocalMode::Harmonies);

    int               mainIdx = -1, harm2Idx = -1, harm3Idx = -1, eventsIdx = -1;
    juce::StringArray trackNames;

    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        const auto* seq = midi.getTrack (t);
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& msg = seq->getEventPointer (i)->message;
            if (msg.isTrackNameEvent())
            {
                const auto name = msg.getTextFromTextMetaEvent();
                trackNames.add ("  [" + juce::String (t) + "] " + (name.isEmpty() ? "(unnamed)" : name));
                if (harmonies)
                {
                    if (name.contains ("HARM1") && mainIdx  < 0) mainIdx  = t;
                    if (name.contains ("HARM2") && harm2Idx < 0) harm2Idx = t;
                    if (name.contains ("HARM3") && harm3Idx < 0) harm3Idx = t;
                }
                else if (name.contains ("PART VOCALS") && mainIdx < 0)
                {
                    mainIdx = t;
                }
                if (name.contains ("EVENTS") && eventsIdx < 0)
                    eventsIdx = t;
                break;
            }
        }
    }

    if (mainIdx < 0)
    {
        chartStatus = (harmonies ? "No 'HARM1' track found in:\n"
                                 : "No 'PART VOCALS' track found in:\n")
                    + file.getFileName()
                    + "\n\nTracks in file:\n"
                    + trackNames.joinIntoString ("\n");
        return;
    }

    std::vector<NoteInfo>                 notes;
    std::vector<std::pair<double,double>> shiftMarkers;
    int minP = 127, maxP = 0;

    // Parse one MIDI file track, tagging notes with their part. Only the lead
    // (Main) contributes Range Shift sections; phrases go to a per-part list.
    auto parseSeq = [&] (int trackIdx, VocalPart part, bool collectSections,
                         std::vector<PhraseInfo>* phraseList,
                         std::vector<PhraseInfo>* overdriveList)
    {
        if (trackIdx < 0) return;

        juce::MidiMessageSequence track (*midi.getTrack (trackIdx));
        track.updateMatchedPairs();

        // Type 1 and type 5 both carry lyrics, minus bracketed marker text — kept in sync
        // with the REAPER path in parseChartNotes(); see the comment there for why.
        struct Lyric { double time; juce::String text; };
        std::vector<Lyric> lyrics;
        for (int i = 0; i < track.getNumEvents(); ++i)
        {
            const auto& msg = track.getEventPointer (i)->message;
            if (! msg.isMetaEvent()) continue;

            const int metaType = msg.getMetaEventType();
            if (metaType != 1 && metaType != 5) continue;

            const juce::String text = decodeMidiText ((const char*) msg.getMetaEventData(),
                                                      msg.getMetaEventLength());
            if (isBracketedMarkerText (text)) continue;

            lyrics.push_back ({ msg.getTimeStamp(), text });
        }

        for (int i = 0; i < track.getNumEvents(); ++i)
        {
            const auto* ev  = track.getEventPointer (i);
            const auto& msg = ev->message;

            if (!msg.isNoteOn() || msg.getVelocity() == 0) continue;
            if (!ev->noteOffObject)                         continue;

            const int    pitch  = msg.getNoteNumber();
            const double startT  = msg.getTimeStamp();
            const double endT    = ev->noteOffObject->message.getTimeStamp();

            if (pitch == kRangeShiftPitch)
            {
                if (collectSections) shiftMarkers.push_back ({ startT, endT - startT });
                continue;
            }
            if (pitch == kLyricShiftPitch) continue;   // as above
            if (pitch == kPhraseMarkerPitch)
            {
                if (phraseList != nullptr) phraseList->push_back ({ startT, endT });
                continue;
            }
            if (pitch == kOverdrivePitch)  // collected from HARM1/Solo only
            {
                if (overdriveList != nullptr) overdriveList->push_back ({ startT, endT });
                continue;
            }
            if (pitch >= kFirstReservedPitch) continue;

            juce::String lyric;
            double bestDist = 0.05;
            for (const auto& l : lyrics)
            {
                const double dist = std::abs (l.time - startT);
                if (dist < bestDist) { bestDist = dist; lyric = l.text; }
            }

            bool isTalkie = false, isHidden = false;
            classifyNotationSuffixes (lyric, isTalkie, isHidden);
            notes.push_back ({ startT, endT, pitch, lyric, isTalkie, isHidden, part, 1.0f });
            minP = std::min (minP, pitch);
            maxP = std::max (maxP, pitch);
        }
    };

    parseSeq (mainIdx, VocalPart::Main, true, &chartPhrases, &chartOverdrives);
    if (harmonies)
    {
        parseSeq (harm2Idx, VocalPart::Harm2, false, &chartHarm2Phrases, nullptr);
        parseSeq (harm3Idx, VocalPart::Harm3, false, nullptr, nullptr);
    }

    // Read [end] event from EVENTS track for total song duration
    if (eventsIdx >= 0)
    {
        const auto* seq = midi.getTrack (eventsIdx);
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& msg = seq->getEventPointer (i)->message;
            if (msg.isTextMetaEvent() && msg.getTextFromTextMetaEvent().trim() == "[end]")
            {
                songEndTime = msg.getTimeStamp();  // already in seconds (convertTimestampTicksToSeconds called)
                break;
            }
        }
    }
    finalizeChart (std::move (notes), std::move (shiftMarkers), minP, maxP);

    chartStatus = "Loaded " + juce::String ((int) chartNotes.size())
                + " notes from " + file.getFileName();
}
