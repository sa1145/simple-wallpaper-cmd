#pragma once

#include <functional>
#include <windows.h>

class TrayIcon {
public:
    using ExitCallback = std::function<void()>;

    TrayIcon() = default;
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // The hidden owner window and icon stay on the caller's message-pump thread.
    bool Initialize(HINSTANCE instance, ExitCallback exitCallback);
    void Shutdown() noexcept;

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool AddIcon() noexcept;
    void ShowMenu() noexcept;

    HWND m_window = nullptr;
    UINT m_taskbarCreatedMessage = 0;
    bool m_initialized = false;
    bool m_iconAdded = false;
    ExitCallback m_exitCallback;
};
