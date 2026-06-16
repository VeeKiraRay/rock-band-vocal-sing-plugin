cmake_minimum_required(VERSION 3.22)
project(RBVocalSing VERSION 0.1.0)

include(cmake/CPM.cmake)

CPMAddPackage(
    NAME JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 7.0.12
)

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
        src/ChartLoader.cpp
        src/PluginEditor.cpp
        src/PluginEditorHighway.cpp
        src/PluginEditorLyrics.cpp
)

target_compile_definitions(RBVocalSing
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(RBVocalSing
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_opengl          # opt-in GPU rendering (experimental OpenGL toggle)
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
