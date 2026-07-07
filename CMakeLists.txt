cmake_minimum_required(VERSION 3.22)

project(LightHost VERSION 1.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    PRODUCT_NAME                  "Light Host"
    VERSION                       ${PROJECT_VERSION}
    COMPANY_NAME                  "LightHost"
    BUNDLE_ID                     "com.lighthost.app"
    ICON_BIG                      "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT    "Light Host needs audio input to process it through your plugins."
)

juce_generate_juce_header(LightHost)

target_sources(LightHost PRIVATE
    Source/HostStartup.cpp
    Source/IconMenu.cpp
    Source/PluginWindow.cpp
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
