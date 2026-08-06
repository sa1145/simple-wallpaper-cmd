// ============================================================================
// PresentLoop.cpp — Module 6: high-resolution paced render loop
// ============================================================================

#include "PresentLoop.h"

#include <cassert>
#include <cstdio>

// ============================================================================
// Destructor
// ============================================================================

PresentLoop::~PresentLoop() {
    Stop();
    Shutdown();
}

// ============================================================================
// Initialize
// ============================================================================

void PresentLoop::Initialize(const Config& cfg) {
    if (m_initialized) {
        throw std::runtime_error("PresentLoop::Initialize — already initialized");
    }
    if (!cfg.dx12 || !cfg.uploadPipeline) {
        throw std::runtime_error("PresentLoop::Initialize — null dx12 or uploadPipeline");
    }

    m_dx12           = cfg.dx12;
    m_uploadPipeline = cfg.uploadPipeline;
    m_targetFPS      = (cfg.targetFPS == 0 || cfg.targetFPS > 60)
                         ? 60
                         : cfg.targetFPS;

    // Cache the waitable handle
    m_frameLatencyHandle = m_dx12->GetFrameLatencyWaitableObject();
    if (!m_frameLatencyHandle) {
        throw std::runtime_error(
            "PresentLoop::Initialize — frame latency waitable handle is null");
    }
    m_pacingTimer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!m_pacingTimer) {
        throw std::runtime_error("PresentLoop::Initialize — pacing timer creation failed");
    }

    m_initialized = true;
    printf("[PresentLoop] Initialized, target FPS: %u\n", m_targetFPS);
}

// ============================================================================
// Shutdown
// ============================================================================

void PresentLoop::Shutdown() {
    if (!m_initialized) return;

    Stop();

    m_dx12             = nullptr;
    m_uploadPipeline   = nullptr;
    m_frameLatencyHandle = nullptr;
    CloseHandle(m_pacingTimer);
    m_pacingTimer = nullptr;
    m_initialized      = false;
}

// ============================================================================
// Start
// ============================================================================

void PresentLoop::Start() {
    if (m_running.load(std::memory_order_acquire)) return;
    if (!m_initialized) {
        throw std::runtime_error("PresentLoop::Start — not initialized");
    }

    m_stopRequested.store(false, std::memory_order_relaxed);
    m_pauseRequested.store(false, std::memory_order_relaxed);
    m_paused.store(false, std::memory_order_relaxed);
    m_totalFrames.store(0, std::memory_order_relaxed);
    m_skippedFrames.store(0, std::memory_order_relaxed);
    m_avgFrameTimeMs.store(0.0, std::memory_order_relaxed);
    m_currentFPS.store(0.0, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_release);

    m_renderThread = std::thread(&PresentLoop::RenderThreadFunc, this);

    printf("[PresentLoop] Render thread started\n");
}

// ============================================================================
// Stop
// ============================================================================

void PresentLoop::Stop() {
    if (!m_running.load(std::memory_order_acquire)) return;

    // Unblock any pause wait
    m_stopRequested.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_pauseMutex);
        m_pauseRequested.store(false, std::memory_order_release);
    }
    m_pauseCV.notify_all();

    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }

    m_running.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);

    printf("[PresentLoop] Render thread stopped. "
           "Total: %llu frames, Skipped: %llu\n",
           static_cast<unsigned long long>(m_totalFrames.load()),
           static_cast<unsigned long long>(m_skippedFrames.load()));
}

// ============================================================================
// Pause — block until render thread confirms it is paused
// ============================================================================

void PresentLoop::Pause() {
    if (!m_running.load(std::memory_order_acquire)) return;
    if (m_paused.load(std::memory_order_acquire))   return;

    m_pauseRequested.store(true, std::memory_order_release);

    // Wait for render thread to confirm pause
    std::unique_lock<std::mutex> lock(m_pauseConfirmMutex);
    m_pauseConfirmCV.wait(lock, [this] {
        return m_paused.load(std::memory_order_acquire);
    });

    printf("[PresentLoop] Render thread paused\n");
}

// ============================================================================
// Resume
// ============================================================================

void PresentLoop::Resume() {
    if (!m_running.load(std::memory_order_acquire)) return;
    if (!m_paused.load(std::memory_order_acquire))  return;

    {
        std::lock_guard<std::mutex> lock(m_pauseMutex);
        m_pauseRequested.store(false, std::memory_order_release);
        m_paused.store(false, std::memory_order_release);
    }
    m_pauseCV.notify_one();

    printf("[PresentLoop] Render thread resumed\n");
}

// ============================================================================
// GetStats
// ============================================================================

PresentLoop::Stats PresentLoop::GetStats() const noexcept {
    Stats s;
    s.totalFrames    = m_totalFrames.load(std::memory_order_relaxed);
    s.skippedFrames  = m_skippedFrames.load(std::memory_order_relaxed);
    s.avgFrameTimeMs = m_avgFrameTimeMs.load(std::memory_order_relaxed);
    s.currentFPS     = m_currentFPS.load(std::memory_order_relaxed);
    return s;
}

// ============================================================================
// RenderThreadFunc — main loop on the render thread
// ============================================================================

void PresentLoop::RenderThreadFunc() {
    printf("[PresentLoop] RenderThread enter\n");

    // Initial wait — ensure the swap chain is ready before the first frame.
    // The waitable object is signaled after SetMaximumFrameLatency(1),
    // so this returns immediately on the first call.
    WaitForSingleObjectEx(m_frameLatencyHandle, 1000, TRUE);

    while (!m_stopRequested.load(std::memory_order_acquire)) {

        // --- Check for pause request ---
        if (m_pauseRequested.load(std::memory_order_acquire)) {
            // Confirm to the caller that we are paused
            {
                std::lock_guard<std::mutex> lock(m_pauseConfirmMutex);
                m_paused.store(true, std::memory_order_release);
            }
            m_pauseConfirmCV.notify_one();

            // Block until resumed or stopped
            std::unique_lock<std::mutex> lock(m_pauseMutex);
            m_pauseCV.wait(lock, [this] {
                return !m_pauseRequested.load(std::memory_order_acquire)
                    || m_stopRequested.load(std::memory_order_acquire);
            });

            if (m_stopRequested.load(std::memory_order_acquire)) break;

            // After resume, wait for swap chain readiness before continuing
            WaitForSingleObjectEx(m_frameLatencyHandle, 1000, TRUE);
            continue;
        }

        // --- Execute one frame ---
        if (!ExecuteOneFrame()) {
            break;  // fatal error or stop requested
        }
    }

    printf("[PresentLoop] RenderThread exit\n");
}

// ============================================================================
// ExecuteOneFrame — the core frame cycle
//
// ★ STRICT ORDER:
//   Normal:  BeginFrame → UploadFrame(true)  → EndFrame → Present → pace
//   Skip:    BeginFrame → UploadFrame(false) → Close(cmdList) → pace
//
// When UploadFrame returns false (ring buffer empty):
//   - Close the command list (BeginFrame opened it, must close before next BeginFrame)
//   - Do NOT call EndFrame (don't submit empty command list)
//   - Do NOT call Present (no new content; previous frame stays on screen)
//   - Use the same high-resolution timer to preserve pacing
// ============================================================================

bool PresentLoop::ExecuteOneFrame() {
    using Clock = std::chrono::steady_clock;
    const auto frameStart = Clock::now();
    const auto frameDuration = std::chrono::duration<double>(1.0 / m_targetFPS);

    // ===== Step 1: BeginFrame =====
    // Waits on fence for current back buffer, resets command allocator & list.
    ID3D12GraphicsCommandList* cmdList = m_dx12->BeginFrame();
    if (!cmdList) {
        printf("[PresentLoop] BeginFrame returned null — aborting\n");
        return false;
    }

    // ===== Step 2: UploadFrame =====
    // Reads from ring buffer, copies to upload heap, records CopyTextureRegion.
    const bool uploaded = m_uploadPipeline->UploadFrame(cmdList);

    if (uploaded) {
        // ===== Step 3: EndFrame (normal path) =====
        // Closes command list, executes it, signals fence.
        m_dx12->EndFrame();

        // ===== Step 4: Present =====
        // Queue the frame for display with VSYNC.
        m_dx12->Present(1);

        // Update frame counter
        m_totalFrames.fetch_add(1, std::memory_order_relaxed);
    } else {
        // ===== Skip path: ring buffer empty =====
        //
        // ★ CRITICAL: BeginFrame opened the command list in recording state.
        //   We MUST close it before the next BeginFrame (which calls Reset).
        //   But we do NOT execute it — no work was recorded.
        //
        // ★ Do NOT call EndFrame — it would submit an empty command list
        //   and advance the fence unnecessarily.
        //
        // ★ Do NOT call Present — there's no new frame to display.
        //   The previous frame remains on screen (no flicker).
        //
        cmdList->Close();

        m_skippedFrames.fetch_add(1, std::memory_order_relaxed);
    }

    const auto targetTime = frameStart
        + std::chrono::duration_cast<Clock::duration>(frameDuration);
    const auto remaining = targetTime - Clock::now();
    if (remaining > Clock::duration::zero()) {
        LARGE_INTEGER dueTime{};
        dueTime.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(
            remaining).count() / 100;
        if (SetWaitableTimerEx(
                m_pacingTimer, &dueTime, 0, nullptr, nullptr, nullptr, 0)) {
            WaitForSingleObject(m_pacingTimer, INFINITE);
        } else {
            std::this_thread::sleep_until(targetTime);
        }
    }

    if (!uploaded) return true;

    // ===== Update statistics =====
    const auto frameEnd = Clock::now();
    const double frameTimeMs = std::chrono::duration<double, std::milli>(
        frameEnd - frameStart).count();

    // Exponential moving average for smooth FPS display
    const double alpha = 0.05;  // smoothing factor
    double prevAvg = m_avgFrameTimeMs.load(std::memory_order_relaxed);
    if (prevAvg <= 0.0) {
        prevAvg = frameTimeMs;  // first frame
    }
    const double newAvg = prevAvg * (1.0 - alpha) + frameTimeMs * alpha;
    m_avgFrameTimeMs.store(newAvg, std::memory_order_relaxed);
    m_currentFPS.store(newAvg > 0.0 ? 1000.0 / newAvg : 0.0,
                       std::memory_order_relaxed);

    return true;
}
