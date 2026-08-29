#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct MonitorDescriptor {
    std::wstring devicePath;
    std::wstring gdiDeviceName;
    HMONITOR monitor = nullptr;
    RECT bounds{};
    LUID adapterId{};
    UINT32 targetId = 0;
    bool primary = false;
};

[[nodiscard]] std::vector<MonitorDescriptor> EnumerateActiveMonitors();
