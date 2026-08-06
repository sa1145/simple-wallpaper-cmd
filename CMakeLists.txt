# ============================================================================
# CMakeLists.txt — DX12 Wallpaper Engine
# ============================================================================
#
# Build requirements:
#   - CMake 3.21+
#   - Visual Studio 2022 (MSVC v143+)
#   - Windows SDK 10.0.19041.0+
#
# Build commands:
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#
# Or with Ninja (faster):
#   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
# ============================================================================

cmake_minimum_required(VERSION 3.21)

project(DX12WallpaperEngine
    VERSION     0.1.0
    LANGUAGES   CXX
    DESCRIPTION "Minimal GPU-overhead wallpaper playback engine using DX12 Flip Model"
)

# ---------------------------------------------------------------------------
# C++ Standard — C++20 per spec
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(HEADER_FILES
    src/Common.h
    src/WindowManager.h
    src/DX12Context.h
    src/FrameRingBuffer.h
    src/VideoDecoder.h
    src/UploadPipeline.h
    src/PresentLoop.h
    src/WallpaperEngine.h
)

set(SOURCE_FILES
    src/WindowManager.cpp
    src/DX12Context.cpp
    src/VideoDecoder.cpp
    src/UploadPipeline.cpp
    src/PresentLoop.cpp
    src/WallpaperEngine.cpp
    src/main.cpp
)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(${PROJECT_NAME}
    ${SOURCE_FILES}
    ${HEADER_FILES}
)

# Console subsystem (wmain entry point)
# Change to WIN32 if switching to wWinMain
set_target_properties(${PROJECT_NAME} PROPERTIES
    WIN32_EXECUTABLE        FALSE
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# Compile definitions
# ---------------------------------------------------------------------------
target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN         # Exclude rarely-used Windows headers
    NOMINMAX                    # Prevent Windows.h min/max macros
    UNICODE                     # Use wide-char Win32 APIs
    _UNICODE
)

# ---------------------------------------------------------------------------
# Compiler options (MSVC)
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4                     # Warning level 4
        /WX-                    # Don't treat warnings as errors (relax for development)
        /permissive-            # Strict conformance mode
        /Zc:__cplusplus         # Report correct __cplusplus value
        /Zc:preprocessor        # Use conforming preprocessor
        /utf-8                  # Source and execution charset UTF-8
        /MP                     # Multi-processor compilation
    )

    # Release optimizations
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/O2 /Oi /GL /GS->
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
    )

    # Debug settings
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:/Od /Zi /RTC1>
    )
endif()

# ---------------------------------------------------------------------------
# Link libraries (per spec)
#
# d3d12.lib       — Direct3D 12 API
# dxgi.lib        — DXGI (swap chain, adapter enumeration)
# d3dcompiler.lib — Shader compilation (reserved for future use)
# mfplat.lib      — Media Foundation platform
# mfreadwrite.lib — MF Source Reader
# mfuuid.lib      — MF GUIDs
# propsys.lib     — PropVariant utilities (PropVariantInit/Clear)
# dwmapi.lib      — DWM window attributes (cloaked state)
# ---------------------------------------------------------------------------
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d12.lib
    dxgi.lib
    d3dcompiler.lib
    mfplat.lib
    mfreadwrite.lib
    mfuuid.lib
    propsys.lib
    d3d11.lib
    dwmapi.lib
)

add_executable(FrameQueueCheck src/FrameQueueCheck.cpp)
target_include_directories(FrameQueueCheck PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(FrameQueueCheck PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(FrameQueueCheck PRIVATE /W4 /permissive- /utf-8)
endif()

# ---------------------------------------------------------------------------
# Platform check
# ---------------------------------------------------------------------------
if(NOT WIN32)
    message(FATAL_ERROR "This project requires Windows 10/11 with DX12 support.")
endif()

# ---------------------------------------------------------------------------
# Install (optional)
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "========================================")
message(STATUS " DX12 Wallpaper Engine v${PROJECT_VERSION}")
message(STATUS " C++ Standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS " Generator:     ${CMAKE_GENERATOR}")
message(STATUS " Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "========================================")
