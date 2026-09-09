#include "MonitorEnumerator.h"
#include "WindowManager.h"

#include <exception>
#include <iostream>
#include <utility>

namespace {

bool Matches(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top
        && left.right == right.right && left.bottom == right.bottom;
}

WindowManager::Config TargetConfig(const MonitorDescriptor& monitor) {
    WindowManager::Config config;
    config.width = static_cast<uint32_t>(monitor.bounds.right - monitor.bounds.left);
    config.height = static_cast<uint32_t>(monitor.bounds.bottom - monitor.bounds.top);
    config.posX = monitor.bounds.left;
    config.posY = monitor.bounds.top;
    config.autoDetectSize = false;
    config.monitorDevicePath = monitor.devicePath;
    return config;
}

void CheckTarget(const WindowManager& window, const MonitorDescriptor& monitor) {
    RECT rect{};
    HWND host = GetParent(window.GetHwnd());
    wchar_t hostClass[256]{};
    RECT hostRect{};
    DWORD shellProcessId = 0;
    DWORD hostProcessId = 0;
    GetWindowThreadProcessId(GetShellWindow(), &shellProcessId);
    GetWindowThreadProcessId(host, &hostProcessId);

    if (!host || !GetClassNameW(host, hostClass, 256)
        || lstrcmpW(hostClass, L"WorkerW") != 0
        || hostProcessId != shellProcessId
        || !GetWindowRect(host, &hostRect)
        || hostRect.left > monitor.bounds.left || hostRect.top > monitor.bounds.top
        || hostRect.right < monitor.bounds.right || hostRect.bottom < monitor.bounds.bottom
        || !GetWindowRect(window.GetHwnd(), &rect) || !Matches(rect, monitor.bounds)
        || window.GetMonitorDevicePath() != monitor.devicePath
        || window.IsAutoDetectSize()
        || window.GetPosX() != monitor.bounds.left || window.GetPosY() != monitor.bounds.top) {
        throw std::runtime_error("WindowManager target state does not match monitor");
    }
}

} // namespace

int main() {
    try {
        HWND shell = GetShellWindow();
        wchar_t shellClass[256]{};
        if (!shell || !GetClassNameW(shell, shellClass, 256)
            || lstrcmpW(shellClass, L"Progman") != 0) {
            std::cerr << "SKIP: interactive Explorer desktop is unavailable\n";
            return 77;
        }

        const auto monitors = EnumerateActiveMonitors();
        if (monitors.empty()) {
            throw std::runtime_error("No active monitors found");
        }

        const MonitorDescriptor& firstMonitor = monitors.front();
        const MonitorDescriptor& secondMonitor = monitors.size() > 1 ? monitors[1] : firstMonitor;
        WindowManager first;
        first.Create(TargetConfig(firstMonitor));
        WindowManager moved = std::move(first);
        WindowManager second;
        second.Create(TargetConfig(secondMonitor));
        WindowManager assigned;
        assigned = std::move(moved);

        CheckTarget(assigned, firstMonitor);
        CheckTarget(second, secondMonitor);
        if (!assigned.IsValid() || !second.IsValid()
            || !assigned.IsAttachedToCurrentDesktopHost() || !second.IsAttachedToCurrentDesktopHost()
            || GetParent(assigned.GetHwnd()) != GetParent(second.GetHwnd())) {
            throw std::runtime_error("WindowManager instances do not share the current desktop host");
        }
        SendMessageW(assigned.GetHwnd(), WM_DISPLAYCHANGE, 0, 0);
        SendMessageW(second.GetHwnd(), WM_DISPLAYCHANGE, 0, 0);
        CheckTarget(assigned, firstMonitor);
        CheckTarget(second, secondMonitor);

        const HWND firstHwnd = assigned.GetHwnd();
        const HWND secondHwnd = second.GetHwnd();
        assigned.Destroy();
        second.Destroy();
        if (IsWindow(firstHwnd) || IsWindow(secondHwnd)) {
            throw std::runtime_error("WindowManager left a HWND behind");
        }
        std::cout << (monitors.size() > 1 ? "PASS: two-monitor desktop host stability\n"
                                          : "PASS: single-monitor desktop host reuse\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
