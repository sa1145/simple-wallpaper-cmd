#pragma once
// ============================================================================
// WallpaperEngine.h — Module 7: Top-level orchestrator for all modules
// ============================================================================
//
// 職責：組裝並協調所有模組，提供單一入口 Run()。
//
// 模組組裝順序：
//   WindowManager → DX12Context → FrameRingBuffer → VideoDecoder
//   → UploadPipeline → PresentLoop
//
// ★ Video Loop (EOS) 處理流程（嚴格順序）：
//
//   當 VideoDecoder 到達影片結尾（End-of-Stream）時，EOS callback
//   設定 m_loopSeekRequested 旗標。主迴圈檢測到旗標後，從主執行緒
//   執行以下安全 seek 序列：
//
//   1. VideoDecoder::Pause()      ← 阻塞，確保 DecodeThread 真正暫停
//   2. PresentLoop::Pause()       ← 阻塞，確保 RenderThread 真正暫停
//   3. FrameRingBuffer::Reset()   ← 此時兩個 thread 都已停止，無競爭
//   4. VideoDecoder::Seek(0.0)    ← MF SourceReader seek 到影片開頭
//   5. VideoDecoder::Resume()     ← 恢復 DecodeThread
//   6. PresentLoop::Resume()      ← 恢復 RenderThread
//
//   ⚠ Reset() 絕不能在任何 thread 執行中呼叫
//   ⚠ Pause() 是阻塞的，保證返回時 thread 已暫停
//
// Thread model:
//   - Main thread:   message pump + loop seek coordination
//   - DecodeThread:   owned by VideoDecoder
//   - RenderThread:   owned by PresentLoop
// ============================================================================

#include "Common.h"
#include "WindowManager.h"
#include "DX12Context.h"
#include "FrameRingBuffer.h"
#include "VideoDecoder.h"
#include "UploadPipeline.h"
#include "PresentLoop.h"

#include <atomic>
#include <string>

class WallpaperEngine {
public:
    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------
    struct Config {
        std::wstring    videoPath;               // 影片檔案路徑
        uint32_t        targetFPS       = 60;
        bool            enableDebugLayer = false; // DX12 debug layer
        bool            autoDetectSize  = true;   // 自動偵測螢幕解析度
        uint32_t        windowWidth     = 1920;   // 手動解析度（autoDetect=false 時生效）
        uint32_t        windowHeight    = 1080;
    };

    WallpaperEngine()  = default;
    ~WallpaperEngine();

    // Non-copyable
    WallpaperEngine(const WallpaperEngine&)            = delete;
    WallpaperEngine& operator=(const WallpaperEngine&) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Initialize all modules in dependency order.
    void Initialize(const Config& cfg);

    /// Run the main loop (blocks until WM_QUIT or Stop is called).
    /// Handles message pump and loop seek coordination.
    void Run();

    /// Request a graceful shutdown from any thread.
    void Stop();

    /// Teardown all modules in reverse order.
    void Shutdown();

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    [[nodiscard]] bool IsRunning() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }

    [[nodiscard]] const PresentLoop::Stats GetRenderStats() const noexcept {
        return m_presentLoop.GetStats();
    }

private:
    // EOS callback — called from DecodeThread, sets flag for main thread
    void OnEndOfStream();

    // Execute the safe loop seek sequence from the main thread
    void PerformLoopSeek();

    // Handle display change (resize)
    void OnDisplayChange(uint32_t newWidth, uint32_t newHeight);

    // -----------------------------------------------------------------------
    // Modules (owned, initialized in order)
    // -----------------------------------------------------------------------
    WindowManager       m_windowManager;
    DX12Context         m_dx12Context;
    FrameRingBuffer     m_ringBuffer;
    VideoDecoder        m_videoDecoder;
    UploadPipeline      m_uploadPipeline;
    PresentLoop         m_presentLoop;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    std::atomic<bool>   m_running{false};
    std::atomic<bool>   m_stopRequested{false};
    std::atomic<bool>   m_loopSeekRequested{false};  // set by EOS callback

    bool                m_pausedForMaximizedWindow = false;
    bool                m_initialized = false;
};
