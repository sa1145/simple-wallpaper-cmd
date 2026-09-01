#pragma once

#include "MonitorEnumerator.h"
#include "MonitorPipeline.h"
#include "PresentLoop.h"
#include "WallpaperConfig.h"

#include <memory>
#include <string>
#include <vector>

class MonitorCoordinator {
public:
    struct Config {
        std::wstring videoPath;
        uint32_t targetFPS = 60;
        bool enableDebugLayer = false;
        bool autoDetectSize = true;
        uint32_t manualWidth = 1920;
        uint32_t manualHeight = 1080;
        MonitorPipeline::BudgetQuery budgetQuery;
        const WallpaperConfig* wallpaperConfig = nullptr;
        GUID desktopId{};
    };

    MonitorCoordinator() = default;
    ~MonitorCoordinator();
    MonitorCoordinator(const MonitorCoordinator&) = delete;
    MonitorCoordinator& operator=(const MonitorCoordinator&) = delete;

    void Initialize(const Config& config);
    void Start();
    bool Update();
    void Stop();
    void Shutdown();
    void Pause();
    void Resume();
    void SetCurrentDesktopId(const GUID& desktopId);

    // Shared with the deterministic check so production and verification use
    // the same monitor-path and desktop-GUID resolution.
    [[nodiscard]] static ResolvedWallpaper ResolveWallpaperForMonitor(
        const Config& config, const MonitorDescriptor& monitor, const GUID& desktopId);

    // Narrow test seam. Production display-change handling calls this same method.
    bool Reconcile(const std::vector<MonitorDescriptor>& monitors);
    void ScheduleDisplayChange() noexcept { m_displayChangePending = true; }

    [[nodiscard]] const PresentLoop::Stats GetPrimaryRenderStats() const noexcept;
    [[nodiscard]] uint64_t GetPrimaryUploadedFrameCountForCheck() const noexcept;
    [[nodiscard]] uint64_t GetPrimaryHardCutCountForCheck() const noexcept;
    [[nodiscard]] HWND GetPipelineWindowForCheck(const std::wstring& devicePath) const noexcept;

private:
    struct Entry {
        MonitorDescriptor descriptor;
        std::unique_ptr<MonitorPipeline> pipeline;
    };

    void CheckOwnerThread() const;
    void Cleanup(bool shutdown);
    static void ShutdownEntries(std::vector<Entry>& entries) noexcept;

    Config m_config;
    std::vector<Entry> m_entries;
    DWORD m_ownerThreadId = 0;
    bool m_initialized = false;
    bool m_running = false;
    bool m_paused = false;
    bool m_displayChangePending = false;
    bool m_quitRequested = false;
    GUID m_desktopId{};
};
