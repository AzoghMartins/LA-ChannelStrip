cmake_minimum_required(VERSION 3.22)

# macOS: one universal binary for Apple silicon and Intel Macs. Windows: link the C/C++ runtime
# statically so the plugin does not need the Visual C++ redistributable. (Both must be set before project().)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(LAChannelStrip VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    # M_PI and friends; sources are UTF-8
    add_compile_definitions(_USE_MATH_DEFINES)
    add_compile_options(/utf-8)
endif()

option(LACS_BUILD_TESTS "Build the DSP and plugin tests" ON)
option(LACS_COPY_AFTER_BUILD "Install the plugins into the user/system plugin folders after building" OFF)
set(LACS_JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (fetched from GitHub when empty)")

# ---------------------------------------------------------------------------------------------
# DSP engine (no JUCE dependency, unit tested separately)
add_library(lacs_engine STATIC
    Source/dsp/Modules.cpp
    Source/dsp/StripEngine.cpp)
target_include_directories(lacs_engine PUBLIC Source)
target_compile_options(lacs_engine PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O3>)

if(LACS_BUILD_TESTS)
    enable_testing()
    add_executable(lacs_tests tests/EngineTests.cpp)
    target_link_libraries(lacs_tests PRIVATE lacs_engine)
    target_compile_options(lacs_tests PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-O2>)
    add_test(NAME engine COMMAND lacs_tests)
endif()

if(LACS_ENGINE_ONLY)
    return()
endif()

# ---------------------------------------------------------------------------------------------
# JUCE
if(LACS_JUCE_DIR)
    add_subdirectory(${LACS_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

set(LACS_PLUGIN_SOURCES
    Source/Parameters.cpp
    Source/PluginProcessor.cpp
    Source/gui/Theme.cpp
    Source/gui/Widgets.cpp
    Source/gui/Displays.cpp
    Source/gui/PluginEditor.cpp)

set(LACS_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

# VST3 everywhere; LV2 + Standalone on Linux; AU on macOS
if(WIN32)
    set(LACS_FORMATS VST3)
elseif(APPLE)
    set(LACS_FORMATS VST3 AU)
else()
    set(LACS_FORMATS VST3 LV2 Standalone)
endif()

# ---------------------------------------------------------------------------------------------
# Plugin
juce_add_plugin(LAChannelStrip
    PRODUCT_NAME "LA-ChannelStrip"
    COMPANY_NAME "LA Audio"
    BUNDLE_ID "com.la-audio.lachannelstrip"
    PLUGIN_MANUFACTURER_CODE LaAu
    PLUGIN_CODE LaCs
    FORMATS ${LACS_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Dynamics EQ
    LV2URI "https://github.com/AzoghMartins/LA-ChannelStrip"
    COPY_PLUGIN_AFTER_BUILD ${LACS_COPY_AFTER_BUILD})

target_sources(LAChannelStrip PRIVATE ${LACS_PLUGIN_SOURCES})
target_compile_definitions(LAChannelStrip PUBLIC ${LACS_DEFINITIONS} JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(LAChannelStrip
    PRIVATE
        lacs_engine
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------------------
if(LACS_BUILD_TESTS)
    # Drives the real processor + editor like a host (needs an X display, e.g. xvfb-run).
    juce_add_console_app(lacs_integration PRODUCT_NAME "LA-ChannelStrip Integration Test")
    target_sources(lacs_integration PRIVATE tests/PluginIntegrationTest.cpp ${LACS_PLUGIN_SOURCES})
    target_compile_definitions(lacs_integration PRIVATE JucePlugin_Name="LA-ChannelStrip" ${LACS_DEFINITIONS})
    target_link_libraries(lacs_integration PRIVATE
        lacs_engine
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_recommended_config_flags)
    add_test(NAME integration COMMAND lacs_integration)
endif()
