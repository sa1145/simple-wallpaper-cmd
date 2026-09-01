#pragma once

#include "MonitorEnumerator.h"
#include "PresentLoop.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <dxgi1_6.h>

class MonitorPipeline {
public:
    using BudgetQuery = std::function<DXGI_QUERY_VIDEO_MEMORY_INFO(IDXGIAdapter3*)>;

    struct Config {
        MonitorDescriptor target;
        std::wstring videoPath;
        uint32_t targetFPS = 60;
        bool enableDebugLayer = false;
        BudgetQuery budgetQuery;
        std::function<void()> onDisplayChange;
    };

    MonitorPipeline();
    ~MonitorPipeline() noexcept;
    MonitorPipeline(const MonitorPipeline&) = delete;
    MonitorPipeline& operator=(const MonitorPipeline&) = delete;
    MonitorPipeline(MonitorPipeline&&) = delete;
    MonitorPipeline& operator=(MonitorPipeline&&) = delete;

    bool Initialize(const Config& config);
    void Start();
    void Update();
    void Pause();
    void Resume();
    // Performs a hard cut without recreating the HWND, DX12 device, or budget event.
    // Returns false when the requested media could not be applied; a successful rollback preserves the old media.
    bool SwitchMedia(const std::wstring& videoPath, uint32_t targetFPS);
    void Stop();
    void Shutdown();
    bool PumpMessages();

    // Test seam: exercises the same event path used by DXGI budget notifications.
    bool SignalBudgetChangeForCheck();

    [[nodiscard]] float GetRenderScale() const noexcept { return m_renderScale; }
    [[nodiscard]] bool IsPrimary() const noexcept { return m_config.target.primary; }
    [[nodiscard]] uint32_t GetTargetFPS() const noexcept { return m_config.targetFPS; }
    [[nodiscard]] uint64_t GetHardCutCount() const noexcept { return m_hardCutCount; }
    [[nodiscard]] uint64_t GetCpuCopiedFrameCount() const noexcept;
    [[nodiscard]] uint64_t GetPresentedFrameCount() const noexcept;
    [[nodiscard]] uint64_t GetUploadedFrameCount() const noexcept;
    [[nodiscard]] PresentLoop::Stats GetRenderStats() const noexcept;
    [[nodiscard]] uint32_t GetRenderWidth() const noexcept;
    [[nodiscard]] uint32_t GetRenderHeight() const noexcept;
    [[nodiscard]] RECT GetWindowBounds() const noexcept;
    [[nodiscard]] HWND GetWindowHandle() const noexcept;
    [[nodiscard]] bool IsWindowHealthy() const noexcept;

private:
    struct Runtime;

    bool Build(float scale);
    bool RebuildBelowCurrentScale();
    void OpenMedia(Runtime& runtime, const std::wstring& videoPath, uint32_t targetFPS);
    void CloseMedia(Runtime& runtime);
    void RestoreMediaState(bool wasRunning, bool wasPaused);
    void TearDownRuntime();
    void PerformLoopSeek();
    void OnEndOfStream();
    void RegisterBudgetEvent();
    void UnregisterBudgetEvent();

    Config m_config;
    std::unique_ptr<Runtime> m_runtime;
    std::atomic<bool> m_loopSeekRequested{false};
    float m_renderScale = 0.0f;
    bool m_initialized = false;
    bool m_running = false;
    bool m_paused = false;
    uint64_t m_hardCutCount = 0;
};
