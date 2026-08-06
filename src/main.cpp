// ============================================================================
// main.cpp — Module 8: Entry point, command-line parsing
// ============================================================================
//
// 用法：
//   WallpaperEngine.exe <video_path> [options]
//
// 選項：
//   --fps <N>          目標 FPS（預設 60）
//   --debug            啟用 DX12 Debug Layer
//   --width <N>        手動指定視窗寬度（停用自動偵測）
//   --height <N>       手動指定視窗高度（停用自動偵測）
//
// 範例：
//   WallpaperEngine.exe C:\Videos\wallpaper.mp4
//   WallpaperEngine.exe C:\Videos\wallpaper.webm --fps 60
//   WallpaperEngine.exe C:\Videos\wallpaper.mp4 --debug --width 2560 --height 1440
// ============================================================================

#include "WallpaperEngine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <windows.h>

// ---------------------------------------------------------------------------
// Print usage help
// ---------------------------------------------------------------------------
static void PrintUsage(const wchar_t* exeName) {
    wprintf(L"\n");
    wprintf(L"DX12 Wallpaper Engine v0.1\n");
    wprintf(L"=========================\n\n");
    wprintf(L"Usage:\n");
    wprintf(L"  %ls <video_path> [options]\n\n", exeName);
    wprintf(L"Options:\n");
    wprintf(L"  --fps <N>      Target FPS (1-60, default: 60)\n");
    wprintf(L"  --debug        Enable DX12 debug layer\n");
    wprintf(L"  --width <N>    Manual window width  (disables auto-detect)\n");
    wprintf(L"  --height <N>   Manual window height (disables auto-detect)\n");
    wprintf(L"\nExamples:\n");
    wprintf(L"  %ls C:\\Videos\\wallpaper.mp4\n", exeName);
    wprintf(L"  %ls C:\\Videos\\wallpaper.webm --fps 60 --debug\n", exeName);
    wprintf(L"\n");
}

// ---------------------------------------------------------------------------
// Parse command-line arguments into WallpaperEngine::Config
// ---------------------------------------------------------------------------
static bool ParseArgs(int argc, wchar_t* argv[], WallpaperEngine::Config& cfg) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return false;
    }

    // First positional argument: video file path
    cfg.videoPath = argv[1];

    // Check for help flag
    if (cfg.videoPath == L"--help" || cfg.videoPath == L"-h" || cfg.videoPath == L"/?") {
        PrintUsage(argv[0]);
        return false;
    }

    // Parse optional flags
    bool manualWidth  = false;
    bool manualHeight = false;

    for (int i = 2; i < argc; ++i) {
        std::wstring arg = argv[i];

        if (arg == L"--fps" && i + 1 < argc) {
            cfg.targetFPS = static_cast<uint32_t>(_wtoi(argv[++i]));
            if (cfg.targetFPS == 0) cfg.targetFPS = 60;
        }
        else if (arg == L"--debug") {
            cfg.enableDebugLayer = true;
        }
        else if (arg == L"--width" && i + 1 < argc) {
            cfg.windowWidth = static_cast<uint32_t>(_wtoi(argv[++i]));
            manualWidth = true;
        }
        else if (arg == L"--height" && i + 1 < argc) {
            cfg.windowHeight = static_cast<uint32_t>(_wtoi(argv[++i]));
            manualHeight = true;
        }
        else {
            wprintf(L"[Error] Unknown option: %ls\n", arg.c_str());
            PrintUsage(argv[0]);
            return false;
        }
    }

    // If either dimension is manually specified, disable auto-detect
    if (manualWidth || manualHeight) {
        cfg.autoDetectSize = false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Validate that the video file exists
// ---------------------------------------------------------------------------
static bool FileExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// ============================================================================
// wWinMain — Windows GUI entry point (no console window by default)
//
// To also support console usage, we use wmain below and set the subsystem
// to CONSOLE in the build system. If SUBSYSTEM:WINDOWS is used instead,
// replace wmain with wWinMain.
// ============================================================================

int wmain(int argc, wchar_t* argv[]) {
    // Enable UTF-8 console output
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Parse command-line arguments
    WallpaperEngine::Config cfg;
    if (!ParseArgs(argc, argv, cfg)) {
        return 1;
    }

    // Validate video file
    if (!FileExists(cfg.videoPath)) {
        wprintf(L"[Error] Video file not found: %ls\n", cfg.videoPath.c_str());
        return 1;
    }

    // Run the engine
    WallpaperEngine engine;
    int exitCode = 0;

    try {
        engine.Initialize(cfg);
        engine.Run();
    }
    catch (const std::runtime_error& e) {
        printf("[Fatal Error] %s\n", e.what());
        exitCode = 1;
    }
    catch (...) {
        printf("[Fatal Error] Unknown exception\n");
        exitCode = 1;
    }

    // Explicit shutdown (also called by destructor, but being explicit is clearer)
    engine.Shutdown();

    printf("[main] Exit code: %d\n", exitCode);
    return exitCode;
}
