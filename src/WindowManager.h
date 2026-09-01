#pragma once
// ============================================================================
// WindowManager.h — Module 1: Desktop-bottom borderless window
// ============================================================================
//
// 職責：建立永遠在桌面最底層的無邊框視窗
//
// Key behaviors:
//   - Registers a custom WNDCLASS and creates a popup window with:
//       WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT
//   - Positions at HWND_BOTTOM so it sits behind all desktop icons
//   - Supports multi-monitor: can target a specific monitor or primary
//   - Pumps the Win32 message loop (non-blocking PeekMessage variant)
//   - Provides HWND for DX12Context to create a SwapChain against
//
// Thread safety:
//   - Create() and Destroy() must be called from the same thread
//     (the "render thread" or main thread).
//   - PumpMessages() must be called from the creating thread.
// ============================================================================

#include "Common.h"

#include <functional>
#include <string>

class WindowManager {
public:
    // -----------------------------------------------------------------------
    // Configuration passed to Create()
    // -----------------------------------------------------------------------
    struct Config {
        uint32_t    width           = 1920;
        uint32_t    height          = 1080;
        int32_t     posX            = 0;        // top-left X on virtual screen
        int32_t     posY            = 0;        // top-left Y on virtual screen
        bool        autoDetectSize  = true;     // ignore width/height, use primary monitor
        std::wstring monitorDevicePath;          // persistent monitor identity
        std::wstring windowTitle    = L"DX12 Wallpaper Engine";
    };

    WindowManager()  = default;
    ~WindowManager();

    // Non-copyable, movable
    WindowManager(const WindowManager&)            = delete;
    WindowManager& operator=(const WindowManager&) = delete;
    WindowManager(WindowManager&& other) noexcept;
    WindowManager& operator=(WindowManager&& other) noexcept;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Registers the window class and creates the wallpaper window.
    /// Throws std::runtime_error on failure.
    void Create(const Config& cfg = {});

    /// Destroys the window. The process-wide class remains registered.
    void Destroy();

    // -----------------------------------------------------------------------
    // Message loop
    // -----------------------------------------------------------------------

    /// Processes pending Win32 messages (non-blocking).
    /// Returns false if WM_QUIT was received (app should exit).
    bool PumpMessages();

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    [[nodiscard]] HWND      GetHwnd()   const noexcept { return m_hwnd; }
    [[nodiscard]] uint32_t  GetWidth()  const noexcept { return m_width; }
    [[nodiscard]] uint32_t  GetHeight() const noexcept { return m_height; }
    [[nodiscard]] int32_t   GetPosX()   const noexcept { return m_posX; }
    [[nodiscard]] int32_t   GetPosY()   const noexcept { return m_posY; }
    [[nodiscard]] bool      IsAutoDetectSize() const noexcept { return m_autoDetectSize; }
    [[nodiscard]] const std::wstring& GetMonitorDevicePath() const noexcept {
        return m_monitorDevicePath;
    }
    [[nodiscard]] bool      IsValid()   const noexcept;
    [[nodiscard]] bool      IsAttachedToCurrentDesktopHost() const noexcept;

    // -----------------------------------------------------------------------
    // Callbacks (optional)
    // -----------------------------------------------------------------------
    using ResizeCallback = std::function<void(uint32_t newWidth, uint32_t newHeight)>;
    using DisplayChangeCallback = std::function<void()>;

    /// Register a callback that fires on WM_DISPLAYCHANGE (monitor resolution changed).
    void SetResizeCallback(ResizeCallback cb) { m_resizeCallback = std::move(cb); }
    void SetDisplayChangeCallback(DisplayChangeCallback cb) { m_displayChangeCallback = std::move(cb); }

private:
    // Win32 window procedure (static → instance dispatch via GWLP_USERDATA)
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // Helpers
    void DetectPrimaryMonitor(uint32_t& outW, uint32_t& outH,
                              int32_t& outX, int32_t& outY) const;
    void PositionAsBehindDesktop();
    void InjectBehindDesktopIcons();
    static HWND FindDesktopHost() noexcept;
    static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam);

    // State
    HWND        m_hwnd          = nullptr;
    HWND        m_desktopHost   = nullptr;
    HINSTANCE   m_hInstance     = nullptr;
    ATOM        m_classAtom     = 0;
    uint32_t    m_width         = 0;
    uint32_t    m_height        = 0;
    int32_t     m_posX          = 0;
    int32_t     m_posY          = 0;
    bool        m_autoDetectSize = true;
    std::wstring m_monitorDevicePath;
    bool        m_quitRequested = false;

    ResizeCallback m_resizeCallback;
    DisplayChangeCallback m_displayChangeCallback;

    static constexpr const wchar_t* CLASS_NAME = L"DX12WallpaperEngineClass";
};
