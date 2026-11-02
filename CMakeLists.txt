cmake_minimum_required(VERSION 3.22)
project(StageTracks VERSION 0.5.5 LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# NEEDS_WEBVIEW2: JUCE descarga el SDK de WebView2 (NuGet) en el primer configure y lo enlaza estaticamente.
juce_add_gui_app(StageTracks
    PRODUCT_NAME "StageTracks"
    COMPANY_NAME "StageTracks"
    BUNDLE_ID "com.stagetracks.player"
    VERSION "0.5.5"
    ICON_BIG "${CMAKE_SOURCE_DIR}/assets/app-icon.png"
    ICON_SMALL "${CMAKE_SOURCE_DIR}/assets/app-icon.png"
    NEEDS_WEBVIEW2 TRUE)

# La interfaz (copiada de REAPER Playback) va embebida en el ejecutable.
juce_add_binary_data(StageTracksWeb SOURCES web/studio.html web/main.js)

juce_generate_juce_header(StageTracks)
target_sources(StageTracks PRIVATE
    src/Main.cpp
    src/MainComponent.cpp
    src/AudioEngine.cpp
    src/PlayerBridge.cpp
    src/WebControlServer.cpp)
target_include_directories(StageTracks PRIVATE src)

# ---- ASIO (opcional) ----
# El SDK de ASIO es de Steinberg y no se puede descargar automaticamente (hay que aceptar su licencia).
# 1) Descargalo de https://www.steinberg.net/asiosdk  2) descomprimelo en  third_party/asiosdk  (debe quedar third_party/asiosdk/common/iasiodrv.h)
#    o pasa -DASIO_SDK_DIR=C:/ruta/al/sdk, o define la variable de entorno ASIO_SDK_DIR.
set(ASIO_SDK_DIR "" CACHE PATH "Carpeta del SDK de ASIO de Steinberg (la que contiene common/iasiodrv.h)")
if(NOT ASIO_SDK_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/ASIOSDK/common/iasiodrv.h")
    set(ASIO_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/ASIOSDK" CACHE PATH
        "Carpeta del SDK de ASIO de Steinberg (la que contiene common/iasiodrv.h)" FORCE)
endif()
file(GLOB _asio_guesses LIST_DIRECTORIES TRUE "${CMAKE_SOURCE_DIR}/third_party/asio*" "${CMAKE_SOURCE_DIR}/third_party/ASIO*")
find_path(ASIO_COMMON_DIR NAMES iasiodrv.h
    HINTS "${ASIO_SDK_DIR}" "$ENV{ASIO_SDK_DIR}" ${_asio_guesses}
    PATH_SUFFIXES common . ASIOSDK2.3.3/common ASIOSDK2.3.2/common ASIOSDK2.3.1/common ASIOSDK2.3/common)
if(ASIO_COMMON_DIR)
    message(STATUS "ASIO: activado (SDK en ${ASIO_COMMON_DIR})")
    target_compile_definitions(StageTracks PRIVATE JUCE_ASIO=1 STAGETRACKS_HAS_ASIO=1)
    target_include_directories(StageTracks PRIVATE "${ASIO_COMMON_DIR}")
else()
    message(WARNING "ASIO: NO activado (no se encontro el SDK de ASIO). Se usara WASAPI/DirectSound. Mira el README, seccion ASIO.")
endif()
target_compile_features(StageTracks PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(StageTracks PRIVATE /utf-8)
endif()
target_compile_definitions(StageTracks PRIVATE
    JUCE_WEB_BROWSER=1 JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_USE_CURL=0 JUCE_APPLICATION_NAME_STRING="StageTracks"
    JUCE_APPLICATION_VERSION_STRING="0.5.5")
target_link_libraries(StageTracks PRIVATE
    StageTracksWeb
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

add_custom_command(TARGET StageTracks POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:StageTracks>/Pads"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Pads" "$<TARGET_FILE_DIR:StageTracks>/Pads"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/assets/app-icon.png" "$<TARGET_FILE_DIR:StageTracks>/app-icon.png"
    COMMENT "Copying chromatic pad loops beside StageTracks")
