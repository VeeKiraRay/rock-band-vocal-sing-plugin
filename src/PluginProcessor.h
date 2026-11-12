#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include "reaper_api.h"
#include "NsdfPeak.h"       // kOctaveRivalRatio — default for the octave-threshold setting
#include "PitchDetector.h"  // rbpitch::Detector — the pitch-detection front end
#include "TakeExport.h"     // rbtake::SegmentationParams — defaults for the tuning below
#include "PercussionHits.h" // rbperc::Note — the chart's percussion notes

// Which vocal part a note belongs to. Main = Solo (PART VOCALS) or HARM1 (lead) —
// both render cyan with lyrics below the highway. Harm2/Harm3 render in their own
// colours with lyrics in the strip above the highway. Order doubles as draw priority.
enum class VocalPart { Main = 0, Harm2 = 1, Harm3 = 2 };

// Reserved marker pitches in Rock Band vocal charts. The parsers route these to
// sections/phrases/overdrives instead of chartNotes -- or, for kLyricShiftPitch, drop them
// outright, since nothing consumes lyric shift yet; it still needs a name and a routing
// slot, because charting it as a note dragged the highway floor to C-1. Excluding a marker
// here also keeps it out of chartMinPitch/chartMaxPitch, which is half the point. The
// editor's pitch-edit drag clamps so a note can never land on one.
constexpr int kRangeShiftPitch    = 0;    // Range Shift section boundary
constexpr int kLyricShiftPitch    = 1;    // RB3 lyric shift (which part's lyric the strip shows)
constexpr int kPhraseMarkerPitch  = 105;  // vocal phrase marker
constexpr int kOverdrivePitch     = 116;  // overdrive (Star Power) phrase marker
constexpr int kFirstReservedPitch = 96;   // >= this: reserved / non-singable
// Percussion cues (tambourine / cowbell / clap) the player hits by tapping the mic. Only
// the playable one is collected. The sample-only one triggers a sound in game and is never
// drawn there either, so it is dropped like any other reserved pitch; it is named so the
// pairing with kPercussionPitch is visible here rather than rediscovered.
constexpr int kPercussionPitch       = 96;  // playable: drawn and scored
constexpr int kPercussionSamplePitch = 97;  // non-playable: deliberately dropped

struct NoteInfo {
    double       startTime;  // seconds (absolute project time)
    double       endTime;
    int          pitch;      // MIDI 0-127
    juce::String lyric;      // may be empty
    bool         isTalkie { false };  // # or ^ in the lyric's suffix run — no pitch match required
    bool         isHidden { false };  // $ in the lyric's suffix run — skip in lyric strip (HARM duplicate)
    VocalPart    part      { VocalPart::Main };
    float        thickScale { 1.0f };  // 1.0 / 1.3 (Harm2 unison) / 1.6 (Harm3 unison) — overlap thickness

    // ── Source back-reference (REAPER only) — lets the editor write a pitch edit
    //    back to the exact MIDI note. All null/-1 for the file-based fallback, which
    //    marks the note non-editable. Survives the startTime sort since it travels
    //    with the struct; pitch-only edits don't reorder notes so the index stays valid
    //    within a session (a reload re-resolves it regardless).
    void*        sourceTake  { nullptr };  // MediaItem_Take*
    void*        sourceItem  { nullptr };  // MediaItem*
    void*        sourceTrack { nullptr };  // MediaTrack*
    int          sourceNoteIndex { -1 };

    // ── Render cache — content-derived, filled by finalizeChart(); the parsers leave
    //    these defaulted. Precomputed so paint() never allocates strings or rescans
    //    the note list per frame (2026-07 paint phase, c1/c2). Appended at the
    //    end of the struct so the parsers' aggregate push_back({...}) stays valid.
    juce::String displayLyric;            // lyric minus #/^/$ suffixes — pills + highway labels
    juce::String syllable;                // stripLyricSuffix text — static-mode word part
    juce::String glue;                    // stripLyricSuffix glue: "" / "-" / " "
    double       effectiveEnd { 0.0 };    // endTime extended through the following same-part
                                          // raw-"+" chain (a hidden "+$" breaks the chain,
                                          // matching the editor's original per-frame scan)
};

// Solo reads PART VOCALS; Harmonies combines HARM1/HARM2/HARM3 on one highway.
enum class VocalMode { Solo = 0, Harmonies = 1 };

struct PhraseInfo {
    double startTime;
    double endTime;
};

// A playable percussion note (kPercussionPitch). Defined in PercussionHits.h, JUCE-free, so
// the hit rules can be unit-tested on it directly; see that header for why only the start
// time carries timing.
using PercussionNote = rbperc::Note;

struct PitchSection {
    double startTime;
    double endTime;
    float  transitionDur;  // Range Shift marker note length — drives the entry animation
    int    minPitch;       // raw note minimum in this section (no padding)
    int    maxPitch;
};

class RBVocalSingAudioProcessor : public juce::AudioProcessor
{
public:
    RBVocalSingAudioProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {}

    ~RBVocalSingAudioProcessor() override
    {
        if (reaperHost != nullptr)
            reaperHost->release();   // balances the queryInterface add-ref (handover)
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    const juce::String getName() const override { return "RBVocalSing"; }
    bool acceptsMidi()  const override { return true; }
    bool producesMidi() const override { return false; }
    bool hasEditor()    const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }

    bool isBusesLayoutSupported (const BusesLayout&) const override { return true; }

    void getStateInformation (juce::MemoryBlock& destData)  override;
    void setStateInformation (const void* data, int size)   override;

    int  getNumPrograms()                                   override { return 1; }
    int  getCurrentProgram()                                override { return 0; }
    void setCurrentProgram (int)                            override {}
    const juce::String getProgramName (int)                 override { return {}; }
    void changeProgramName (int, const juce::String&)       override {}

    juce::AudioProcessorEditor* createEditor() override;

    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return &vst3Ext; }

    // ── Reaper API (loaded once by VST3 host handover) ──────────────────────
    ReaperApi reaperApi;
    // REAPER's host interface, retained (add-ref'd) at handover and released in the
    // destructor, so the plugin's own parent MediaTrack* can be queried on demand via
    // getReaperParent(1). Queried lazily at point of use: during instantiation the FX
    // may not yet be attached to its track, so a handover-time capture can come back
    // null (bit us 2026-07-20 — take export landed at the end of the project).
    // Message thread only.
    reaper::IReaperHostApplication* reaperHost { nullptr };

    // ── MIDI chart -- UI thread only ─────────────────────────────────────────
    // readChartFromReaper() — call from message thread when reaperApi.isLoaded()
    void readChartFromReaper();
    // loadMidiFile() — fallback when running outside Reaper
    void loadMidiFile (const juce::File& file);
    // reloadChart() — re-read the current source for the current vocalMode (Reaper
    // if connected, else the last loaded .mid). Call after changing vocalMode.
    void reloadChart();

    const std::vector<NoteInfo>&    getChartNotes()        const { return chartNotes; }
    // Single-element mutable access for live drag-edit preview (message thread only).
    // Returns nullptr if out of range. Intentionally never exposes the whole vector so
    // its size/sort-by-startTime invariant can't be broken.
    NoteInfo* mutableChartNoteForEdit (int idx)
    {
        return (idx >= 0 && idx < (int) chartNotes.size()) ? &chartNotes[(size_t) idx] : nullptr;
    }
    // Bumped on every chart clear/load — editor-side derived caches (e.g. lyric pixel
    // widths, which need fonts and so can't live here) key off it for invalidation.
    int getChartVersion() const { return chartVersion; }
    // Sorted, unique chart pitches grouped by pitch class — the octave-wrap candidate
    // pool, built by finalizeChart so the editor's per-frame wrap is a binary-searched
    // slice instead of a full note scan + sort per call.
    const std::array<std::vector<int>, 12>& getPitchClassCandidates() const { return pitchClassCandidates; }
    const std::vector<PhraseInfo>&  getChartPhrases()      const { return chartPhrases; }
    const std::vector<PhraseInfo>&  getChartHarm2Phrases() const { return chartHarm2Phrases; }
    const std::vector<PhraseInfo>&  getChartOverdrives()        const { return chartOverdrives; }
    // Sorted by startTime. From PART VOCALS (Solo) or HARM1 (Harmonies) only.
    const std::vector<PercussionNote>& getChartPercussion()     const { return chartPercussion; }
    const std::vector<double>&      getChartPhraseSeparators()  const { return chartPhraseSeparators; }
    const std::vector<PitchSection>& getChartPitchSections() const { return chartPitchSections; }
    juce::String chartStatus;
    double songEndTime  { -1.0 };  // seconds; from EVENTS [end] event or last note fallback
    int chartMinPitch { 48 };
    int chartMaxPitch { 84 };

    // ── Audio thread -> UI thread ─────────────────────────────────────────────
    std::atomic<float>  currentFrequency    { 0.0f };
    std::atomic<int>    currentMidiNote     { -1 };
    // Fractional MIDI pitch (cents precision) — same estimate as currentMidiNote but
    // stored before rounding; -1 when undetected. Feeds cents scoring, the smooth
    // arrowhead, snap assist and the pitch trail.
    std::atomic<float>  currentMidiFloat    { -1.0f };
    // Voicing confidence of the last analysis (MPM's chosen NSDF peak height, 0–1;
    // 0 while unvoiced or held). Frames below kClarityThreshold are treated as
    // unvoiced; the UI may also use this (e.g. dim the arrowhead when uncertain).
    std::atomic<float>  currentClarity      { 0.0f };
    // Octave-rival strength of the last analysis (0-1): how tall the NSDF peak near half
    // the chosen lag was, relative to the chosen peak — i.e. the evidence that this
    // estimate is an octave too LOW. 0 when the octave guard is off, when no rival peak
    // was found, or while unvoiced. Deliberately NOT the same axis as currentClarity: an
    // octave-down pick usually has *high* clarity, because 2x the true period genuinely
    // correlates well. Doubt is `>= rbpitch::kOctaveRivalRatio`; the sub-threshold values
    // are kept so the UI can show how close the call was.
    std::atomic<float>  currentOctaveRatio  { 0.0f };
    // The *decision* the ratio above is only the evidence for: true once a rival has
    // persisted for kOctaveRunHops consecutive voiced hops. Separate from the ratio
    // because persistence is temporal state — a consumer holding one sample cannot
    // reconstruct it, and having the display and the correction each re-derive it would
    // let them drift apart.
    std::atomic<bool>   currentOctaveDoubt  { false };
    std::atomic<double> currentPlaybackTime { 0.0 };  // fallback when API unavailable
    // Samples in the most recent processBlock call. Quoted by the editor's large-buffer
    // hint as the number to change; never used to *decide* the hint, which measures how
    // often the position actually moves (see rbperf::positionLimited).
    std::atomic<int>    lastBlockSize       { 0 };

    // One entry per audio-thread hop estimate (~every 16 ms). Queued so the UI
    // thread can see every estimate instead of point-sampling the latest atomics
    // once per frame — a hop-level detection that gets superseded (next hop, hold
    // decay, silence) before the next UI frame reads the atomics was previously
    // invisible to note-hit tracking and the take-export pitch trail, especially
    // for notes shorter than one UI frame interval (the short-note hit-tracking work).
    // `voiced` is the RMS gate, not "pitch found" - which is what lets an unpitched mic
    // tap score a percussion note.
    struct HopSample { double time; float midiFloat; bool voiced;
                       float octaveRatio; bool octaveDoubt; };
    static constexpr int kHopQueueCapacity = 128;  // ~2s of hops @ ~16ms — UI-stall headroom

    // Message thread only. Drains up to maxCount queued hop samples (oldest first)
    // into dest; returns the count actually read.
    int drainHopSamples (HopSample* dest, int maxCount) noexcept;

    // ── Audio-thread cost diagnostics (RB_PITCH_PROFILE builds) ───────────────
    // Written on the audio thread, read by drawStatusBar. Members exist under both
    // flag values so the class layout never depends on the macro (same convention as
    // the editor's paint EMAs); only the writes and the readout are compiled out.
    //
    // profDetectMs   — EMA of one detectPitch() call in ms. The number that moves if
    //                  the FFT size, window or internal rate change.
    // profBlockLoad  — EMA of processBlock's duration as a fraction of its deadline
    //                  (numSamples / hostSampleRate). This is the audio-thread load:
    //                  it includes the per-host-sample front-end filters, which no
    //                  decimation change affects.
    // profDetectCount— monotonic detection counter; the editor differentiates it to
    //                  show the achieved hop rate (expected ~62/s today). Confirms the
    //                  cadence half of any window/hop retune actually landed.
    std::atomic<double>   profDetectMs    { 0.0 };
    std::atomic<double>   profBlockLoad   { 0.0 };
    std::atomic<uint32_t> profDetectCount { 0 };

    // ── UI thread -> audio thread ─────────────────────────────────────────────
    std::atomic<float> gateThreshold  { 0.01f };
    std::atomic<bool>  audioAboveGate { false };
    std::atomic<int>   pitchMethod   { 2 };   // 0=Autocorr  1=YIN  2=MPM (production default)

    // Octave guard: 0=Off  1=Warn  2=Warn+Correct. Off skips the rival search entirely,
    // so the detector is bit-for-bit what it was before the guard existed. Warn measures
    // and publishes but never changes the pitch; Correct additionally re-picks the rival
    // lag, which is why it has to happen here rather than in the editor — the corrected
    // value must reach the median filter, the hop queue, scoring and the take export
    // together. MPM only: the fallback detectors have no rival measure.
    //
    // Serialised directly rather than through a saved* mirror (the gateThreshold /
    // vocalMode pattern) — the audio thread reads this one, so a second copy would only
    // be something to keep in sync. Absent from a pre-guard session, which loads as 0.
    std::atomic<int>   octaveGuard   { 0 };

    // How strong the rival must be, as a fraction of the reported peak, before the guard
    // marks it. User-tunable because the right value is a property of the material, not of
    // the algorithm: it trades missed errors against false alarms, and a stem's artifacts
    // decide where that balance sits. Clamped to kOctaveThresholdMin..Max — the ceiling is
    // the key threshold (see rbpitch::kOctaveRivalRatio), above which a real octave error
    // cannot score at all, and below the floor a rival is too weak to mean anything.
    std::atomic<float> octaveThreshold { rbpitch::kOctaveRivalRatio };

    static constexpr float kOctaveThresholdMin = 0.60f;
    static constexpr float kOctaveThresholdMax = 0.90f;

    // ── Advanced tuning (expert; off by default) ──────────────────────────────
    // Loosens detection and segmentation for rough material — AI-separated stems, where
    // letting a little too much through beats missing notes.
    //
    // Everything here is gated behind one master switch, deliberately. Relying on the
    // values simply defaulting to the live constants would make safety a *property* of the
    // defaults rather than a structural guarantee: one future knob added with a slightly
    // wrong default would silently change detection for every existing install. With the
    // switch off, none of these values is read at all.
    //
    // Note this is NOT keyed on "Live-mic used". That setting answers a question about the
    // rig (is the incoming audio a microphone?) and gates the calibration offset; wiring
    // detection sensitivity to it would mean flipping a *timing* control silently retuned
    // the detector. Enabling expert tuning is its own, explicit decision.
    //
    // The Settings "Mic gate" is untouched by all this and keeps applying to every source:
    // it already exists, already works, and a second gate here would be two controls for
    // one value.
    std::atomic<bool>   advTuningEnabled    { false };
    std::atomic<float>  advClarityThreshold { rbpitch::kClarityThreshold };
    std::atomic<double> advMinNoteLen       { rbtake::SegmentationParams{}.minNoteLen };
    std::atomic<double> advMaxGapMerge      { rbtake::SegmentationParams{}.maxGapMerge };
    std::atomic<double> advSplitHysteresis  { rbtake::SegmentationParams{}.splitHysteresis };

    // Naming: the group is unprefixed rather than saved* because two of the four values
    // are read on the audio thread (the switch and the clarity gate, through
    // detectorParams()). The segmentation three are message-thread only but are named with
    // them, since they are one panel and are restored as a unit.

    // Resets the tuning *values* to the shipped constants. Deliberately leaves
    // advTuningEnabled alone: the button restores what the knobs say, and silently
    // switching the panel off would be a surprise rather than a restore.
    void restoreAdvancedDefaults();

    // The one place the tuning decision turns into numbers, so processBlock, the cursor
    // readout, the batch sweep and the take export cannot drift apart.
    rbpitch::DetectorParams    detectorParams()     const;
    rbtake::SegmentationParams segmentationParams() const;

    // ── Vocal mode (Solo / Harmonies) — set by editor, drives chart re-read ────
    std::atomic<int> vocalMode { (int) VocalMode::Solo };

    // ── Persisted UI settings ─────────────────────────────────────────────────
    // Written by the editor on the message thread, but read/written by
    // get/setStateInformation from whatever thread the host calls them on (the VST3
    // spec doesn't promise the message thread) — hence atomics, like vocalMode above.
    std::atomic<double> savedFrameInterval   { 1.0 / 60.0 };
    std::atomic<bool>   savedReaperPriority  { false };
    std::atomic<bool>   savedOctaveWrap      { true };
    std::atomic<int>    savedPitchTolCents   { 50 };    // GREAT window ± cents; CLOSE = 2×
    std::atomic<bool>   savedPitchTrail      { true };  // sung-pitch contour on the highway
    std::atomic<int>    savedLatencyOffsetMs { 0 };     // output calibration: chart shift
    std::atomic<int>    savedInputOffsetMs   { 0 };     // input calibration: detection shift.
                                                        // Defaults to 0 so a session saved
                                                        // before the split loads unchanged.
    std::atomic<float>  savedPixPerSec       { 200.0f };
    std::atomic<float>  savedLyricScale      { 1.0f };
    std::atomic<int>    savedSquashGuard     { 0 };   // 0=Off 1=Spacing 2=Auto-shrink 3=Hybrid
    // Highway note markings: 0=Off 1=Lyric 2=Pitch 3=Both. Defaults to Both, unlike its
    // neighbours here — the pitch is the reason the setting exists, so a session saved
    // before it existed opens showing it rather than hiding it behind a menu.
    std::atomic<int>    savedNoteMarkings    { 3 };
    std::atomic<int>    savedLyricMode       { 0 };   // 0=Scrolling, 1=Static
    std::atomic<int>    savedRenderer        { 0 };   // 0=CPU, 2=OpenGL+persist
    std::atomic<bool>   savedLiveMicInput    { false }; // does the *output* offset apply to
                                                        // detected pitch (live mic) as well
                                                        // as to the chart (always)? Purely
                                                        // a calibration question - it does
                                                        // not gate the tuning above.
    std::atomic<bool>   savedStemMode        { false };  // offline stem tools on the
                                                         // editor (cursor readout)

private:
    // VST3ClientExtensions — Reaper calls setIHostApplication to hand over its API
    struct Vst3Ext final : public juce::VST3ClientExtensions
    {
        explicit Vst3Ext (RBVocalSingAudioProcessor& p) : proc (p) {}
        void setIHostApplication (Steinberg::FUnknown* ptr) override;
        RBVocalSingAudioProcessor& proc;
    } vst3Ext { *this };

    // ── Pitch detection ───────────────────────────────────────────────────────
    // The whole front end — conditioning, decimation, the analysis ring, the detectors,
    // the median filter and the hold decay — lives in rbpitch::Detector (PitchDetector.h).
    // It is instance-owned so the offline stem analyzer can run a second one over project
    // audio without racing this one or sharing its median history, hold counter and
    // octave-guard run counter (planned stem-analyzer work, P1).
    //
    // What stays here is publishing: reading the settings atomics into DetectorParams once
    // per block, then storing each hop's result into the atomics above and the hop queue
    // below with a project timestamp attached.
    rbpitch::Detector detector;

    double hostSampleRate { 48000.0 };  // set in prepareToPlay; used for hop timestamping
                                        // (getSampleRate() isn't reliably set outside a
                                        // real host)

    // SPSC queue backing drainHopSamples() above — written here (audio thread),
    // drained on the message thread. Drop-newest on overflow: the fifo only fills
    // if the UI thread stalls for ~2s+, at which point a dropped sample is the
    // least of the problems.
    juce::AbstractFifo hopFifo { kHopQueueCapacity };
    std::array<HopSample, kHopQueueCapacity> hopBuffer;
    void pushHopSample (double time, float midiFloat, bool voiced,
                        float octaveRatio, bool octaveDoubt) noexcept;

    std::vector<NoteInfo>    chartNotes;
    std::vector<PhraseInfo>  chartPhrases;       // HARM1 / Solo phrase grid (bottom strip + static)
    std::vector<PhraseInfo>  chartHarm2Phrases;  // HARM2 phrase grid (top strip static mode)
    std::vector<PhraseInfo>  chartOverdrives;          // overdrive (Star Power) ranges from HARM1 / Solo
    std::vector<PercussionNote> chartPercussion;       // playable percussion from HARM1 / Solo
    std::vector<double>      chartPhraseSeparators;    // precomputed separator line times (seconds)
    std::vector<PitchSection> chartPitchSections;
    int                      chartVersion { 0 };       // see getChartVersion()
    std::array<std::vector<int>, 12> pitchClassCandidates;  // see getPitchClassCandidates()

    juce::File lastMidiFile;  // remembered for reloadChart() outside Reaper

    void parseChartNotes (void* midiTake,
                          void*                                       srcItem,       // MediaItem* (nullable: file path)
                          void*                                       srcTrack,      // MediaTrack* (nullable: file path)
                          VocalPart                                   part,
                          bool                                        collectSections,
                          std::vector<NoteInfo>&                      out,
                          std::vector<PhraseInfo>*                    outPhrases,    // nullable
                          std::vector<PhraseInfo>*                    outOverdrives, // nullable
                          std::vector<PercussionNote>*                outPercussion, // nullable
                          std::vector<std::pair<double,double>>&      outShiftMarkers,
                          int& minP, int& maxP);

    // Shared bookends of both chart loaders (2026-07 code review, 3.2). clearChart() resets
    // all chart state; finalizeChart() is everything after "find source + parse":
    // sort → harmony post-passes → pitch padding → sections/separators → songEndTime
    // fallback. Each loader is then just: clearChart → find + parse → apply any [end]
    // event to songEndTime → finalizeChart → status string.
    void clearChart();
    void finalizeChart (std::vector<NoteInfo>&&                 notes,
                        std::vector<std::pair<double,double>>&& shiftMarkers,
                        int minP, int maxP);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RBVocalSingAudioProcessor)
};
