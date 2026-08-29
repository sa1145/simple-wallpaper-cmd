#include "WallpaperEngine.h"

#include <chrono>
#include <cstdio>
#include <dwmapi.h>
#include <stdexcept>
#include <thread>

namespace {
bool IsDwmCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

bool HasOtherMaximizedWindow() {
    bool found = false;
    EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (processId != GetCurrentProcessId() && IsWindowVisible(hwnd) && !IsIconic(hwnd)
            && !IsDwmCloaked(hwnd) && GetWindowPlacement(hwnd, &placement)
            && placement.showCmd == SW_SHOWMAXIMIZED) {
            *reinterpret_cast<bool*>(parameter) = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}
}

WallpaperEngine::~WallpaperEngine() { try { Shutdown(); } catch (...) {} }

void WallpaperEngine::Initialize(const Config& config) {
    if (m_initialized || config.videoPath.empty())
        throw std::runtime_error("WallpaperEngine::Initialize — invalid state or video path");
    std::unique_ptr<WallpaperConfig> wallpaperConfig;
    GUID desktopId{};
    if (!config.configPath.empty()) {
        wallpaperConfig = std::make_unique<WallpaperConfig>();
        wallpaperConfig->Load(config.configPath);
        if (!m_desktopTracker.Start([this](const GUID& changedDesktopId) {
                m_coordinator.SetCurrentDesktopId(changedDesktopId);
            })) {
            const auto& diagnostic = m_desktopTracker.GetDiagnostic();
            std::printf("[WallpaperEngine] Virtual desktop tracking disabled: build %lu, %ls, HRESULT 0x%08lX\n",
                        diagnostic.windowsBuild, diagnostic.interfaceRevision.c_str(),
                        static_cast<unsigned long>(diagnostic.hresult));
        } else {
            desktopId = m_desktopTracker.GetCurrentDesktopId();
        }
    }
    MonitorCoordinator::Config coordinatorConfig;
    coordinatorConfig.videoPath = config.videoPath;
    coordinatorConfig.targetFPS = config.targetFPS == 0 || config.targetFPS > 60 ? 60 : config.targetFPS;
    coordinatorConfig.enableDebugLayer = config.enableDebugLayer;
    coordinatorConfig.autoDetectSize = config.autoDetectSize;
    coordinatorConfig.manualWidth = config.windowWidth;
    coordinatorConfig.manualHeight = config.windowHeight;
    coordinatorConfig.wallpaperConfig = wallpaperConfig.get();
    coordinatorConfig.desktopId = desktopId;
    try {
        m_coordinator.Initialize(coordinatorConfig);
        m_wallpaperConfig = std::move(wallpaperConfig);
        m_initialized = true;
    } catch (...) {
        m_desktopTracker.Stop();
        throw;
    }
}

void WallpaperEngine::Run() {
    if (!m_initialized) throw std::runtime_error("WallpaperEngine::Run — not initialized");
    m_stopRequested.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_pausedForMaximizedWindow = false;
    try {
        m_coordinator.Start();
        while (!m_stopRequested.load(std::memory_order_acquire)) {
            if (m_desktopTracker.IsTracking()) m_desktopTracker.PumpPending();
            if (!m_coordinator.Update()) break;
            const bool pause = HasOtherMaximizedWindow();
            if (pause && !m_pausedForMaximizedWindow) {
                m_coordinator.Pause();
                m_pausedForMaximizedWindow = true;
            } else if (!pause && m_pausedForMaximizedWindow) {
                m_coordinator.Resume();
                m_pausedForMaximizedWindow = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        m_coordinator.Stop();
    } catch (...) {
        const std::exception_ptr original = std::current_exception();
        try { m_coordinator.Shutdown(); } catch (...) {}
        m_desktopTracker.Stop();
        m_wallpaperConfig.reset();
        m_initialized = false;
        m_running.store(false, std::memory_order_release);
        std::rethrow_exception(original);
    }
    m_pausedForMaximizedWindow = false;
    m_running.store(false, std::memory_order_release);
}

void WallpaperEngine::Stop() { m_stopRequested.store(true, std::memory_order_release); }

void WallpaperEngine::Shutdown() {
    if (m_initialized) m_coordinator.Shutdown();
    m_desktopTracker.Stop();
    m_wallpaperConfig.reset();
    m_initialized = false;
    m_running.store(false, std::memory_order_release);
    m_pausedForMaximizedWindow = false;
}

const PresentLoop::Stats WallpaperEngine::GetRenderStats() const noexcept {
    return m_coordinator.GetPrimaryRenderStats();
}
