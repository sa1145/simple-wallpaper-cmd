#include "VirtualDesktopTracker.h"

#include <cstdio>

namespace {

void PrintGuid(const GUID& guid) {
    std::wprintf(L"{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
                 static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3,
                 guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                 guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

bool IsZeroGuid(const GUID& guid) {
    return InlineIsEqualGUID(guid, GUID{});
}

void PrintStop(const VirtualDesktopTracker& tracker) {
    const auto& diagnostic = tracker.GetDiagnostic();
    std::wprintf(L"STOP tracking=%ls cookie=%lu unregister_hr=0x%08lX hr=0x%08lX\n",
                 tracker.IsTracking() ? L"true" : L"false", diagnostic.notificationCookie,
                 static_cast<unsigned long>(diagnostic.unregisterHresult),
                 static_cast<unsigned long>(diagnostic.hresult));
}

} // namespace

int wmain() {
    VirtualDesktopTracker tracker;
    const DWORD ownerThreadId = GetCurrentThreadId();
    GUID previousDesktopId{};
    unsigned int eventCount = 0;
    bool callbackFailed = false;
    const bool started = tracker.Start([&](const GUID& desktopId) {
        const DWORD dispatchThreadId = GetCurrentThreadId();
        if (dispatchThreadId != ownerThreadId || IsZeroGuid(desktopId) ||
            InlineIsEqualGUID(previousDesktopId, desktopId)) {
            callbackFailed = true;
            return;
        }
        previousDesktopId = desktopId;
        ++eventCount;
        std::wprintf(L"EVENT index=%u owner=%lu dispatch=%lu guid=", eventCount,
                     ownerThreadId, dispatchThreadId);
        PrintGuid(desktopId);
        std::wprintf(L"\n");
    });
    const auto& diagnostic = tracker.GetDiagnostic();
    if (!started || !tracker.IsTracking() || IsZeroGuid(tracker.GetCurrentDesktopId())) {
        std::wprintf(L"DISABLED build=%lu revision=%ls iid=", diagnostic.windowsBuild,
                     diagnostic.interfaceRevision.c_str());
        PrintGuid(diagnostic.interfaceIid);
        std::wprintf(L" hr=0x%08lX\n", static_cast<unsigned long>(diagnostic.hresult));
        tracker.Stop();
        PrintStop(tracker);
        return 2;
    }

    previousDesktopId = tracker.GetCurrentDesktopId();
    std::wprintf(L"READY owner=%lu initial=", ownerThreadId);
    PrintGuid(previousDesktopId);
    std::wprintf(L"\n");

    constexpr unsigned int kExpectedEvents = 20;
    constexpr ULONGLONG kTimeoutMs = 120000;
    const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
    while (eventCount < kExpectedEvents && !callbackFailed && GetTickCount64() < deadline) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        while (tracker.PumpPending() && !callbackFailed) {}
        if (eventCount == kExpectedEvents || callbackFailed) break;

        const ULONGLONG now = GetTickCount64();
        const DWORD waitMs = static_cast<DWORD>(
            (deadline - now) < 250 ? (deadline - now) : 250);
        if (MsgWaitForMultipleObjectsEx(0, nullptr, waitMs, QS_ALLINPUT,
                                        MWMO_INPUTAVAILABLE) == WAIT_FAILED) {
            callbackFailed = true;
        }
    }

    const int exitCode = callbackFailed ? 3 : eventCount == kExpectedEvents ? 0 : 4;
    if (exitCode != 0) {
        std::wprintf(L"TIMEOUT received=%u expected=%u callback_failed=%ls\n", eventCount,
                     kExpectedEvents, callbackFailed ? L"true" : L"false");
    }
    tracker.Stop();
    PrintStop(tracker);
    return exitCode;
}
