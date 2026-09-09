// ============================================================================
// WindowManager.cpp — Module 1: Desktop-bottom borderless window
// ============================================================================

#include "WindowManager.h"

#include <cassert>
#include <stdexcept>
#include <utility>

namespace {

struct DesktopHostSearch {
    RECT desktopRect{};
    DWORD shellProcessId = 0;
    HWND host = nullptr;
};

bool IsMatchingWorker(HWND hwnd, const DesktopHostSearch& search) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    return processId == search.shellProcessId;
}

void RecordCandidate(DesktopHostSearch& search, HWND candidate) {
    if (!IsMatchingWorker(candidate, search)) return;
    search.host = candidate;
}

} // namespace

HWND WindowManager::FindDesktopHost() noexcept {
    HWND progman = GetShellWindow();
    if (!progman) progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;

    DWORD shellPid = 0;
    GetWindowThreadProcessId(progman, &shellPid);

    // 1. Check if Progman has a direct child WorkerW
    for (HWND worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
         worker;
         worker = FindWindowExW(progman, worker, L"WorkerW", nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(worker, &pid);
        if (pid == shellPid) {
            return worker;
        }
    }

    // 2. Check top-level WorkerW windows directly behind Progman in Z-order
    for (HWND worker = FindWindowExW(nullptr, progman, L"WorkerW", nullptr);
         worker;
         worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(worker, &pid);
        if (pid == shellPid) {
            return worker;
        }
    }

    // 3. Fallback: Search top-level WorkerW windows that contain SHELLDLL_DefView
    HWND fallbackHost = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        wchar_t className[256]{};
        if (GetClassNameW(hwnd, className, 256) && lstrcmpW(className, L"WorkerW") == 0) {
            HWND defView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
            if (defView != nullptr) {
                HWND targetWorker = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr);
                if (targetWorker != nullptr) {
                    *reinterpret_cast<HWND*>(lParam) = targetWorker;
                    return FALSE;
                }
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&fallbackHost));

    return fallbackHost;
}

// ============================================================================
// Destructor
// ============================================================================

WindowManager::~WindowManager() {
    Destroy();
}

// ============================================================================
// Move semantics
// ============================================================================

WindowManager::WindowManager(WindowManager&& other) noexcept
    : m_hwnd(other.m_hwnd)
    , m_desktopHost(other.m_desktopHost)
    , m_height(other.m_height)
    , m_posX(other.m_posX)
    , m_posY(other.m_posY)
    , m_autoDetectSize(other.m_autoDetectSize)
    , m_monitorDevicePath(std::move(other.m_monitorDevicePath))
    , m_quitRequested(other.m_quitRequested)
    , m_resizeCallback(std::move(other.m_resizeCallback))
    , m_displayChangeCallback(std::move(other.m_displayChangeCallback))
{
    if (m_hwnd) {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    other.m_hwnd      = nullptr;
    other.m_desktopHost = nullptr;
    other.m_classAtom = 0;
}

WindowManager& WindowManager::operator=(WindowManager&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_hwnd            = other.m_hwnd;
        m_desktopHost     = other.m_desktopHost;
        m_hInstance       = other.m_hInstance;
        m_classAtom       = other.m_classAtom;
        m_width           = other.m_width;
        m_height          = other.m_height;
        m_posX            = other.m_posX;
        m_posY            = other.m_posY;
        m_autoDetectSize  = other.m_autoDetectSize;
        m_monitorDevicePath = std::move(other.m_monitorDevicePath);
        m_quitRequested   = other.m_quitRequested;
        m_resizeCallback  = std::move(other.m_resizeCallback);
        m_displayChangeCallback = std::move(other.m_displayChangeCallback);

        if (m_hwnd) {
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        }

        other.m_hwnd      = nullptr;
        other.m_desktopHost = nullptr;
        other.m_classAtom = 0;
    }
    return *this;
}

// ============================================================================
// Create
// ============================================================================

void WindowManager::Create(const Config& cfg) {
    if (m_hwnd) {
        throw std::runtime_error("WindowManager::Create — window already exists");
    }
    if (cfg.width == 0 || cfg.height == 0) {
        throw std::invalid_argument("WindowManager::Create — window dimensions must be non-zero");
    }

    m_hInstance = GetModuleHandleW(nullptr);
    if (!m_hInstance) {
        throw std::runtime_error("WindowManager::Create — GetModuleHandle failed");
    }

    // ----- Register window class -----
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = sizeof(void*);          // space for GWLP_USERDATA
    wc.hInstance     = m_hInstance;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszMenuName  = nullptr;
    wc.lpszClassName = CLASS_NAME;
    wc.hIconSm       = nullptr;

    m_classAtom = RegisterClassExW(&wc);
    if (!m_classAtom) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("WindowManager::Create — RegisterClassExW failed");
        }
        m_classAtom = 1;
    }

    // ----- Determine window dimensions -----
    uint32_t w = cfg.width;
    uint32_t h = cfg.height;
    int32_t  x = cfg.posX;
    int32_t  y = cfg.posY;

    if (cfg.autoDetectSize) {
        DetectPrimaryMonitor(w, h, x, y);
    }

    m_width  = w;
    m_height = h;
    m_posX = x;
    m_posY = y;
    m_autoDetectSize = cfg.autoDetectSize;
    m_monitorDevicePath = cfg.monitorDevicePath;

    // ----- Extended styles per spec -----
    // WS_EX_NOACTIVATE  — never steals focus
    // WS_EX_TOOLWINDOW  — hidden from Alt-Tab and taskbar
    constexpr DWORD exStyle = WS_EX_NOACTIVATE
                            | WS_EX_TOOLWINDOW
                            | WS_EX_TRANSPARENT;

    // WS_POPUP — borderless, no title bar
    constexpr DWORD style = WS_POPUP;

    // ----- Create the window -----
    m_hwnd = CreateWindowExW(
        exStyle,
        CLASS_NAME,
        cfg.windowTitle.c_str(),
        style,
        x, y,
        static_cast<int>(w),
        static_cast<int>(h),
        nullptr,            // no parent
        nullptr,            // no menu
        m_hInstance,
        this                // pass 'this' via lpParam → WM_NCCREATE
    );

    if (!m_hwnd) {
        DWORD err = GetLastError();
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "WindowManager::Create — CreateWindowExW failed (0x%08lX)", err);
        throw std::runtime_error(msg);
    }

    try {
        InjectBehindDesktopIcons();
    } catch (...) {
        Destroy();
        throw;
    }

    // Show without activating
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_hwnd);
}

// ============================================================================
// Destroy
// ============================================================================

void WindowManager::Destroy() {
    const HWND hwnd = std::exchange(m_hwnd, nullptr);
    m_desktopHost = nullptr;
    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    m_classAtom = 0;
}

bool WindowManager::IsValid() const noexcept {
    return m_hwnd && IsWindow(m_hwnd) && m_desktopHost && IsWindow(m_desktopHost)
        && GetParent(m_hwnd) == m_desktopHost;
}

bool WindowManager::IsAttachedToCurrentDesktopHost() const noexcept {
    return IsValid() && m_desktopHost == FindDesktopHost();
}

// ============================================================================
// PumpMessages  (non-blocking)
// ============================================================================

bool WindowManager::PumpMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            m_quitRequested = true;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !m_quitRequested;
}

// ============================================================================
// WndProc — static → instance dispatch
// ============================================================================

LRESULT CALLBACK WindowManager::WndProc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp) {
    WindowManager* self = nullptr;

    if (msg == WM_NCCREATE) {
        // Store 'this' pointer from CreateWindowExW's lpParam
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<WindowManager*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<WindowManager*>(
                   GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================================
// HandleMessage — instance-level message handling
// ============================================================================

LRESULT WindowManager::HandleMessage(HWND hwnd, UINT msg,
                                      WPARAM wp, LPARAM lp) {
    switch (msg) {

    // --- Prevent the window from ever being activated / gaining focus ---
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ACTIVATE:
        // If someone tries to activate us, push back to bottom
        if (LOWORD(wp) != WA_INACTIVE) {
            PositionAsBehindDesktop();
        }
        return 0;

    // Reconciliation owns all child HWND resize/rebuild work on the owner thread.
    case WM_DISPLAYCHANGE: {
        if (m_displayChangeCallback) m_displayChangeCallback();
        return 0;
    }

    // --- If something covers us, try to stay at the very bottom ---
    case WM_WINDOWPOSCHANGING: {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        pos->hwndInsertAfter = HWND_BOTTOM;
        pos->flags |= SWP_NOACTIVATE;
        return 0;
    }

    case WM_NCDESTROY:
        if (m_hwnd == hwnd) {
            m_hwnd = nullptr;
            m_desktopHost = nullptr;
            if (m_displayChangeCallback) m_displayChangeCallback();
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================================
// DetectPrimaryMonitor — queries the primary monitor work area
// ============================================================================

void WindowManager::DetectPrimaryMonitor(uint32_t& outW, uint32_t& outH,
                                          int32_t& outX, int32_t& outY) const {
    // Use full screen rect (not work area) — wallpaper covers entire monitor
    // including the taskbar region.
    HMONITOR hMon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);

    if (GetMonitorInfoW(hMon, &mi)) {
        outX = mi.rcMonitor.left;
        outY = mi.rcMonitor.top;
        outW = static_cast<uint32_t>(mi.rcMonitor.right  - mi.rcMonitor.left);
        outH = static_cast<uint32_t>(mi.rcMonitor.bottom - mi.rcMonitor.top);
    } else {
        // Fallback: use system metrics
        outX = 0;
        outY = 0;
        outW = static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN));
        outH = static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN));
    }
}

// ============================================================================
// PositionAsBehindDesktop — HWND_BOTTOM placement (core wallpaper trick)
// ============================================================================

void WindowManager::PositionAsBehindDesktop() {
    assert(m_hwnd && "PositionAsBehindDesktop called with null HWND");

    SetWindowPos(
        m_hwnd,
        HWND_BOTTOM,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW
    );
}

// ============================================================================
// InjectBehindDesktopIcons
// ============================================================================

void WindowManager::InjectBehindDesktopIcons() {
    assert(m_hwnd && "InjectBehindDesktopIcons called with null HWND");

    HWND progman = GetShellWindow();
    if (!progman) progman = FindWindowW(L"Progman", nullptr);
    if (!progman) {
        throw std::runtime_error("WindowManager::Create — desktop host not found");
    }

    HWND desktopHost = FindDesktopHost();
    if (!desktopHost) {
        // Force Windows to spawn the WorkerW background window when recovery needs one.
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(progman, 0x052C, 0x0000000D, 0, SMTO_NORMAL, 1000, &ignored);
        SendMessageTimeoutW(progman, 0x052C, 0x0000000D, 1, SMTO_NORMAL, 1000, &ignored);
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &ignored);
        desktopHost = FindDesktopHost();
    }
    if (!desktopHost) {
        throw std::runtime_error("WindowManager::Create — wallpaper host not found");
    }

    LONG_PTR style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    SetWindowLongPtrW(m_hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD);
    SetParent(m_hwnd, desktopHost);
    if (GetParent(m_hwnd) != desktopHost) {
        throw std::runtime_error("WindowManager::Create — failed to attach wallpaper to desktop");
    }
    m_desktopHost = desktopHost;

    POINT childPos{m_posX, m_posY};
    MapWindowPoints(HWND_DESKTOP, desktopHost, &childPos, 1);
    SetWindowPos(
        m_hwnd,
        HWND_BOTTOM,
        childPos.x, childPos.y,
        static_cast<int>(m_width),
        static_cast<int>(m_height),
        SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW
    );
}

// ============================================================================
// EnumWindowsProc
// ============================================================================

BOOL CALLBACK WindowManager::EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    wchar_t ownerClass[256]{};
    if (!GetClassNameW(hwnd, ownerClass, 256)
        || (lstrcmpW(ownerClass, L"Progman") != 0
            && lstrcmpW(ownerClass, L"WorkerW") != 0)) {
        return TRUE;
    }

    BOOL hasDefView = FALSE;
    EnumChildWindows(hwnd, [](HWND child, LPARAM data) -> BOOL {
        wchar_t className[256];
        if (GetClassNameW(child, className, 256)
            && lstrcmpW(className, L"SHELLDLL_DefView") == 0) {
            *reinterpret_cast<BOOL*>(data) = TRUE;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hasDefView));

    if (hasDefView) {
        auto& search = *reinterpret_cast<DesktopHostSearch*>(lParam);
        for (HWND target = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr);
             target;
             target = FindWindowExW(nullptr, target, L"WorkerW", nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(target, &pid);
            if (pid == search.shellProcessId) {
                search.host = target;
                return FALSE;
            }
        }
    }
    return TRUE;
}
