#pragma once

// Reaper VST3 API access.
//
// Provides IReaperHostApplication (from JUCE-bundled VST3 SDK headers)
// and a typed struct of the Reaper API functions we need.
//
// Usage:
//   1. In your AudioProcessor, override getVST3ClientExtensions() and
//      call reaperApi.load(app) inside setIHostApplication.
//   2. In exactly ONE .cpp file add:
//        namespace reaper { using namespace Steinberg; DEF_CLASS_IID(IReaperHostApplication) }
//   3. Call reaperApi.isLoaded() before using any function pointer.

// JuceHeader for the JUCE_*_IGNORE_WARNINGS macros below and for the VST3 SDK headers
// JUCE bundles. Included here rather than assumed: until StemAudioReader.h this file
// was only ever reached through PluginProcessor.h, which includes JUCE first, so the
// dependency was real but invisible.
#include <JuceHeader.h>

JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4100)
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wzero-as-null-pointer-constant", "-Wnon-virtual-dtor")

// CStringA is typedef'd to const char* in the VST3 SDK's ftypes.h — no ATL needed.
#include <pluginterfaces/base/ftypes.h>
#include <pluginterfaces/base/funknown.h>

namespace reaper {
    using namespace Steinberg;
    using INT_PTR = pointer_sized_int;
    using uint32  = Steinberg::uint32;
    #include "extern/reaper_vst3_interfaces.h"
    // DEF_CLASS_IID(IReaperHostApplication) must appear in exactly one .cpp
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE
JUCE_END_IGNORE_WARNINGS_MSVC

// ─── Typed function pointers ──────────────────────────────────────────────────
// All void* args correspond to MediaTrack*, MediaItem*, MediaItem_Take*, or
// ReaProject* — using void* avoids needing reaper_plugin.h.

struct ReaperApi
{
    int    (*CountTracks)                 (void* proj)                           = nullptr;
    void*  (*GetTrack)                    (void* proj, int idx)                  = nullptr;
    bool   (*GetTrackName)                (void* track, char* buf, int bufsz)    = nullptr;

    int    (*CountTrackMediaItems)        (void* track)                          = nullptr;
    void*  (*GetTrackMediaItem)           (void* track, int idx)                 = nullptr;
    void*  (*GetActiveTake)              (void* item)                            = nullptr;
    bool   (*TakeIsMIDI)                 (void* take)                            = nullptr;

    void   (*MIDI_CountEvts)             (void* take,
                                          int* notecnt, int* cccnt, int* txtsyxcnt) = nullptr;
    bool   (*MIDI_GetNote)               (void* take, int idx,
                                          bool* sel, bool* mute,
                                          double* sppq, double* eppq,
                                          int* chan, int* pitch, int* vel)           = nullptr;
    bool   (*MIDI_GetTextSysexEvt)       (void* take, int idx,
                                          bool* sel, bool* mute,
                                          double* ppq, int* type,
                                          char* msg, int* msglen)                    = nullptr;
    double (*MIDI_GetProjTimeFromPPQPos) (void* take, double ppqpos)                 = nullptr;

    double (*GetPlayPosition)            ()                                       = nullptr;
    double (*GetCursorPosition)          ()                                       = nullptr;
    int    (*GetPlayState)               ()                                       = nullptr;
    int    (*GetProjectStateChangeCount) (void* proj)                             = nullptr;

    void   (*SetEditCurPos)           (double time, bool moveview, bool seekplay) = nullptr;
    double (*CSurf_OnPlayRateChange) (double playrate)                            = nullptr;
    double (*Master_GetPlayRate)     (void* proj)                                 = nullptr;

    // ── Write side (note editing) — pointer params; NULL leaves a field unchanged ──
    bool   (*MIDI_SetNote)           (void* take, int idx,
                                      bool* sel, bool* muted,
                                      double* startppq, double* endppq,
                                      int* chan, int* pitch, int* vel,
                                      bool* noSort)                               = nullptr;
    void   (*Undo_BeginBlock2)       (void* proj)                                 = nullptr;
    void   (*Undo_EndBlock2)         (void* proj, const char* desc, int flags)    = nullptr;
    void   (*MarkTrackItemsDirty)    (void* track, void* item)                    = nullptr;

    // ── Write side (take export) — const bool* args are REAPER "Optional" params;
    // NULL selects the default. Signatures transcribed from the REAPER API docs —
    // they must match exactly (the void* function-pointer cast can't catch a drift).
    void   (*InsertTrackAtIndex)          (int idx, bool wantDefaults)            = nullptr;
    bool   (*GetSetMediaTrackInfo_String) (void* track, const char* parmname,
                                           char* stringNeedBig, bool setNewValue) = nullptr;
    double (*GetMediaTrackInfo_Value)     (void* track, const char* parmname)     = nullptr;
    void*  (*CreateNewMIDIItemInProj)     (void* track, double starttime, double endtime,
                                           const bool* qnInOptional)              = nullptr;
    bool   (*MIDI_InsertNote)             (void* take, bool selected, bool muted,
                                           double startppqpos, double endppqpos,
                                           int chan, int pitch, int vel,
                                           const bool* noSortInOptional)          = nullptr;
    void   (*MIDI_Sort)                   (void* take)                            = nullptr;
    double (*MIDI_GetPPQPosFromProjTime)  (void* take, double projtime)           = nullptr;
    // Arrange-view refresh cosmetics — not gated by canExportTake(); null-guard at
    // the call site.
    void   (*UpdateArrange)               ()                                      = nullptr;
    void   (*TrackList_AdjustWindows)     (bool isMinor)                          = nullptr;

    // ── Offline audio read (stem analyzer) ────────────────────────────────────
    // Reads a track's media-item audio at any project time with no transport running.
    // Message thread only, like the rest of this struct.
    //
    // Two properties that decide how a reading should be read: a *track* accessor sees
    // the media items only, so it is PRE track FX, and it is indexed in project time, so
    // the play rate cannot affect it.
    //
    // GetAudioAccessorSamples writes **doubles**, interleaved across numchannels, and
    // returns 0 when the requested range holds no audio at all. As with the take-export
    // pointers above, these signatures are transcribed from the REAPER API and must match
    // exactly: a wrong name resolves to nullptr and is caught by canAnalyzeOffline(), but
    // a wrong *signature* is not caught by anything.
    void*  (*CreateTrackAudioAccessor)   (void* track)                           = nullptr;
    void   (*DestroyAudioAccessor)       (void* accessor)                        = nullptr;
    int    (*GetAudioAccessorSamples)    (void* accessor, int samplerate, int numchannels,
                                          double starttime_sec, int numsamplesperchannel,
                                          double* samplebuffer)                  = nullptr;
    double (*GetAudioAccessorStartTime)  (void* accessor)                        = nullptr;
    double (*GetAudioAccessorEndTime)    (void* accessor)                        = nullptr;
    bool   (*AudioAccessorValidateState) (void* accessor)                        = nullptr;
    // Forces a reload from the underlying media. Not gated by canAnalyzeOffline() —
    // null-guard at the call site, like UpdateArrange.
    void   (*AudioAccessorUpdate)        (void* accessor)                        = nullptr;

    bool isLoaded() const { return CountTracks != nullptr; }

    // True when every write-side pointer resolved (older REAPER → read-only).
    bool canEdit() const
    {
        return MIDI_SetNote != nullptr && Undo_BeginBlock2 != nullptr
            && Undo_EndBlock2 != nullptr && MarkTrackItemsDirty != nullptr;
    }

    // True when the take-export write path fully resolved (canEdit covers the undo
    // block + MarkTrackItemsDirty it shares with note editing).
    bool canExportTake() const
    {
        return canEdit()
            && InsertTrackAtIndex != nullptr && GetSetMediaTrackInfo_String != nullptr
            && GetMediaTrackInfo_Value != nullptr && CreateNewMIDIItemInProj != nullptr
            && MIDI_InsertNote != nullptr && MIDI_Sort != nullptr
            && MIDI_GetPPQPosFromProjTime != nullptr;
    }

    // True when the offline audio-read set fully resolved. Independent of canEdit():
    // reading a stem needs no write access at all. The caller must additionally have a
    // track to read, which on the plugin path means a non-null retained host interface.
    bool canAnalyzeOffline() const
    {
        return CreateTrackAudioAccessor   != nullptr && DestroyAudioAccessor      != nullptr
            && GetAudioAccessorSamples    != nullptr && GetAudioAccessorStartTime != nullptr
            && GetAudioAccessorEndTime    != nullptr && AudioAccessorValidateState != nullptr;
    }

    void load (reaper::IReaperHostApplication* app)
    {
        auto get = [app] (const char* n) { return app->getReaperApi (n); };
        #define RLOAD(fn) fn = (decltype(fn)) get (#fn)
        RLOAD (CountTracks);
        RLOAD (GetTrack);
        RLOAD (GetTrackName);
        RLOAD (CountTrackMediaItems);
        RLOAD (GetTrackMediaItem);
        RLOAD (GetActiveTake);
        RLOAD (TakeIsMIDI);
        RLOAD (MIDI_CountEvts);
        RLOAD (MIDI_GetNote);
        RLOAD (MIDI_GetTextSysexEvt);
        RLOAD (MIDI_GetProjTimeFromPPQPos);
        RLOAD (GetPlayPosition);
        RLOAD (GetCursorPosition);
        RLOAD (GetPlayState);
        RLOAD (GetProjectStateChangeCount);
        RLOAD (SetEditCurPos);
        RLOAD (CSurf_OnPlayRateChange);
        RLOAD (Master_GetPlayRate);
        RLOAD (MIDI_SetNote);
        RLOAD (Undo_BeginBlock2);
        RLOAD (Undo_EndBlock2);
        RLOAD (MarkTrackItemsDirty);
        RLOAD (InsertTrackAtIndex);
        RLOAD (GetSetMediaTrackInfo_String);
        RLOAD (GetMediaTrackInfo_Value);
        RLOAD (CreateNewMIDIItemInProj);
        RLOAD (MIDI_InsertNote);
        RLOAD (MIDI_Sort);
        RLOAD (MIDI_GetPPQPosFromProjTime);
        RLOAD (UpdateArrange);
        RLOAD (TrackList_AdjustWindows);
        RLOAD (CreateTrackAudioAccessor);
        RLOAD (DestroyAudioAccessor);
        RLOAD (GetAudioAccessorSamples);
        RLOAD (GetAudioAccessorStartTime);
        RLOAD (GetAudioAccessorEndTime);
        RLOAD (AudioAccessorValidateState);
        RLOAD (AudioAccessorUpdate);
        #undef RLOAD
    }
};
