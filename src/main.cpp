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
//   --config <path>    Optional monitor/desktop wallpaper JSON
//
// 範例：
//   WallpaperEngine.exe C:\Videos\wallpaper.mp4
//   WallpaperEngine.exe C:\Videos\wallpaper.webm --fps 60
//   WallpaperEngine.exe C:\Videos\wallpaper.mp4 --debug --width 2560 --height 1440
// ============================================================================

#include "WallpaperEngine.h"
#include "TrayIcon.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <windows.h>
#include <shellapi.h>

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
    wprintf(L"  --config <P>   Monitor/desktop wallpaper JSON\n");
    wprintf(L"\nExamples:\n");
    wprintf(L"  %ls C:\\Videos\\wallpaper.mp4\n", exeName);
    wprintf(L"  %ls C:\\Videos\\wallpaper.webm --fps 60 --debug\n", exeName);
    wprintf(L"\n");
}

// ---------------------------------------------------------------------------
// Parse command-line arguments into WallpaperEngine::Config
// ---------------------------------------------------------------------------
static bool ParseArgs(int argc, wchar_t* argv[], WallpaperEngine::Config& cfg) {
    int cliCount = 0;
    int videoIndex = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--cli") == 0) {
            ++cliCount;
        } else if (videoIndex == 0) {
            videoIndex = i;
        }
    }
    if (cliCount > 1) {
        wprintf(L"[Error] --cli may only be specified once\n");
        PrintUsage(argv[0]);
        return false;
    }
    if (videoIndex == 0) {
        PrintUsage(argv[0]);
        return false;
    }

    // First positional argument: video file path
    cfg.videoPath = argv[videoIndex];

    // Check for help flag
    if (cfg.videoPath == L"--help" || cfg.videoPath == L"-h" || cfg.videoPath == L"/?") {
        PrintUsage(argv[0]);
        return false;
    }

    // Parse optional flags
    bool manualWidth  = false;
    bool manualHeight = false;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];

        if (i == videoIndex || arg == L"--cli") {
            continue;
        }
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
        else if (arg == L"--config") {
            if (i + 1 >= argc || std::wcsncmp(argv[i + 1], L"--", 2) == 0) {
                wprintf(L"[Error] --config requires a path\n");
                PrintUsage(argv[0]);
                return false;
            }
            cfg.configPath = argv[++i];
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

static bool IsValidStandardHandle(HANDLE handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    return GetFileType(handle) != FILE_TYPE_UNKNOWN || GetLastError() == ERROR_SUCCESS;
}

static bool BindStandardStream(HANDLE inherited, FILE* stream) {
    if (!IsValidStandardHandle(inherited)) {
        FILE* reopened = nullptr;
        return freopen_s(&reopened, "CONOUT$", "w", stream) == 0;
    }

    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), inherited, GetCurrentProcess(), &duplicate,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return false;
    }

    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(duplicate), _O_WRONLY | _O_TEXT);
    if (descriptor == -1) {
        CloseHandle(duplicate);
        return false;
    }
    if (_dup2(descriptor, _fileno(stream)) == -1) {
        _close(descriptor);
        return false;
    }
    _close(descriptor);
    clearerr(stream);
    return true;
}

static void AttachCliConsole() {
    const HANDLE inheritedStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE inheritedStderr = GetStdHandle(STD_ERROR_HANDLE);
    const bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
    const DWORD attachError = attached ? ERROR_SUCCESS : GetLastError();
    const bool stdoutBound = BindStandardStream(
        IsValidStandardHandle(inheritedStdout) ? inheritedStdout : GetStdHandle(STD_OUTPUT_HANDLE), stdout);
    const bool stderrBound = BindStandardStream(
        IsValidStandardHandle(inheritedStderr) ? inheritedStderr : GetStdHandle(STD_ERROR_HANDLE), stderr);
    if (!attached) {
        std::fprintf(stderr, "[main] AttachConsole failed: %lu\n", attachError);
    }
    if (!stdoutBound || !stderrBound) {
        std::fprintf(stderr, "[main] Failed to bind CLI output stream\n");
    }
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return 1;
    }

    bool cliRequested = false;
    for (int i = 1; i < argc; ++i) cliRequested |= std::wcscmp(argv[i], L"--cli") == 0;
    if (cliRequested) {
        AttachCliConsole();
    }

    int exitCode = 0;
    // Parse command-line arguments
    WallpaperEngine::Config cfg;
    if (!ParseArgs(argc, argv, cfg)) {
        exitCode = 1;
    }
    else if (!FileExists(cfg.videoPath)) {
        wprintf(L"[Error] Video file not found: %ls\n", cfg.videoPath.c_str());
        exitCode = 1;
    }
    else {
        WallpaperEngine engine;
        TrayIcon tray;

        try {
            engine.Initialize(cfg);
            tray.Initialize(GetModuleHandleW(nullptr), [&engine] { engine.Stop(); });
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

        tray.Shutdown();
        engine.Shutdown();
    }

    printf("[main] Exit code: %d\n", exitCode);
    LocalFree(argv);
    return exitCode;
}
