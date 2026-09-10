#include "TrayIcon.h"

#include <cstdio>
#include <shellapi.h>
#include <utility>

namespace {
constexpr wchar_t kWindowClass[] = L"DX12WallpaperEngineTrayClass";
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kSettingsCommand = 1;
constexpr UINT kChangeWallpaperCommand = 2;
constexpr UINT kExitCommand = 3;
}

TrayIcon::~TrayIcon() { Shutdown(); }

bool TrayIcon::Initialize(HINSTANCE instance, ExitCallback exitCallback) {
    if (m_initialized) return true;

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::fprintf(stderr, "[TrayIcon] RegisterClassExW failed: %lu\n", GetLastError());
        return false;
    }

    m_exitCallback = std::move(exitCallback);
    m_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    if (!m_taskbarCreatedMessage)
        std::fprintf(stderr, "[TrayIcon] RegisterWindowMessageW failed: %lu\n", GetLastError());
    m_window = CreateWindowExW(0, kWindowClass, L"", WS_OVERLAPPED,
                               0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!m_window) {
        std::fprintf(stderr, "[TrayIcon] CreateWindowExW failed: %lu\n", GetLastError());
        m_exitCallback = {};
        return false;
    }

    m_initialized = true;
    AddIcon();
    return true;
}

void TrayIcon::Shutdown() noexcept {
    if (!m_initialized) return;
    if (m_iconAdded) {
        NOTIFYICONDATAW icon{sizeof(icon)};
        icon.hWnd = m_window;
        icon.uID = kTrayIconId;
        if (!Shell_NotifyIconW(NIM_DELETE, &icon))
            std::fprintf(stderr, "[TrayIcon] NIM_DELETE failed: %lu\n", GetLastError());
        m_iconAdded = false;
    }
    if (m_window && !DestroyWindow(m_window))
        std::fprintf(stderr, "[TrayIcon] DestroyWindow failed: %lu\n", GetLastError());
    m_window = nullptr;
    m_exitCallback = {};
    m_initialized = false;
}

bool TrayIcon::AddIcon() noexcept {
    if (!m_window) return false;
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = m_window;
    icon.uID = kTrayIconId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = kTrayCallbackMessage;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(icon.szTip, L"DX12 Wallpaper Engine", _TRUNCATE);
    if (Shell_NotifyIconW(NIM_ADD, &icon)) {
        m_iconAdded = true;
    } else {
        std::fprintf(stderr, "[TrayIcon] NIM_ADD failed: %lu\n", GetLastError());
    }
    return m_iconAdded;
}

void TrayIcon::ShowMenu() noexcept {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kSettingsCommand, L"Settings");
    AppendMenuW(menu, MF_STRING, kChangeWallpaperCommand, L"Change Wallpaper");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitCommand, L"Exit");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(m_window);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                        point.x, point.y, 0, m_window, nullptr);
    DestroyMenu(menu);
    if (command == kExitCommand && m_exitCallback) m_exitCallback();
}

LRESULT TrayIcon::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (m_taskbarCreatedMessage && message == m_taskbarCreatedMessage && m_initialized) {
        AddIcon();
        return 0;
    }
    if (message == kTrayCallbackMessage && (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)) {
        ShowMenu();
        return 0;
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        m_window = nullptr;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK TrayIcon::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* tray = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return tray ? tray->HandleMessage(window, message, wParam, lParam)
                : DefWindowProcW(window, message, wParam, lParam);
}
