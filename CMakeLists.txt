cmake_minimum_required(VERSION 3.22)

# CMP0091: select the MSVC runtime via CMAKE_MSVC_RUNTIME_LIBRARY (not compile flags).
# Must be set before the first project() call to take effect.
cmake_policy(SET CMP0091 NEW)

# LightHostGUI — a GUI fork of upstream LightHost (https://github.com/opencma/LightHost).
# Re-stamped as a fresh 1.0.0 identity; forked from LightHost 1.3.0.
project(LightHostGUI VERSION 1.0.0
        DESCRIPTION "GUI fork of upstream LightHost 1.3.0"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Statically link the MSVC runtime so the Release exe is a single self-contained
# binary (no VC++ redistributable needed on the target machine).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# JUCE 8 — fetched at configure time, pinned. First configure downloads ~40 MB.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.13
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# App target
# ---------------------------------------------------------------------------
juce_add_gui_app(LightHost
    PRODUCT_NAME                  "LightHost GUI"
    VERSION                       ${PROJECT_VERSION}
    COMPANY_NAME                  "LightHost"
    BUNDLE_ID                     "com.lighthost.app"
    ICON_BIG                      "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT    "LightHost GUI needs audio input to process it through your plugins."
)

juce_generate_juce_header(LightHost)

target_sources(LightHost PRIVATE
    Source/HostStartup.cpp
    Source/IconMenu.cpp
    Source/PluginWindow.cpp
    Source/Engine/GraphDocument.cpp
    Source/Engine/GraphController.cpp
    Source/Engine/PresetStore.cpp
    Source/Engine/SelfTest.cpp
    Source/UI/MainComponent.cpp
    Source/UI/NodeCard.cpp
    Source/UI/CanvasView.cpp
    Source/UI/TitleBar.cpp
)

# Tray icons referenced in code as BinaryData::menu_icon_png / menu_icon_white_png
juce_add_binary_data(LightHostAssets SOURCES
    Resources/menu_icon.png
    Resources/menu_icon_white.png
)

target_compile_definitions(LightHost PRIVATE
    # Plugin formats: VST3 everywhere, AU on macOS. VST2 SDK is not
    # redistributable — leave off unless you own the legacy SDK.
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    $<$<PLATFORM_ID:Darwin>:JUCE_PLUGINHOST_AU=1>

    # Slim the build; revisit JUCE_WEB_BROWSER if the WebView GUI option is chosen.
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0

    # Existing code uses modal loops (DialogWindow::runModal) — off by default in JUCE 8.
    JUCE_MODAL_LOOPS_PERMITTED=1

    JUCE_DISPLAY_SPLASH_SCREEN=0    # allowed: this project is GPLv3

    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LightHost,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LightHost,JUCE_VERSION>"
)

target_link_libraries(LightHost PRIVATE
    LightHostAssets
    juce::juce_audio_utils   # pulls audio_basics/devices/formats/processors, gui_basics
    juce::juce_gui_extra     # SystemTrayIconComponent
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ---------------------------------------------------------------------------
# ASIO (Windows low-latency driver API). Steinberg's SDK is header-only and
# freely downloadable but not redistributable, so it lives OUTSIDE the repo.
# Point ASIO_SDK_DIR at the SDK root (the folder containing 'common') to compile
# it in; leave it empty/invalid to build without ASIO (WASAPI exclusive mode is
# still available). On this machine the path is set in the local, untracked
# CMakeSettings.json. Defining JUCE_ASIO=1 + the common/ include path is the
# standard JUCE requirement: it enables the ASIO backend in juce_audio_devices,
# which AudioDeviceSelectorComponent then lists automatically.
# ---------------------------------------------------------------------------
set(ASIO_SDK_DIR "" CACHE PATH
    "Steinberg ASIO SDK root (folder containing 'common'); empty/invalid = build without ASIO")

if(WIN32 AND ASIO_SDK_DIR AND EXISTS "${ASIO_SDK_DIR}/common/iasiodrv.h")
    target_compile_definitions(LightHost PRIVATE JUCE_ASIO=1)
    target_include_directories(LightHost PRIVATE "${ASIO_SDK_DIR}/common")
    message(STATUS "ASIO enabled: ${ASIO_SDK_DIR}")
else()
    message(STATUS "ASIO SDK not found; building without ASIO (WASAPI exclusive still available)")
endif()

# ---------------------------------------------------------------------------
# Headless unit tests (off by default). Exercises the graph document model
# with no audio device / real plugins / display, so it runs in CI or a sandbox.
#   cmake -B build -DLIGHTHOST_BUILD_TESTS=ON
#   cmake --build build --target LightHostTests && ./build/LightHostTests_artefacts/LightHostTests
# ---------------------------------------------------------------------------
option(LIGHTHOST_BUILD_TESTS "Build headless unit tests" OFF)

if(LIGHTHOST_BUILD_TESTS)
    juce_add_console_app(LightHostTests PRODUCT_NAME "LightHostTests")
    juce_generate_juce_header(LightHostTests)

    target_sources(LightHostTests PRIVATE
        Tests/GraphDocumentTests.cpp
        Source/Engine/PresetStore.cpp
        Tests/MeterTests.cpp
        Tests/StripTests.cpp
        Tests/ParamTests.cpp
        Tests/RoutingTests.cpp
        Tests/MidiTests.cpp
        Tests/PresetTests.cpp
        Source/Engine/GraphDocument.cpp
    )

    target_compile_definitions(LightHostTests PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
    )

    target_link_libraries(LightHostTests PRIVATE
        juce::juce_audio_processors   # PluginDescription, KnownPluginList (pulls core/data_structures)
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )
endif()

# ---------------------------------------------------------------------------
# Runtime tests (off by default). Drives the real GraphController against
# installed VST3 plugins — duplicate-plugin coexistence, bypass-survives-restart,
# and legacy first-run migration — with no tray menu or display interaction.
# Skips (exit 0) on a box with no loadable stereo VST3.
#   cmake -B build -DLIGHTHOST_BUILD_RUNTIME_TESTS=ON
#   cmake --build build --config Release --target LightHostRuntimeTests
#   "build/LightHostRuntimeTests_artefacts/Release/LightHostRuntimeTests.exe"
# ---------------------------------------------------------------------------
option(LIGHTHOST_BUILD_RUNTIME_TESTS "Build runtime tests that load real plugins" OFF)

if(LIGHTHOST_BUILD_RUNTIME_TESTS)
    juce_add_console_app(LightHostRuntimeTests PRODUCT_NAME "LightHostRuntimeTests")
    juce_generate_juce_header(LightHostRuntimeTests)

    target_sources(LightHostRuntimeTests PRIVATE
        Tests/GraphRuntimeTests.cpp
        Source/Engine/SelfTest.cpp
        Source/Engine/GraphDocument.cpp
        Source/Engine/GraphController.cpp
        Source/Engine/PresetStore.cpp
        Source/PluginWindow.cpp
    )

    target_compile_definitions(LightHostRuntimeTests PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
        $<$<PLATFORM_ID:Darwin>:JUCE_PLUGINHOST_AU=1>
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_STANDALONE_APPLICATION=1
    )

    target_link_libraries(LightHostRuntimeTests PRIVATE
        juce::juce_audio_utils   # AudioProcessorGraph, format manager, plugin instances
        juce::juce_gui_extra     # DocumentWindow base for PluginWindow
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )
endif()
