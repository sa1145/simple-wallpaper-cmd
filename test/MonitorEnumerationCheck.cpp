#include "MonitorEnumerator.h"

#include <exception>
#include <iostream>

int main() {
    try {
        const auto monitors = EnumerateActiveMonitors();
        if (monitors.empty()) {
            std::cerr << "No active monitors found\n";
            return 1;
        }

        for (const auto& monitor : monitors) {
            std::wcout << L"path=" << monitor.devicePath
                       << L" rect=" << monitor.bounds.left << L',' << monitor.bounds.top
                       << L',' << monitor.bounds.right << L',' << monitor.bounds.bottom
                       << L" primary=" << (monitor.primary ? L"true" : L"false") << L'\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
