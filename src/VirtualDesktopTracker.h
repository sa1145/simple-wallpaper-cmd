#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

class VirtualDesktopTracker {
public:
    using ChangedCallback = std::function<void(const GUID& desktopId)>;

    struct Diagnostic {
        DWORD windowsBuild = 0;
        std::wstring interfaceRevision;
        GUID interfaceIid{};
        HRESULT hresult = S_OK;
        HRESULT unregisterHresult = S_OK;
        DWORD notificationCookie = 0;
    };

    VirtualDesktopTracker();
    ~VirtualDesktopTracker() noexcept;
    VirtualDesktopTracker(const VirtualDesktopTracker&) = delete;
    VirtualDesktopTracker& operator=(const VirtualDesktopTracker&) = delete;

    // false means the optional backend is disabled; callers must keep the
    // wallpaper engine running without virtual-desktop notifications.
    bool Start(ChangedCallback callback);
    bool PumpPending();
    void Stop() noexcept;

    [[nodiscard]] bool IsTracking() const noexcept;
    [[nodiscard]] GUID GetCurrentDesktopId() const noexcept;
    [[nodiscard]] const Diagnostic& GetDiagnostic() const noexcept;

private:
    void QueueDesktopChange(const GUID& desktopId) noexcept;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
