#include "MonitorCoordinator.h"

#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <iterator>
#include <stdexcept>

namespace {
void Require(bool condition) {
    if (!condition) throw std::runtime_error("MonitorCoordinatorCheck requirement failed");
}

struct EngineWindowCount {
    UINT value = 0;
};

BOOL CALLBACK CountEngineChild(HWND hwnd, LPARAM parameter) {
    wchar_t className[64]{};
    if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className)))
        && wcscmp(className, L"DX12WallpaperEngineClass") == 0) {
        ++reinterpret_cast<EngineWindowCount*>(parameter)->value;
    }
    return TRUE;
}

BOOL CALLBACK CountEngineChildren(HWND hwnd, LPARAM parameter) {
    EnumChildWindows(hwnd, CountEngineChild, parameter);
    return TRUE;
}

UINT CountEngineWindows() {
    EngineWindowCount count;
    EnumWindows(CountEngineChildren, reinterpret_cast<LPARAM>(&count));
    return count.value;
}
}

int wmain() {
    try {
        GUID desktopOne{};
        GUID desktopTwo{};
        Require(SUCCEEDED(CLSIDFromString(L"{11111111-1111-1111-1111-111111111111}", &desktopOne)));
        Require(SUCCEEDED(CLSIDFromString(L"{22222222-2222-2222-2222-222222222222}", &desktopTwo)));

        WallpaperConfig wallpaperConfig;
        wallpaperConfig.LoadJson(R"json({
            "profiles": {
                "fallback": {"video": "D:/fallback.mp4", "fps": 20},
                "profile": {"video": "D:/profile.mp4", "fps": 30}
            },
            "monitors": {
                "MONITOR-A": {
                    "{11111111-1111-1111-1111-111111111111}": {"video": "D:/direct.mp4", "fps": 40},
                    "{22222222-2222-2222-2222-222222222222}": {"profile": "profile"}
                },
                "MONITOR-B": {
                    "{11111111-1111-1111-1111-111111111111}": {"video": "D:/other.mp4", "fps": 50}
                }
            },
            "fallback": "fallback"
        })json");
        MonitorCoordinator::Config resolutionConfig;
        resolutionConfig.videoPath = L"D:/base.mp4";
        resolutionConfig.targetFPS = 60;
        resolutionConfig.wallpaperConfig = &wallpaperConfig;
        MonitorDescriptor firstDescriptor;
        firstDescriptor.devicePath = L"MONITOR-A";
        MonitorDescriptor secondDescriptor;
        secondDescriptor.devicePath = L"MONITOR-B";
        const auto direct = MonitorCoordinator::ResolveWallpaperForMonitor(
            resolutionConfig, firstDescriptor, desktopOne);
        const auto profile = MonitorCoordinator::ResolveWallpaperForMonitor(
            resolutionConfig, firstDescriptor, desktopTwo);
        const auto isolated = MonitorCoordinator::ResolveWallpaperForMonitor(
            resolutionConfig, secondDescriptor, desktopOne);
        const auto fallback = MonitorCoordinator::ResolveWallpaperForMonitor(
            resolutionConfig, secondDescriptor, desktopTwo);
        MonitorCoordinator::Config baseConfig;
        baseConfig.videoPath = L"D:/base.mp4";
        baseConfig.targetFPS = 60;
        const auto base = MonitorCoordinator::ResolveWallpaperForMonitor(
            baseConfig, firstDescriptor, desktopOne);
        Require(direct.videoPath == L"D:/direct.mp4" && direct.fps == 40);
        Require(profile.videoPath == L"D:/profile.mp4" && profile.fps == 30);
        Require(isolated.videoPath == L"D:/other.mp4" && isolated.fps == 50);
        Require(fallback.videoPath == L"D:/fallback.mp4" && fallback.fps == 20);
        Require(base.videoPath == L"D:/base.mp4" && base.fps == 60);

        auto monitors = EnumerateActiveMonitors();
        Require(!monitors.empty());

        MonitorCoordinator coordinator;
        MonitorCoordinator::Config config;
        config.videoPath = L"D:\\test_cpp_build\\test.mp4";
        bool rejectSecondary = false;
        config.budgetQuery = [&rejectSecondary](IDXGIAdapter3*) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (rejectSecondary) {
                info.CurrentUsage = 1;
                info.Budget = 0;
            }
            return info;
        };
        coordinator.Initialize(config);
        coordinator.Start();

        const MonitorDescriptor primary = monitors.front();
        const HWND preserved = coordinator.GetPipelineWindowForCheck(primary.devicePath);
        Require(preserved && IsWindow(preserved));

        for (wchar_t& character : monitors.front().devicePath)
            character = static_cast<wchar_t>(std::towupper(character));
        monitors.front().gdiDeviceName = L"transient-gdi-name";
        monitors.front().monitor = reinterpret_cast<HMONITOR>(static_cast<INT_PTR>(1));
        Require(coordinator.Reconcile(monitors));
        Require(coordinator.GetPipelineWindowForCheck(primary.devicePath) == preserved);

        MonitorDescriptor secondary = primary;
        secondary.primary = false;
        secondary.devicePath += L"#MonitorCoordinatorCheck";
        monitors.push_back(secondary);
        Require(coordinator.Reconcile(monitors));
        Require(coordinator.GetPipelineWindowForCheck(primary.devicePath) == preserved);
        const HWND secondaryWindow = coordinator.GetPipelineWindowForCheck(secondary.devicePath);
        Require(secondaryWindow && IsWindow(secondaryWindow));

        monitors.pop_back();
        Require(coordinator.Reconcile(monitors));
        Require(coordinator.GetPipelineWindowForCheck(primary.devicePath) == preserved);
        Require(!IsWindow(secondaryWindow));
        Require(coordinator.Update());

        const HWND oldRebuildWindow = preserved;
        monitors.front().bounds.right -= 2;
        Require(coordinator.Reconcile(monitors));
        const HWND rebuiltWindow = coordinator.GetPipelineWindowForCheck(primary.devicePath);
        Require(rebuiltWindow && rebuiltWindow != oldRebuildWindow && IsWindow(rebuiltWindow));
        Require(!IsWindow(oldRebuildWindow));

        const UINT windowsBeforeFailure = CountEngineWindows();
        auto failedTopology = monitors;
        failedTopology.front().bounds.right -= 2;
        MonitorDescriptor failedSecondary = failedTopology.front();
        failedSecondary.primary = false;
        failedSecondary.devicePath += L"#MonitorCoordinatorCheckFailure";
        failedTopology.push_back(failedSecondary);
        rejectSecondary = true;
        Require(!coordinator.Reconcile(failedTopology));
        Require(coordinator.GetPipelineWindowForCheck(primary.devicePath) == rebuiltWindow);
        Require(IsWindow(rebuiltWindow));
        Require(coordinator.GetPipelineWindowForCheck(failedSecondary.devicePath) == nullptr);
        Require(CountEngineWindows() == windowsBeforeFailure);

        coordinator.Stop();
        coordinator.Shutdown();
        Require(!IsWindow(rebuiltWindow));
        Require(CountEngineWindows() == 0);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MonitorCoordinatorCheck failed: %s\\n", error.what());
        return 1;
    }
}
