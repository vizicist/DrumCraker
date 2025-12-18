cmake_minimum_required(VERSION 3.15)
project(DrumCrakerVST VERSION 1.2.2)

# Define plugin version
set(PLUGIN_VERSION_MAJOR 1)
set(PLUGIN_VERSION_MINOR 2)
set(PLUGIN_VERSION_PATCH 3)
set(PLUGIN_VERSION "${PLUGIN_VERSION_MAJOR}.${PLUGIN_VERSION_MINOR}.${PLUGIN_VERSION_PATCH}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Performance optimizations (conservative for DAW compatibility)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -funroll-loops -fomit-frame-pointer -DNDEBUG")
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-s")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "-s")
endif()

# JUCE
add_subdirectory(JUCE)

juce_add_plugin(DrumCrakerVST
    COMPANY_NAME "Wamphyre"
    PLUGIN_MANUFACTURER_CODE Wamp
    PLUGIN_CODE Dcvt
    FORMATS VST3
    PRODUCT_NAME "DrumCraker"
    VST3_CATEGORIES Instrument Sampler
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_SYNTH TRUE
    IS_MIDI_EFFECT FALSE
)

target_sources(DrumCrakerVST PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/DrumKitLoader.cpp
    src/SampleEngine.cpp
    src/VoiceManager.cpp
)

target_compile_definitions(DrumCrakerVST PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=0
    PLUGIN_VERSION="${PLUGIN_VERSION}"
)

target_link_libraries(DrumCrakerVST 
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
