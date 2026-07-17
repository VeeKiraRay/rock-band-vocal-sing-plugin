#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <vector>
#include "reaper_api.h"

// Which vocal part a note belongs to. Main = Solo (PART VOCALS) or HARM1 (lead) —
// both render cyan with lyrics below the highway. Harm2/Harm3 render in their own
// colours with lyrics in the strip above the highway. Order doubles as draw priority.
enum class VocalPart { Main = 0, Harm2 = 1, Harm3 = 2 };

// Reserved marker pitches in Rock Band vocal charts. The parsers route these to
// sections/phrases/overdrives instead of chartNotes; the editor's pitch-edit drag
// clamps so a note can never land on one.
constexpr int kRangeShiftPitch    = 0;    // Range Shift section boundary
constexpr int kPhraseMarkerPitch  = 105;  // vocal phrase marker
constexpr int kOverdrivePitch     = 116;  // overdrive (Star Power) phrase marker
constexpr int kFirstReservedPitch = 96;   // >= this: reserved / non-singable

struct NoteInfo {
    double       startTime;  // seconds (absolute project time)
    double       endTime;
    int          pitch;      // MIDI 0-127
    juce::String lyric;      // may be empty
    bool         isTalkie { false };  // lyric ends with # or ^ — no pitch match required
    bool         isHidden { false };  // lyric ends with $ — skip in lyric strip (HARM duplicate)
    VocalPart    part      { VocalPart::Main };
    float        thickScale { 1.0f };  // 1.0 / 1.1 / 1.2 — precomputed overlap thickness

    // ── Source back-reference (REAPER only) — lets the editor write a pitch edit
    //    back to the exact MIDI note. All null/-1 for the file-based fallback, which
    //    marks the note non-editable. Survives the startTime sort since it travels
    //    with the struct; pitch-only edits don't reorder notes so the index stays valid
    //    within a session (a reload re-resolves it regardless).
    void*        sourceTake  { nullptr };  // MediaItem_Take*
    void*        sourceItem  { nullptr };  // MediaItem*
    void*        sourceTrack { nullptr };  // MediaTrack*
    int          sourceNoteIndex { -1 };
};

// Solo reads PART VOCALS; Harmonies combines HARM1/HARM2/HARM3 on one highway.
enum class VocalMode { Solo = 0, Harmonies = 1 };

struct PhraseInfo {
    double startTime;
    double endTime;
};

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
    const std::vector<PhraseInfo>&  getChartPhrases()      const { return chartPhrases; }
    const std::vector<PhraseInfo>&  getChartHarm2Phrases() const { return chartHarm2Phrases; }
    const std::vector<PhraseInfo>&  getChartOverdrives()        const { return chartOverdrives; }
    const std::vector<double>&      getChartPhraseSeparators()  const { return chartPhraseSeparators; }
    const std::vector<PitchSection>& getChartPitchSections() const { return chartPitchSections; }
    juce::String chartStatus;
    double songEndTime  { -1.0 };  // seconds; from EVENTS [end] event or last note fallback
    int chartMinPitch { 48 };
    int chartMaxPitch { 84 };

    // ── Audio thread -> UI thread ─────────────────────────────────────────────
    std::atomic<float>  currentFrequency    { 0.0f };
    std::atomic<int>    currentMidiNote     { -1 };
    std::atomic<double> currentPlaybackTime { 0.0 };  // fallback when API unavailable

    // ── UI thread -> audio thread ─────────────────────────────────────────────
    std::atomic<float> gateThreshold  { 0.01f };
    std::atomic<bool>  audioAboveGate { false };
    std::atomic<int>   pitchMethod   { 0 };   // 0=Autocorr  1=YIN  2=MPM

    // ── Vocal mode (Solo / Harmonies) — set by editor, drives chart re-read ────
    std::atomic<int> vocalMode { (int) VocalMode::Solo };

    // ── Persisted UI settings ─────────────────────────────────────────────────
    // Written by the editor on the message thread, but read/written by
    // get/setStateInformation from whatever thread the host calls them on (the VST3
    // spec doesn't promise the message thread) — hence atomics, like vocalMode above.
    std::atomic<double> savedFrameInterval   { 1.0 / 60.0 };
    std::atomic<bool>   savedReaperPriority  { false };
    std::atomic<bool>   savedOctaveWrap      { true };
    std::atomic<int>    savedLatencyOffsetMs { 0 };
    std::atomic<float>  savedPixPerSec       { 200.0f };
    std::atomic<int>    savedLyricMode       { 0 };   // 0=Scrolling, 1=Static
    std::atomic<int>    savedRenderer        { 0 };   // 0=CPU, 2=OpenGL+persist

private:
    // VST3ClientExtensions — Reaper calls setIHostApplication to hand over its API
    struct Vst3Ext final : public juce::VST3ClientExtensions
    {
        explicit Vst3Ext (RBVocalSingAudioProcessor& p) : proc (p) {}
        void setIHostApplication (Steinberg::FUnknown* ptr) override;
        RBVocalSingAudioProcessor& proc;
    } vst3Ext { *this };

    double storedSampleRate { 44100.0 };

    float detectPitch                (const float*, int);
    float detectPitchAutocorrelation (const float*, int);
    float detectPitchYIN             (const float*, int);
    float detectPitchMPM             (const float*, int);

    // kPitchBufSize is the analysis window at the ≤48 kHz baseline (2048 ≈ 42.7 ms).
    // prepareToPlay scales the effective window (pitchBufTarget) with the sample rate so
    // its real-time span — and the detectors' 80 Hz floor — survive high rates, capped
    // at kPitchBufCap (= the window at 192 kHz). kMaxLagBuf must exceed the largest lag,
    // 192000 / 80 = 2400, indexed inclusively. (CODE_REVIEW.md 1.2)
    static constexpr int kPitchBufSize    = 2048;
    static constexpr int kPitchBufCap     = 8192;
    static constexpr int kPitchHoldCycles = 6;
    static constexpr int kMaxLagBuf       = 2500;

    float pitchAccum[kPitchBufCap] = {};
    int   pitchAccumFill  { 0 };
    int   pitchHoldFrames { 0 };
    int   pitchBufTarget  { kPitchBufSize };  // samples per detection window at storedSampleRate

    std::vector<NoteInfo>    chartNotes;
    std::vector<PhraseInfo>  chartPhrases;       // HARM1 / Solo phrase grid (bottom strip + static)
    std::vector<PhraseInfo>  chartHarm2Phrases;  // HARM2 phrase grid (top strip static mode)
    std::vector<PhraseInfo>  chartOverdrives;          // overdrive (Star Power) ranges from HARM1 / Solo
    std::vector<double>      chartPhraseSeparators;    // precomputed separator line times (seconds)
    std::vector<PitchSection> chartPitchSections;

    juce::File lastMidiFile;  // remembered for reloadChart() outside Reaper

    void parseChartNotes (void* midiTake,
                          void*                                       srcItem,       // MediaItem* (nullable: file path)
                          void*                                       srcTrack,      // MediaTrack* (nullable: file path)
                          VocalPart                                   part,
                          bool                                        collectSections,
                          std::vector<NoteInfo>&                      out,
                          std::vector<PhraseInfo>*                    outPhrases,    // nullable
                          std::vector<PhraseInfo>*                    outOverdrives, // nullable
                          std::vector<std::pair<double,double>>&      outShiftMarkers,
                          int& minP, int& maxP);

    // Shared bookends of both chart loaders (CODE_REVIEW.md 3.2). clearChart() resets
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
