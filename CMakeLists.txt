cmake_minimum_required(VERSION 3.25)

# Must precede project(): JUCE and the compiler read these at configure time.
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architectures")

# Release builds pass the tag's version; hosts compare it to decide whether to rescan the plugin.
set(CLICKMAKER_VERSION "0.1.0" CACHE STRING "Plugin version, major.minor.patch")

# No OBJC/OBJCXX languages: .mm files then build as CXX, so JUCE's hidden-visibility preset applies to
# its Objective-C++ modules and the plugin exports no weak symbols for other JUCE plugins to bind to.
project(ClickMaker VERSION ${CLICKMAKER_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.3.tar.gz
    URL_HASH SHA256=a81e5508b8a0efa483917794ebeaff56aed3075730c405a947734413f40c1aba)
FetchContent_MakeAvailable(JUCE)

# Shared by the plugin and the test runner; each target compiles its own copy.
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/Source)
add_library(ClickMakerCore INTERFACE)
target_sources(ClickMakerCore INTERFACE
    ${SRC}/Timeline.cpp
    ${SRC}/ClickEngine.cpp
    ${SRC}/VoiceBank.cpp
    ${SRC}/CueEngine.cpp
    ${SRC}/SpeechSynth.mm
    ${SRC}/SpeechWorker.cpp
    ${SRC}/PluginProcessor.cpp
    ${SRC}/PluginEditor.cpp
    ${SRC}/CuePanel.cpp)
set_source_files_properties(${SRC}/SpeechSynth.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_include_directories(ClickMakerCore INTERFACE ${SRC})
target_compile_definitions(ClickMakerCore INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)
target_link_libraries(ClickMakerCore INTERFACE
    juce::juce_audio_utils
    "-framework AVFoundation")

juce_add_plugin(ClickMaker
    COMPANY_NAME "ClickMaker"
    BUNDLE_ID com.clickmaker.clickmaker
    PLUGIN_NAME "ClickMaker"
    PRODUCT_NAME "ClickMaker"
    DESCRIPTION "Host-synced click and spoken cues"
    PLUGIN_MANUFACTURER_CODE Ckmk
    PLUGIN_CODE Clmk
    FORMATS AU Standalone
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD TRUE)

target_link_libraries(ClickMaker
    PRIVATE
        ClickMakerCore
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

juce_add_console_app(ClickMakerTests
    PRODUCT_NAME "ClickMakerTests"
    PLUGINHOST_AU TRUE)

target_sources(ClickMakerTests PRIVATE
    Tests/TestMain.cpp
    Tests/RenderHarness.cpp
    Tests/TimelineClickTests.cpp
    Tests/CueTests.cpp
    Tests/ProcessorTests.cpp
    Tests/AUHostedTests.cpp)

target_compile_definitions(ClickMakerTests PRIVATE
    JUCE_MODAL_LOOPS_PERMITTED=1
    CLICKMAKER_AU_IDENTIFIER="AudioUnit:Synths/aumu,Clmk,Ckmk"
    CLICKMAKER_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output")

target_link_libraries(ClickMakerTests PRIVATE
    ClickMakerCore
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# The runner hosts the installed AU in-process. Exported weak symbols (JUCE templates and singletons)
# would make dyld bind the plugin's JUCE to the runner's differently-configured copy.
set_target_properties(ClickMakerTests PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN TRUE)

enable_testing()
add_test(NAME unit COMMAND ClickMakerTests --category=Unit)
add_test(NAME processor COMMAND ClickMakerTests --category=Processor)
add_test(NAME refresh_au_registry COMMAND sh -c "killall -9 AudioComponentRegistrar 2>/dev/null; sleep 1; exit 0")
add_test(NAME au_hosted COMMAND ClickMakerTests --category=AUHosted)
add_test(NAME auval COMMAND auval -strict -v aumu Clmk Ckmk)
add_test(NAME auval_out_of_process COMMAND auval -oop -v aumu Clmk Ckmk)
set_tests_properties(refresh_au_registry PROPERTIES FIXTURES_SETUP au_registry)
set_tests_properties(au_hosted auval auval_out_of_process PROPERTIES
    FIXTURES_REQUIRED au_registry
    RESOURCE_LOCK installed_au)
