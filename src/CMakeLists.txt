cmake_minimum_required(VERSION 3.20)
project(AI_NPC LANGUAGES CXX)

# Current DK22Pac/plugin-sdk uses modern C++ features.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# plugin-sdk's current GTA SA library is built with the static MSVC CRT (/MT).
# Match it exactly to prevent LNK2038 RuntimeLibrary mismatches.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(
    PLUGIN_SDK_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/plugin-sdk"
    CACHE PATH
    "Path to DK22Pac/plugin-sdk"
)

if(NOT WIN32)
    message(FATAL_ERROR
        "This target must be built on Windows with the x86 MSVC toolchain."
    )
endif()

if(NOT EXISTS "${PLUGIN_SDK_DIR}/shared/plugin.h")
    message(FATAL_ERROR
        "plugin-sdk not found: ${PLUGIN_SDK_DIR}/shared/plugin.h"
    )
endif()

set(PLUGIN_SA_DIR "${PLUGIN_SDK_DIR}/plugin_sa")
set(GAME_SA_DIR "${PLUGIN_SA_DIR}/game_sa")
set(RW_DIR "${GAME_SA_DIR}/rw")

if(NOT EXISTS "${GAME_SA_DIR}")
    message(FATAL_ERROR
        "GTA SA plugin-sdk directory not found: ${GAME_SA_DIR}"
    )
endif()

if(NOT EXISTS "${RW_DIR}/rwcore.h")
    message(FATAL_ERROR
        "RenderWare headers not found: ${RW_DIR}/rwcore.h"
    )
endif()

find_library(
    PLUGIN_SDK_LIB
    NAMES Plugin
    PATHS "${PLUGIN_SDK_DIR}/output/lib"
    NO_DEFAULT_PATH
)

if(NOT PLUGIN_SDK_LIB)
    message(FATAL_ERROR
        "Plugin.lib was not found in ${PLUGIN_SDK_DIR}/output/lib"
    )
endif()

# plugin-sdk Shader.obj imports legacy D3DX9 functions. The workflow obtains
# the official Microsoft.DXSDK.D3DX package and passes this exact x86 library.
set(
    D3DX9_LIBRARY
    ""
    CACHE FILEPATH
    "Path to x86 d3dx9.lib"
)

if(NOT D3DX9_LIBRARY)
    find_library(
        D3DX9_LIBRARY
        NAMES d3dx9
        PATHS
            "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/D3DX/build/native/release/lib/x86"
        NO_DEFAULT_PATH
    )
endif()

if(NOT D3DX9_LIBRARY)
    message(FATAL_ERROR
        "d3dx9.lib was not found. Install Microsoft.DXSDK.D3DX 9.29.952.8 or pass -DD3DX9_LIBRARY=<path>."
    )
endif()

add_library(AI_NPC SHARED main.cpp)

set_property(TARGET AI_NPC PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
set_property(TARGET AI_NPC PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

# GTA SA 1.0 US is a 32-bit target. The workflow configures Win32.
target_compile_definitions(
    AI_NPC
    PRIVATE
        GTASA
        PLUGIN_SGV_10US
        RW
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

target_include_directories(
    AI_NPC
    PRIVATE
        "${GAME_SA_DIR}"
        "${PLUGIN_SA_DIR}"
        "${GAME_SA_DIR}/enums"
        "${PLUGIN_SDK_DIR}/shared"
        "${PLUGIN_SDK_DIR}/shared/game"
        "${RW_DIR}"
        "${PLUGIN_SDK_DIR}"
)

target_compile_options(
    AI_NPC
    PRIVATE
        /std:c++latest
        /EHsc
)

target_link_options(
    AI_NPC
    PRIVATE
        /LTCG
)

target_link_libraries(
    AI_NPC
    PRIVATE
        "${PLUGIN_SDK_LIB}"
        "${D3DX9_LIBRARY}"
        winhttp
        user32
        gdi32
)

set_target_properties(
    AI_NPC
    PROPERTIES
        OUTPUT_NAME "AI_NPC"
        SUFFIX ".asi"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)
