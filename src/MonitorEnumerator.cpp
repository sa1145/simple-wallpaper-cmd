#include "MonitorEnumerator.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

[[noreturn]] void ThrowWin32(const char* api, LONG error) {
    throw std::runtime_error(std::string(api) + " failed (" + std::to_string(error) + ')');
}

struct GdiMonitor {
    std::wstring deviceName;
    HMONITOR monitor = nullptr;
    RECT bounds{};
    bool primary = false;
};

struct GdiMonitors {
    std::vector<GdiMonitor> monitors;
    DWORD error = ERROR_SUCCESS;
};

BOOL CALLBACK CollectGdiMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto& result = *reinterpret_cast<GdiMonitors*>(parameter);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        result.error = GetLastError();
        return FALSE;
    }

    result.monitors.push_back({info.szDevice, monitor, info.rcMonitor,
                               (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

bool IsValidRect(const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool IsOrdinalLess(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                right.c_str(), static_cast<int>(right.size()), FALSE)
        == CSTR_LESS_THAN;
}

} // namespace

std::vector<MonitorDescriptor> EnumerateActiveMonitors() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    bool queried = false;
    for (int attempt = 0; attempt != 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS) {
            ThrowWin32("GetDisplayConfigBufferSizes", result);
        }

        paths.resize(pathCount);
        modes.resize(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                                    &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (result != ERROR_SUCCESS) {
            ThrowWin32("QueryDisplayConfig", result);
        }
        paths.resize(pathCount);
        queried = true;
        break;
    }
    if (!queried) {
        ThrowWin32("QueryDisplayConfig", ERROR_INSUFFICIENT_BUFFER);
    }

    GdiMonitors gdiMonitors;
    if (!EnumDisplayMonitors(nullptr, nullptr, CollectGdiMonitor,
                             reinterpret_cast<LPARAM>(&gdiMonitors))) {
        DWORD error = gdiMonitors.error == ERROR_SUCCESS ? GetLastError() : gdiMonitors.error;
        ThrowWin32("EnumDisplayMonitors", error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
    }

    std::vector<MonitorDescriptor> monitors;
    monitors.reserve(paths.size());
    std::unordered_set<std::wstring> devicePaths;
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id = path.targetInfo.id;
        LONG result = DisplayConfigGetDeviceInfo(&targetName.header);
        if (result != ERROR_SUCCESS) {
            ThrowWin32("DisplayConfigGetDeviceInfo(target)", result);
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        result = DisplayConfigGetDeviceInfo(&sourceName.header);
        if (result != ERROR_SUCCESS) {
            ThrowWin32("DisplayConfigGetDeviceInfo(source)", result);
        }

        std::wstring devicePath = targetName.monitorDevicePath;
        std::wstring gdiDeviceName = sourceName.viewGdiDeviceName;
        if (devicePath.empty() || !devicePaths.insert(devicePath).second) {
            throw std::runtime_error("DisplayConfig monitor device path is empty or duplicated");
        }

        const auto gdiMonitor = std::find_if(gdiMonitors.monitors.begin(), gdiMonitors.monitors.end(),
            [&gdiDeviceName](const GdiMonitor& monitor) { return monitor.deviceName == gdiDeviceName; });
        if (gdiDeviceName.empty() || gdiMonitor == gdiMonitors.monitors.end()) {
            throw std::runtime_error("Active display path has no matching GDI monitor");
        }
        if (!IsValidRect(gdiMonitor->bounds)) {
            throw std::runtime_error("GDI monitor has invalid bounds");
        }

        monitors.push_back({std::move(devicePath), std::move(gdiDeviceName), gdiMonitor->monitor,
                            gdiMonitor->bounds, path.targetInfo.adapterId, path.targetInfo.id,
                            gdiMonitor->primary});
    }

    const auto primaryCount = std::count_if(monitors.begin(), monitors.end(),
        [](const MonitorDescriptor& monitor) { return monitor.primary; });
    if (primaryCount != 1) {
        throw std::runtime_error("Active monitor list must contain exactly one primary monitor");
    }

    std::sort(monitors.begin(), monitors.end(), [](const MonitorDescriptor& left,
                                                    const MonitorDescriptor& right) {
        return left.primary != right.primary ? left.primary : IsOrdinalLess(left.devicePath, right.devicePath);
    });
    return monitors;
}
