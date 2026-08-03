#pragma once
#include <JuceHeader.h>
#include <array>
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
    //    the note list per frame (PERFORMANCE_IMPROVEMENT.md c1/c2). Appended at the
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
    std::atomic<double> currentPlaybackTime { 0.0 };  // fallback when API unavailable

    // One entry per audio-thread hop estimate (~every 16 ms). Queued so the UI
    // thread can see every estimate instead of point-sampling the latest atomics
    // once per frame — a hop-level detection that gets superseded (next hop, hold
    // decay, silence) before the next UI frame reads the atomics was previously
    // invisible to note-hit tracking and the take-export pitch trail, especially
    // for notes shorter than one UI frame interval (see _upcoming/SHORT_NOTE_HIT_TRACKING.md).
    struct HopSample { double time; float midiFloat; bool voiced; };
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
    std::atomic<int>    savedLatencyOffsetMs { 0 };
    std::atomic<float>  savedPixPerSec       { 200.0f };
    std::atomic<float>  savedLyricScale      { 1.0f };
    std::atomic<int>    savedSquashGuard     { 0 };   // 0=Off 1=Spacing 2=Auto-shrink 3=Hybrid
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

    float detectPitch                (const float*, int);
    float detectPitchAutocorrelation (const float*, int);
    float detectPitchYIN             (const float*, int);
    float detectPitchMPM             (const float*, int);

    // ── Pitch-detection front end ─────────────────────────────────────────────
    // Detection runs at a fixed ~24 kHz internal rate: processBlock conditions the
    // input (high-pass at the band floor against rumble/plosives, 6 kHz low-pass
    // anti-alias), keeps every decimFactor-th sample into a ring of the last
    // kPitchWindow samples, and analyses the ring every kPitchHop new samples —
    // overlapping windows, one estimate every ~11 ms (~94/s). All window/lag maths
    // uses detectRate, never the host rate, so the constants below hold at every host
    // rate by construction. kPitchWindow = 768 internal samples ≈ 32 ms at 24 kHz.
    //
    // The rate was 16 kHz until 2026-08-03. Measured at 24 kHz (sweeps in
    // tests/NsdfPeakTests.cpp, tabulated in _upcoming/PITCH_DETECTION_FINDINGS.md):
    // ~14% fewer dropouts on noisy input at equal accuracy, high-range (MIDI 72-84)
    // error 3.6 -> 1.5 cents, and — because the window is a fixed *sample count* — the
    // analysis window shortens 48 -> 32 ms, which is SHORT_NOTE_HIT_TRACKING §1's
    // short-note improvement arriving for free. 24 kHz is also the ceiling: integer
    // decimation from a 48 kHz host reaches only 48000/24000/16000, and a higher
    // internal rate would push kPitchWindow + maxLag past kFftSize.
    //
    // kMaxLagBuf must exceed the largest lag, detectRate / rbpitch::kMinFreqHz (60 Hz);
    // the worst case is a host rate just under 36 kHz (decimFactor 1 → lag ≈ 599),
    // hence 640. Note kPitchWindow ≥ 2 × maxLag does not hold there — the correlation
    // width only has to be a fraction of the lag range, not exceed it (measured down to
    // 0.4× with no loss, PITCH_DETECTION_FINDINGS.md §7), so
    // detectPitchAutocorrelation's guard was relaxed to match YIN's. Only hosts in the
    // 34-36 kHz band go below that measured 0.4 ratio, and none are in common use.
    static constexpr int kPitchWindow     = 768;
    static constexpr int kPitchHop        = 256;  // 3 hops per window
    static constexpr int kPitchHoldCycles = 24;   // ≈ 260 ms of hold at the ~11 ms hop cadence
    static constexpr int kMaxLagBuf       = 640;

    // Frames whose NSDF peak is below this are unvoiced (breath, consonants, noise):
    // they feed the hold decay instead of publishing a garbage pitch.
    static constexpr float kClarityThreshold = 0.80f;

    // MPM's NSDF autocorrelation term is computed as IFFT(|FFT(x)|²) — O(N log N)
    // instead of the direct O(N × maxLag) form. kFftSize must be ≥ kPitchWindow +
    // maxLag (≤ 1087) so the circular correlation is linear over the scanned lags.
    static constexpr int kFftOrder = 11;
    static constexpr int kFftSize  = 1 << kFftOrder;   // 2048

    // One transposed-direct-form-II biquad section. Coefficients are written by
    // setBiquadLP/HP in prepareToPlay only; process() runs on the audio thread.
    struct Biquad
    {
        float b0 {}, b1 {}, b2 {}, a1 {}, a2 {};
        float z1 {}, z2 {};
        float process (float x) noexcept
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0.0f; }
    };
    static void setBiquadLP (Biquad&, double sampleRate, double freq);
    static void setBiquadHP (Biquad&, double sampleRate, double freq);

    double detectRate     { 16000.0 };  // internal detection rate = host rate / decimFactor
    double hostSampleRate { 48000.0 };  // set in prepareToPlay; used for hop timestamping
                                         // (getSampleRate() isn't reliably set outside a real host)
    int    decimFactor { 3 };
    int    decimPhase  { 0 };
    Biquad hpFilter;                 // 70 Hz high-pass (rumble / plosive rejection)
    Biquad lpFilter1, lpFilter2;     // 2 × 6 kHz low-pass (decimation anti-alias)

    // Ring of the most recent kPitchWindow internal samples, linearised into
    // windowScratch for each detection so the detectors see contiguous memory.
    float pitchRing[kPitchWindow]     = {};
    float windowScratch[kPitchWindow] = {};
    int   ringPos         { 0 };   // next write index == oldest sample once filled
    int   ringFilled      { 0 };   // saturates at kPitchWindow
    int   hopCount        { 0 };   // internal samples since the last detection
    int   pitchHoldFrames { 0 };
    float lastDetectClarity { 0.0f };   // written by detectPitch, published by processBlock

    // The published pitch is the median of the last 3 accepted estimates once the
    // history is full — a single-frame octave/subharmonic error (note onsets, tone
    // transitions) never reaches the UI, at the cost of one hop ≈ 16 ms of latency.
    // Reset on every unvoiced frame so a fresh onset publishes immediately.
    float medianHist[3] = {};
    int   medianCount { 0 };

    // SPSC queue backing drainHopSamples() above — written here (audio thread),
    // drained on the message thread. Drop-newest on overflow: the fifo only fills
    // if the UI thread stalls for ~2s+, at which point a dropped sample is the
    // least of the problems.
    juce::AbstractFifo hopFifo { kHopQueueCapacity };
    std::array<HopSample, kHopQueueCapacity> hopBuffer;
    void pushHopSample (double time, float midiFloat, bool voiced) noexcept;

    // Constructed once with the processor (allocation happens off the audio thread);
    // fftScratch is the in-place real-FFT workspace (2 × kFftSize floats).
    juce::dsp::FFT pitchFft { kFftOrder };
    float fftScratch[2 * kFftSize] = {};

    std::vector<NoteInfo>    chartNotes;
    std::vector<PhraseInfo>  chartPhrases;       // HARM1 / Solo phrase grid (bottom strip + static)
    std::vector<PhraseInfo>  chartHarm2Phrases;  // HARM2 phrase grid (top strip static mode)
    std::vector<PhraseInfo>  chartOverdrives;          // overdrive (Star Power) ranges from HARM1 / Solo
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
