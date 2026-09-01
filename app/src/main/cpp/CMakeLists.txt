cmake_minimum_required(VERSION 3.22.1)
project(apfa LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# BASS / BASSMIDI roots, relative to this file (aPFA/app/src/main/cpp).
set(BASS_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/../../../../bass24-android)
set(BASSMIDI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../bassmidi24-android)

# Prebuilt BASS / BASSMIDI as imported shared libraries (per-ABI).
add_library(bass SHARED IMPORTED)
set_target_properties(bass PROPERTIES
    IMPORTED_LOCATION ${BASS_DIR}/libs/${ANDROID_ABI}/libbass.so)

add_library(bassmidi SHARED IMPORTED)
set_target_properties(bassmidi PROPERTIES
    IMPORTED_LOCATION ${BASSMIDI_DIR}/libs/${ANDROID_ABI}/libbassmidi.so)

# The aPFA engine. Both renderers are compiled in: renderer.cpp = RendererES2
# (ES2 baseline, also the iOS renderer) and renderer_es3.cpp = RendererES3 (the
# Android default). createRenderer() picks one at runtime per the Legacy toggle.
add_library(apfa SHARED
    native-lib.cpp
    bigalloc.cpp
    engine.cpp
    midi_parser.cpp
    synth.cpp
    streamer.cpp
    renderer.cpp
    renderer_es3.cpp)

# bigalloc.cpp gives every allocation of 8 MB or more its own mmap, so freeing
# one returns the ADDRESS SPACE and not just the pages — see the file header.
# It compiles to nothing on a 64-bit ABI, and is why it sits next to the engine
# rather than inside the streaming gate below: the in-RAM parse allocates the
# same size of table and retains it the same way.

# Streaming event pool (file-backed 72-byte pool behind a sliding window —
# streamer.h) is part of the stock Android build; the runtime gate is
# Engine::load's adaptive check (RAM-fit prediction / crash marker / user
# toggle). The macro stays a macro so iOS builds, which never define it,
# keep the pure in-RAM parse.
target_compile_definitions(apfa PRIVATE APFA_STREAMING)

# The legit run must reflect the fastest the hardware genuinely allows — keep the
# native engine optimised even in debug builds (an -O0 engine would be unfaithful).
target_compile_options(apfa PRIVATE -O2)

target_include_directories(apfa PRIVATE
    ${BASS_DIR}/c
    ${BASSMIDI_DIR}/c)

target_link_libraries(apfa
    bass
    bassmidi
    EGL
    # Link only GLESv2 (NOT GLESv3). RendererES3 resolves its ES3-only entry points
    # (VAOs + instancing) dynamically via eglGetProcAddress, and RendererES2 resolves
    # any instanced-arrays the same way, so libapfa.so carries no static libGLESv3
    # dependency — it must stay loadable on ES2-only GPUs so those devices can still
    # launch and reach the "Legacy Renderer" toggle.
    GLESv2
    android
    log)
