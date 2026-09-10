#include "MonitorCoordinator.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <thread>

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

LONG Width(const RECT& bounds) { return bounds.right - bounds.left; }
LONG Height(const RECT& bounds) { return bounds.bottom - bounds.top; }

void RequirePipelineTarget(HWND window, const MonitorDescriptor& monitor, HWND desktopHost) {
    Require(window && IsWindow(window));
    RECT bounds{};
    Require(GetWindowRect(window, &bounds));
    Require(bounds.left == monitor.bounds.left && bounds.top == monitor.bounds.top
        && bounds.right == monitor.bounds.right && bounds.bottom == monitor.bounds.bottom);
    const HWND parent = GetParent(window);
    wchar_t className[64]{};
    Require(parent && GetClassNameW(parent, className, static_cast<int>(std::size(className)))
        && wcscmp(className, L"WorkerW") == 0);
    Require(!desktopHost || parent == desktopHost);
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

        const auto monitors = EnumerateActiveMonitors();
        Require(monitors.size() == 2);
        const auto primary = std::find_if(monitors.begin(), monitors.end(),
            [](const MonitorDescriptor& monitor) { return monitor.primary; });
        const auto secondary = std::find_if(monitors.begin(), monitors.end(),
            [](const MonitorDescriptor& monitor) { return !monitor.primary; });
        Require(primary != monitors.end() && secondary != monitors.end());
        Require(!primary->devicePath.empty() && !secondary->devicePath.empty());
        Require(CompareStringOrdinal(primary->devicePath.c_str(), -1,
                                     secondary->devicePath.c_str(), -1, TRUE) != CSTR_EQUAL);
        Require(Width(primary->bounds) == 1920 && Height(primary->bounds) == 1080);
        Require(Width(secondary->bounds) == 2400 && Height(secondary->bounds) == 1080);

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

        const HWND primaryWindow = coordinator.GetPipelineWindowForCheck(primary->devicePath);
        const HWND secondaryWindow = coordinator.GetPipelineWindowForCheck(secondary->devicePath);
        RequirePipelineTarget(primaryWindow, *primary, nullptr);
        const HWND desktopHost = GetParent(primaryWindow);
        RequirePipelineTarget(secondaryWindow, *secondary, desktopHost);
        std::printf("[MonitorCoordinatorCheck] two-monitor baseline: primary=%ls %ldx%ld, "
                    "secondary=%ls %ldx%ld, host=%p\n",
                    primary->devicePath.c_str(), Width(primary->bounds), Height(primary->bounds),
                    secondary->devicePath.c_str(), Width(secondary->bounds), Height(secondary->bounds),
                    desktopHost);

        auto transientIdentity = monitors;
        for (size_t index = 0; index < transientIdentity.size(); ++index) {
            for (wchar_t& character : transientIdentity[index].devicePath)
                character = static_cast<wchar_t>(std::towupper(character));
            transientIdentity[index].gdiDeviceName = L"transient-gdi-name";
            transientIdentity[index].monitor = reinterpret_cast<HMONITOR>(
                static_cast<INT_PTR>(index + 1));
        }
        Require(coordinator.Reconcile(transientIdentity));
        Require(coordinator.GetPipelineWindowForCheck(primary->devicePath) == primaryWindow);
        Require(coordinator.GetPipelineWindowForCheck(secondary->devicePath) == secondaryWindow);

        Require(coordinator.Reconcile({*primary}));
        Require(coordinator.GetPipelineWindowForCheck(primary->devicePath) == primaryWindow);
        Require(!IsWindow(secondaryWindow));
        Require(coordinator.GetPipelineWindowForCheck(secondary->devicePath) == nullptr);

        Require(coordinator.Reconcile(monitors));
        const HWND addedSecondaryWindow = coordinator.GetPipelineWindowForCheck(secondary->devicePath);
        Require(coordinator.GetPipelineWindowForCheck(primary->devicePath) == primaryWindow);
        RequirePipelineTarget(addedSecondaryWindow, *secondary, desktopHost);

        auto resizedTopology = monitors;
        auto resizedSecondary = std::find_if(resizedTopology.begin(), resizedTopology.end(),
            [](const MonitorDescriptor& monitor) { return !monitor.primary; });
        Require(resizedSecondary != resizedTopology.end());
        --resizedSecondary->bounds.right;
        Require(coordinator.Reconcile(resizedTopology));
        const HWND resizedSecondaryWindow = coordinator.GetPipelineWindowForCheck(secondary->devicePath);
        Require(coordinator.GetPipelineWindowForCheck(primary->devicePath) == primaryWindow);
        Require(resizedSecondaryWindow != addedSecondaryWindow);
        Require(!IsWindow(addedSecondaryWindow));
        RequirePipelineTarget(resizedSecondaryWindow, *resizedSecondary, desktopHost);

        const UINT windowsBeforeFailure = CountEngineWindows();
        auto failedTopology = resizedTopology;
        const auto failedSecondary = std::find_if(failedTopology.begin(), failedTopology.end(),
            [](const MonitorDescriptor& monitor) { return !monitor.primary; });
        Require(failedSecondary != failedTopology.end());
        --failedSecondary->bounds.right;
        rejectSecondary = true;
        Require(!coordinator.Reconcile(failedTopology));
        rejectSecondary = false;
        Require(coordinator.GetPipelineWindowForCheck(primary->devicePath) == primaryWindow);
        Require(coordinator.GetPipelineWindowForCheck(secondary->devicePath) == resizedSecondaryWindow);
        Require(IsWindow(primaryWindow) && IsWindow(resizedSecondaryWindow));
        Require(CountEngineWindows() == windowsBeforeFailure);

        bool nonOwnerRejected = false;
        std::thread nonOwner([&] {
            try {
                coordinator.Reconcile(resizedTopology);
            } catch (const std::runtime_error&) {
                nonOwnerRejected = true;
            }
        });
        nonOwner.join();
        Require(nonOwnerRejected);
        Require(coordinator.GetPipelineWindowForCheck(primary->devicePath) == primaryWindow);
        Require(coordinator.GetPipelineWindowForCheck(secondary->devicePath) == resizedSecondaryWindow);
        Require(CountEngineWindows() == windowsBeforeFailure);
        Require(coordinator.Update());

        coordinator.Stop();
        coordinator.Shutdown();
        Require(CountEngineWindows() == 0);
        std::printf("[MonitorCoordinatorCheck] two-monitor reconciliation PASS\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MonitorCoordinatorCheck failed: %s\\n", error.what());
        return 1;
    }
}
