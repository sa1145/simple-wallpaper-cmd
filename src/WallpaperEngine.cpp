// ============================================================================
// WallpaperEngine.cpp — Module 7: Top-level orchestrator
// ============================================================================

#include "WallpaperEngine.h"

#include <cassert>
#include <cstdio>
#include <dwmapi.h>

namespace {
bool IsDwmCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                            &cloaked, sizeof(cloaked)))
        && cloaked != 0;
}

bool HasOtherMaximizedWindow() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;

    // Owned dialogs/child windows inherit the state of their root owner.
    hwnd = GetAncestor(hwnd, GA_ROOTOWNER);
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)
        || IsDwmCloaked(hwnd) || !IsZoomed(hwnd)) {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    return processId != GetCurrentProcessId();
}
}

// ============================================================================
// Destructor
// ============================================================================

WallpaperEngine::~WallpaperEngine() {
    Shutdown();
}

// ============================================================================
// Initialize — assemble all modules in dependency order
// ============================================================================

void WallpaperEngine::Initialize(const Config& cfg) {
    if (m_initialized) {
        throw std::runtime_error("WallpaperEngine::Initialize — already initialized");
    }
    if (cfg.videoPath.empty()) {
        throw std::runtime_error("WallpaperEngine::Initialize — videoPath is empty");
    }
    const uint32_t targetFPS = (cfg.targetFPS == 0 || cfg.targetFPS > 60)
                                 ? 60
                                 : cfg.targetFPS;

    printf("========================================\n");
    printf("[WallpaperEngine] Initializing...\n");
    printf("========================================\n");

    // -----------------------------------------------------------------
    // Step 1: WindowManager — create the wallpaper window
    // -----------------------------------------------------------------
    {
        WindowManager::Config winCfg;
        winCfg.autoDetectSize = cfg.autoDetectSize;
        winCfg.width          = cfg.windowWidth;
        winCfg.height         = cfg.windowHeight;
        winCfg.windowTitle    = L"DX12 Wallpaper Engine";

        m_windowManager.Create(winCfg);

        printf("[WallpaperEngine] Window: %u x %u\n",
               m_windowManager.GetWidth(), m_windowManager.GetHeight());
    }

    // Register resize callback for display changes.
    //
    // ★ 執行緒安全備註：
    //   OnDisplayChange 由 WM_DISPLAYCHANGE 觸發，經 DispatchMessageW
    //   在 PumpMessages() 內同步呼叫，因此與 PerformLoopSeek() 同屬
    //   主執行緒的序列化執行路徑，不會並行。
    //
    //   主迴圈順序：PumpMessages() → 檢查 loopSeekRequested → sleep
    //   兩者不可能同時執行，無需額外互斥保護。
    m_windowManager.SetResizeCallback(
        [this](uint32_t w, uint32_t h) { OnDisplayChange(w, h); }
    );

    // -----------------------------------------------------------------
    // Step 2: DX12Context — device, queue, swap chain (Flip Model)
    // -----------------------------------------------------------------
    {
        DX12Context::Config dxCfg;
        dxCfg.hwnd            = m_windowManager.GetHwnd();
        dxCfg.width           = m_windowManager.GetWidth();
        dxCfg.height          = m_windowManager.GetHeight();
        dxCfg.bufferCount     = 2;  // double buffering per spec
        dxCfg.backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;  // match BGRX output
        dxCfg.enableDebugLayer = cfg.enableDebugLayer;

        m_dx12Context.Initialize(dxCfg);
    }

    // -----------------------------------------------------------------
    // Step 3: VideoDecoder — open video, configure MF source reader
    //         (this also queries video dimensions and allocates ring buffer)
    // -----------------------------------------------------------------
    {
        VideoDecoder::Config decCfg;
        decCfg.filePath       = cfg.videoPath;
        decCfg.ringBuffer     = &m_ringBuffer;
        decCfg.adapter        = m_dx12Context.GetAdapter();
        decCfg.allowSoftwareFallback = true;
        decCfg.targetFPS      = targetFPS;

        m_videoDecoder.Open(decCfg);

        // Register EOS callback (fires from DecodeThread → sets atomic flag)
        m_videoDecoder.SetEndOfStreamCallback(
            [this]() { OnEndOfStream(); }
        );
    }

    const auto& videoInfo = m_videoDecoder.GetVideoInfo();

    // -----------------------------------------------------------------
    // Step 4: UploadPipeline — create upload heap buffers
    // -----------------------------------------------------------------
    {
        UploadPipeline::Config upCfg;
        upCfg.dx12        = &m_dx12Context;
        upCfg.ringBuffer  = &m_ringBuffer;
        upCfg.decoder     = &m_videoDecoder;
        upCfg.frameWidth  = videoInfo.width;
        upCfg.frameHeight = videoInfo.height;

        m_uploadPipeline.Initialize(upCfg);
    }

    // -----------------------------------------------------------------
    // Step 5: PresentLoop — bind DX12 context and upload pipeline
    // -----------------------------------------------------------------
    {
        PresentLoop::Config plCfg;
        plCfg.dx12           = &m_dx12Context;
        plCfg.uploadPipeline = &m_uploadPipeline;
        plCfg.targetFPS      = targetFPS;

        m_presentLoop.Initialize(plCfg);
    }

    m_initialized = true;

    printf("========================================\n");
    printf("[WallpaperEngine] Initialization complete\n");
    printf("  Video: %ls\n", cfg.videoPath.c_str());
    printf("  Resolution: %u x %u\n", videoInfo.width, videoInfo.height);
    printf("  FPS: %.2f, Duration: %.2f sec\n",
           videoInfo.fps, videoInfo.durationSec);
    printf("  Ring buffer: %d slots, %zu bytes/frame\n",
           FrameRingBuffer::CAPACITY, m_ringBuffer.GetMaxFrameBytes());
    printf("========================================\n");
}

// ============================================================================
// Run — main loop (blocks until WM_QUIT or Stop)
// ============================================================================

void WallpaperEngine::Run() {
    if (!m_initialized) {
        throw std::runtime_error("WallpaperEngine::Run — not initialized");
    }

    printf("[WallpaperEngine] Starting playback...\n");

    m_running.store(true, std::memory_order_release);
    m_stopRequested.store(false, std::memory_order_relaxed);
    m_loopSeekRequested.store(false, std::memory_order_relaxed);
    m_pausedForMaximizedWindow = false;

    // Start worker threads
    m_videoDecoder.Start();   // launches DecodeThread
    m_presentLoop.Start();    // launches RenderThread

    printf("[WallpaperEngine] All threads running. Entering message loop.\n");

    // -----------------------------------------------------------------
    // Main loop: message pump + loop seek coordination
    // -----------------------------------------------------------------
    while (!m_stopRequested.load(std::memory_order_acquire)) {

        // Pump Win32 messages (non-blocking)
        if (!m_windowManager.PumpMessages()) {
            // WM_QUIT received
            printf("[WallpaperEngine] WM_QUIT received\n");
            break;
        }

        const bool shouldPause = HasOtherMaximizedWindow();
        if (shouldPause && !m_pausedForMaximizedWindow) {
            m_videoDecoder.Pause();
            m_presentLoop.Pause();
            m_pausedForMaximizedWindow = true;
            printf("[WallpaperEngine] Paused: another window is maximized\n");
        } else if (!shouldPause && m_pausedForMaximizedWindow) {
            m_pausedForMaximizedWindow = false;
            m_videoDecoder.Resume();
            m_presentLoop.Resume();
            printf("[WallpaperEngine] Resumed: desktop is visible\n");
        }

        // Duration is the loop boundary; frame count must never stretch time.
        const bool drainedEOS = m_loopSeekRequested.load(std::memory_order_acquire)
                             && m_ringBuffer.IsEmpty();
        if (!m_pausedForMaximizedWindow
            && (drainedEOS || m_videoDecoder.IsPlaybackTimeElapsed())) {
            PerformLoopSeek();
            m_loopSeekRequested.store(false, std::memory_order_release);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    printf("[WallpaperEngine] Exiting main loop\n");

    // Stop worker threads
    m_presentLoop.Stop();
    m_videoDecoder.Stop();
    m_pausedForMaximizedWindow = false;

    m_running.store(false, std::memory_order_release);

    // Print final stats
    auto stats = m_presentLoop.GetStats();
    printf("========================================\n");
    printf("[WallpaperEngine] Final statistics:\n");
    printf("  Total frames presented: %llu\n",
           static_cast<unsigned long long>(stats.totalFrames));
    printf("  Skipped frames: %llu\n",
           static_cast<unsigned long long>(stats.skippedFrames));
    printf("  Average frame time: %.2f ms (%.1f FPS)\n",
           stats.avgFrameTimeMs, stats.currentFPS);
    printf("  Uploaded frames: %llu\n",
           static_cast<unsigned long long>(m_uploadPipeline.GetUploadedFrameCount()));
    printf("  CPU-copied decoded frames: %llu\n",
           static_cast<unsigned long long>(m_videoDecoder.GetCpuCopiedFrameCount()));
    printf("========================================\n");
}

// ============================================================================
// Stop — request graceful shutdown (thread-safe, callable from any thread)
// ============================================================================

void WallpaperEngine::Stop() {
    m_stopRequested.store(true, std::memory_order_release);
}

// ============================================================================
// Shutdown — teardown all modules in reverse order
// ============================================================================

void WallpaperEngine::Shutdown() {
    if (!m_initialized) return;

    printf("[WallpaperEngine] Shutting down...\n");

    // Stop threads first (if not already stopped)
    m_presentLoop.Stop();
    m_videoDecoder.Stop();

    // Shutdown in reverse initialization order
    m_presentLoop.Shutdown();
    m_uploadPipeline.Shutdown();
    m_videoDecoder.Close();
    // m_ringBuffer has no Shutdown — it's a value type
    m_dx12Context.Shutdown();
    m_windowManager.Destroy();

    m_initialized = false;

    printf("[WallpaperEngine] Shutdown complete\n");
}

// ============================================================================
// OnEndOfStream — EOS callback (called from DecodeThread)
// ============================================================================

void WallpaperEngine::OnEndOfStream() {
    // ★ This is called from the DecodeThread.
    //   We MUST NOT perform the loop seek here (would deadlock on Pause).
    //   Instead, set an atomic flag for the main thread to handle.
    printf("[WallpaperEngine] End-of-stream detected, requesting loop seek\n");
    m_loopSeekRequested.store(true, std::memory_order_release);
}

// ============================================================================
// PerformLoopSeek — safe loop seek sequence (called from main thread)
//
// ★ STRICT ORDER (per user specification):
//
//   1. VideoDecoder::Pause()      ← 阻塞，確保 DecodeThread 真正暫停
//   2. PresentLoop::Pause()       ← 阻塞，確保 RenderThread 真正暫停
//   3. FrameRingBuffer::Reset()   ← 此時兩個 thread 都已停止，無競爭
//   4. VideoDecoder::Seek(0.0)    ← MF SourceReader seek 到影片開頭
//   5. VideoDecoder::Resume()     ← 恢復 DecodeThread
//   6. PresentLoop::Resume()      ← 恢復 RenderThread
//
// ⚠ Reset() 只能在兩個 thread 都已暫停後才能呼叫
// ⚠ Pause() 是阻塞的，返回時 thread 已保證暫停
// ============================================================================

void WallpaperEngine::PerformLoopSeek() {
    printf("[WallpaperEngine] Performing loop seek...\n");

    // ----- Step 1: Pause DecodeThread -----
    // Blocks until DecodeThread confirms it is paused and idle.
    m_videoDecoder.Pause();
    assert(m_videoDecoder.IsPaused() &&
           "DecodeThread must be paused before Reset");

    // ----- Step 2: Pause RenderThread -----
    // Blocks until RenderThread confirms it is paused and idle.
    m_presentLoop.Pause();
    assert(m_presentLoop.IsPaused() &&
           "RenderThread must be paused before Reset");

    // GPU slots retain MF samples until every submitted copy has completed.
    m_dx12Context.FlushGPU();

    // ----- Step 3: Reset FrameRingBuffer -----
    // ★ SAFE: both threads are confirmed paused. No concurrent access.
    m_ringBuffer.Reset();

    // NOTE: 不呼叫 ResetFirstUseFlags()。
    // Loop seek 不會重建 SwapChain back buffer，它們仍處於 PRESENT 狀態。
    // ResetFirstUseFlags() 只應在 Resize（back buffer 重建為 COMMON 狀態）後呼叫。

    // ----- Step 4: Seek to beginning -----
    // VideoDecoder::Seek() asserts that DecodeThread is paused.
    m_videoDecoder.Seek(0.0);

    // ----- Step 5: Resume DecodeThread -----
    m_videoDecoder.Resume();

    // ----- Step 6: Resume RenderThread -----
    m_presentLoop.Resume();

    printf("[WallpaperEngine] Loop seek complete — playback restarted\n");
}

// ============================================================================
// OnDisplayChange — handle monitor resolution change
// ============================================================================

void WallpaperEngine::OnDisplayChange(uint32_t newWidth, uint32_t newHeight) {
    printf("[WallpaperEngine] Display change: %u x %u\n", newWidth, newHeight);

    if (!m_initialized) return;

    // Pause both threads for safe resize
    m_videoDecoder.Pause();
    m_presentLoop.Pause();

    // Flush GPU and resize swap chain
    m_dx12Context.ResizeBuffers(newWidth, newHeight);

    // Resize upload pipeline
    m_uploadPipeline.Resize(newWidth, newHeight);

    if (!m_pausedForMaximizedWindow) {
        m_videoDecoder.Resume();
        m_presentLoop.Resume();
    }

    printf("[WallpaperEngine] Resize complete\n");
}
