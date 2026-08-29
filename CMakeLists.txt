cmake_minimum_required(VERSION 3.22)
project(RBVocalSing VERSION 0.1.0)

include(cmake/CPM.cmake)

CPMAddPackage(
    NAME JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 7.0.12
)

option(RBVS_BUILD_TESTS "Build unit tests" ON)

# Diagnostics build: appends per-section paint timings (hw/ly/st/bar ms EMAs) to the
# status bar. Toggle with -DRB_PAINT_PROFILE=ON at configure time; default OFF so
# release builds are unaffected. The macro is always defined (0/1) so `#if` never
# hits an undefined identifier and both targets stay in sync.
option(RB_PAINT_PROFILE "Show per-section paint timings in the status bar" OFF)

# Audio-thread diagnostics: appends detector cost (ms per detectPitch call), audio-thread
# load (processBlock as a fraction of its deadline) and the achieved hop rate to the status
# bar. Separate from RB_PAINT_PROFILE so the detector can be measured without paint
# instrumentation perturbing the same status bar. Same 0/1 always-defined contract.
# Measure in RELEASE ONLY - Debug's unoptimised FFT makes the numbers meaningless
# (see the paint-profiling lesson in CLAUDE.md).
option(RB_PITCH_PROFILE "Show pitch-detector timings in the status bar" OFF)

if (RBVS_BUILD_TESTS)
    CPMAddPackage(
        NAME Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.4
    )
endif()

juce_add_plugin(RBVocalSing
    PLUGIN_MANUFACTURER_CODE Vkry
    COMPANY_NAME "VKR"
    PLUGIN_CODE RBVs
    FORMATS VST3
    PRODUCT_NAME "Rock Band Vocal Sing - Reaper"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(RBVocalSing)

target_sources(RBVocalSing
    PRIVATE
        src/PluginProcessor.cpp
        src/PitchDetection.cpp
        src/NsdfPeak.cpp
        src/ChartLoader.cpp
        src/TakeExport.cpp
        src/StemAudioReader.cpp
        src/PluginEditor.cpp
        src/PluginEditorHighway.cpp
        src/PluginEditorLyrics.cpp
)

target_compile_definitions(RBVocalSing
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        RB_PAINT_PROFILE=$<BOOL:${RB_PAINT_PROFILE}>
        RB_PITCH_PROFILE=$<BOOL:${RB_PITCH_PROFILE}>
)

target_link_libraries(RBVocalSing
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_dsp             # juce::dsp::FFT for the pitch detector's NSDF
        juce::juce_opengl          # opt-in GPU rendering (experimental OpenGL toggle)
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if (RBVS_BUILD_TESTS)
    juce_add_console_app(RBVocalSingTests PRODUCT_NAME "RBVocalSingTests")
    juce_generate_juce_header(RBVocalSingTests)

    target_sources(RBVocalSingTests
        PRIVATE
            src/PluginProcessor.cpp
            src/PitchDetection.cpp
            src/NsdfPeak.cpp
            src/ChartLoader.cpp
            src/TakeExport.cpp
            src/StemAudioReader.cpp
            src/PluginEditor.cpp
            src/PluginEditorHighway.cpp
            src/PluginEditorLyrics.cpp
            tests/TestMain.cpp
            tests/PitchDetectionTests.cpp
            tests/PitchDetectorTests.cpp
            tests/StemAudioReaderTests.cpp
            tests/DetectionLatencyTests.cpp
            tests/NsdfPeakTests.cpp
            tests/ChartLoaderTests.cpp
            tests/StateTests.cpp
            tests/TakeExportTests.cpp
            tests/HighwayRangeTests.cpp
            tests/NoteMarkingsTests.cpp
            tests/NoteNamingTests.cpp
            tests/FrameHealthTests.cpp
            tests/TooltipBoundsTests.cpp
            tests/SliderTooltipTests.cpp
    )

    target_compile_definitions(RBVocalSingTests
        PUBLIC
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
            RB_PAINT_PROFILE=$<BOOL:${RB_PAINT_PROFILE}>
            RB_PITCH_PROFILE=$<BOOL:${RB_PITCH_PROFILE}>
    )

    target_include_directories(RBVocalSingTests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(RBVocalSingTests
        PRIVATE
            juce::juce_audio_processors
            juce::juce_audio_formats
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_opengl
            Catch2::Catch2
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags
    )
endif()
