#pragma once
// ============================================================================
// PresentLoop.h — Module 6: high-resolution paced render loop
// ============================================================================
//
// 職責：以目標 FPS 驅動渲染迴圈，使用高精度 waitable timer 精確計時。
//       協調 DX12Context 和 UploadPipeline 的每幀操作。
//
// ★ 幀迴圈嚴格順序：
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │  BeginFrame()                                                │
//   │      ↓                                                       │
//   │  UploadFrame(cmdList)  ← 返回 true/false                     │
//   │      ↓                                                       │
//   │  [if true]  EndFrame() → Present()                            │
//   │  [if false] Close(cmdList)                                    │
//   │  WaitUntilNextFrame()                                         │
//   │      ↓                                                       │
//   │  下一次迴圈                                                   │
//   └──────────────────────────────────────────────────────────────┘
//
// ★ UploadFrame 返回 false 時（ring buffer 暫時為空）：
//   - 必須關閉 command list（BeginFrame 已開啟它）
//   - 跳過 EndFrame（不提交空的 command list）
//   - 跳過 Present（沒有新內容，前一幀留在螢幕上）
//   - 仍使用高精度計時器維持節拍，避免 busy-wait
//
// Thread safety:
//   - PresentLoop 擁有自己的 RenderThread
//   - Start/Stop/Pause/Resume 從主執行緒（協調執行緒）呼叫
//   - 所有 DX12 操作在 RenderThread 上執行
// ============================================================================

#include "Common.h"
#include "DX12Context.h"
#include "UploadPipeline.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

class PresentLoop {
public:
    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------
    struct Config {
        DX12Context*     dx12           = nullptr;
        UploadPipeline*  uploadPipeline = nullptr;
        uint32_t         targetFPS      = 60;
    };

    // -----------------------------------------------------------------------
    // Runtime statistics
    // -----------------------------------------------------------------------
    struct Stats {
        uint64_t    totalFrames       = 0;  // frames presented
        uint64_t    skippedFrames     = 0;  // frames skipped (ring buffer empty)
        double      avgFrameTimeMs    = 0.0;
        double      currentFPS        = 0.0;
    };

    PresentLoop()  = default;
    ~PresentLoop();

    // Non-copyable
    PresentLoop(const PresentLoop&)            = delete;
    PresentLoop& operator=(const PresentLoop&) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Bind DX12Context and UploadPipeline. Does NOT start the thread.
    void Initialize(const Config& cfg);

    /// Release references and reset state.
    void Shutdown();

    // -----------------------------------------------------------------------
    // Thread control
    // -----------------------------------------------------------------------

    /// Start the render thread.
    void Start();

    /// Signal the render thread to stop and join it.
    void Stop();

    /// Pause the render thread (blocks until confirmed paused).
    /// Used for the safe loop seek flow:
    ///   decoder.Pause() → presentLoop.Pause() → ringBuffer.Reset()
    ///   → decoder.Seek(0) → decoder.Resume() → presentLoop.Resume()
    void Pause();

    /// Resume the render thread.
    void Resume();

    [[nodiscard]] bool IsRunning() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool IsPaused() const noexcept {
        return m_paused.load(std::memory_order_acquire);
    }

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    /// Get a snapshot of current statistics (thread-safe via atomics).
    [[nodiscard]] Stats GetStats() const noexcept;

private:
    // Render thread entry point
    void RenderThreadFunc();

    // Execute one frame of the render loop. Returns false if loop should exit.
    bool ExecuteOneFrame();

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    DX12Context*        m_dx12           = nullptr;
    UploadPipeline*     m_uploadPipeline = nullptr;

    std::thread         m_renderThread;
    std::atomic<bool>   m_running{false};
    std::atomic<bool>   m_stopRequested{false};

    // Pause / resume
    std::atomic<bool>   m_pauseRequested{false};
    std::atomic<bool>   m_paused{false};
    std::mutex          m_pauseMutex;
    std::condition_variable m_pauseCV;
    std::mutex          m_pauseConfirmMutex;
    std::condition_variable m_pauseConfirmCV;

    // Stats (updated from render thread, read from any thread)
    std::atomic<uint64_t>   m_totalFrames{0};
    std::atomic<uint64_t>   m_skippedFrames{0};
    std::atomic<double>     m_avgFrameTimeMs{0.0};
    std::atomic<double>     m_currentFPS{0.0};

    uint32_t    m_targetFPS     = 60;
    bool        m_initialized   = false;

    // Frame latency waitable handle (cached from DX12Context)
    HANDLE      m_frameLatencyHandle = nullptr;
    HANDLE      m_pacingTimer = nullptr;
};
