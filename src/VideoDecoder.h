#pragma once
// ============================================================================
// VideoDecoder.h — Media Foundation GPU-DXVA decoder with CPU fallback
// ============================================================================
//
// 職責：CPU 端影片解碼，輸出 raw BGRA frame 到 FrameRingBuffer（Producer）
//
// 設計原則（per spec）：
//   - CPU 軟解影片，GPU 只做 Texture Upload（禁止 GPU 硬解 DXVA2/D3D11VA）
//   - 使用 Windows Media Foundation (IMFSourceReader)，系統內建免額外依賴
//   - Ring Buffer 預解碼 4 幀緩衝，解碼與渲染分離避免 stall
//   - 禁止每幀 new/malloc，所有 buffer 預分配
//
// 執行緒模型：
//   - VideoDecoder 擁有一條獨立的 DecodeThread
//   - 支援 Pause() / Resume() 用於安全的 loop seek
//
// ★ Video Loop Seek 安全流程（嚴格規則）：
//
//   FrameRingBuffer::Reset() 呼叫前，DecodeThread 和 RenderThread
//   都必須已暫停。完整流程由上層（WallpaperEngine）協調：
//
//   1. decoder.Pause()          — 暫停 DecodeThread
//   2. renderThread pause       — 暫停 RenderThread（上層負責）
//   3. ringBuffer.Reset()       — 清空所有 slot（安全，無競爭）
//   4. decoder.Seek(0)          — MF SourceReader seek 到影片開頭
//   5. decoder.Resume()         — 恢復 DecodeThread
//   6. renderThread resume      — 恢復 RenderThread（上層負責）
//
//   ⚠ 禁止在 DecodeThread / RenderThread 執行中直接呼叫 Reset()
//
// Thread safety:
//   - Start/Stop/Pause/Resume/Seek: 從主執行緒（或協調執行緒）呼叫
//   - DecodeThread 內部迴圈只存取 FrameRingBuffer 的 Producer API
// ============================================================================

#include "Common.h"
#include "FrameRingBuffer.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class VideoDecoder {
public:
    enum class DecodePath { GpuDxva, CpuRgb32 };

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------
    struct Config {
        std::wstring    filePath;               // 影片檔案路徑
        FrameRingBuffer* ringBuffer = nullptr;  // 寫入目標（Producer 端）
        IDXGIAdapter4*  adapter = nullptr;      // non-owning; DX12Context outlives us
        bool            allowSoftwareFallback = true;
        uint32_t        targetFPS = 60;         // 超過此幀率時依時間戳降幀
    };

    // -----------------------------------------------------------------------
    // Video metadata (available after Open)
    // -----------------------------------------------------------------------
    struct VideoInfo {
        uint32_t    width       = 0;
        uint32_t    height      = 0;
        uint32_t    stride      = 0;        // bytes per row (BGRA)
        double      durationSec = 0.0;      // total duration in seconds
        double      fps         = 0.0;      // frames per second
        uint64_t    frameCount  = 0;        // estimated total frames
    };

    // -----------------------------------------------------------------------
    // Callback: fired when end-of-stream is reached (for loop coordination)
    // -----------------------------------------------------------------------
    using EndOfStreamCallback = std::function<void()>;

    VideoDecoder()  = default;
    ~VideoDecoder();

    // Non-copyable
    VideoDecoder(const VideoDecoder&)            = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Initialize Media Foundation and open the video file.
    /// Configures SourceReader for CPU-only BGRA output.
    /// Does NOT start the decode thread.
    void Open(const Config& cfg);

    /// Close the source reader and release MF resources.
    void Close();

    /// Rebuild the reader as CPU RGB32. Only valid before Start().
    void FallbackToCpu(const char* reason);

    // -----------------------------------------------------------------------
    // Thread control
    // -----------------------------------------------------------------------

    /// Start the decode thread. Must call Open() first.
    void Start();

    /// Signal the decode thread to stop and join it.
    void Stop();

    /// Pause the decode thread (blocks until thread is confirmed paused).
    /// Safe to call from the coordinating thread.
    void Pause();

    /// Resume a paused decode thread.
    void Resume();

    /// Returns true if the decode thread is currently paused and idle.
    [[nodiscard]] bool IsPaused() const noexcept {
        return m_paused.load(std::memory_order_acquire);
    }

    /// Returns true if the decode thread is running (started and not stopped).
    [[nodiscard]] bool IsRunning() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }

    // -----------------------------------------------------------------------
    // Seek (must be called while decode thread is PAUSED)
    // -----------------------------------------------------------------------

    /// Seek the source reader to the given position in seconds.
    /// ★ PRECONDITION: DecodeThread must be paused via Pause().
    void Seek(double positionSeconds);

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    [[nodiscard]] const VideoInfo& GetVideoInfo() const noexcept { return m_videoInfo; }
    [[nodiscard]] DecodePath GetDecodePath() const noexcept { return m_decodePath; }
    [[nodiscard]] uint64_t GetCpuCopiedFrameCount() const noexcept {
        return m_cpuCopiedFrameCount.load(std::memory_order_relaxed);
    }
    [[nodiscard]] ID3D11Device* GetD3D11Device() const noexcept { return m_d3d11Device.Get(); }
    [[nodiscard]] ID3D11DeviceContext* GetD3D11Context() const noexcept { return m_d3d11Context.Get(); }
    [[nodiscard]] bool IsEndOfStream() const noexcept {
        return m_endOfStream.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool IsPlaybackTimeElapsed() const noexcept {
        return m_videoInfo.durationSec > 0.0
            && std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - m_playbackClockStart).count()
               >= m_videoInfo.durationSec;
    }

    /// Register a callback that fires when EOS is reached.
    /// The callback is invoked from the DecodeThread — keep it lightweight
    /// (e.g. set an atomic flag for the main thread to handle the loop).
    void SetEndOfStreamCallback(EndOfStreamCallback cb) {
        m_eosCallback = std::move(cb);
    }

private:
    // Decode thread entry point
    void DecodeThreadFunc();

    // Read and decode one sample from the source reader.
    // Returns false on EOS or error.
    bool DecodeOneSample();

    // MF initialization helpers
    void InitializeMF();
    void ShutdownMF();
    void CreateD3D11DeviceManager();
    void CreateSourceReader(const std::wstring& filePath, DecodePath path);
    void ConfigureOutputType(DecodePath path);
    void QueryVideoInfo();
    void ProbeGpuSample();
    void OpenCpuPath(const char* reason);

    // -----------------------------------------------------------------------
    // Media Foundation objects
    // -----------------------------------------------------------------------
    ComPtr<IMFSourceReader>     m_sourceReader;
    ComPtr<IMFDXGIDeviceManager> m_dxgiDeviceManager;
    ComPtr<ID3D11Device>        m_d3d11Device;
    ComPtr<ID3D11DeviceContext> m_d3d11Context;
    UINT                        m_deviceManagerResetToken = 0;
    bool                        m_mfInitialized = false;
    DecodePath                  m_decodePath = DecodePath::CpuRgb32;
    IDXGIAdapter4*              m_adapter = nullptr;
    bool                        m_allowSoftwareFallback = true;
    std::wstring                m_filePath;
    ComPtr<IMFSample>           m_pendingSample;
    DWORD                       m_pendingFlags = 0;
    LONGLONG                    m_pendingTimestamp = 0;

    // -----------------------------------------------------------------------
    // Video metadata
    // -----------------------------------------------------------------------
    VideoInfo                   m_videoInfo{};
    uint32_t                    m_targetFPS = 60;
    LONGLONG                    m_minFrameIntervalHns = 166'666;
    LONGLONG                    m_nextFrameTimestampHns = -1;
    LONGLONG                    m_timestampOffsetHns = 0;
    std::chrono::steady_clock::time_point m_playbackClockStart{};
    std::chrono::steady_clock::time_point m_pauseStartedAt{};

    // -----------------------------------------------------------------------
    // Ring buffer (owned by caller, we're the Producer)
    // -----------------------------------------------------------------------
    FrameRingBuffer*            m_ringBuffer = nullptr;

    // -----------------------------------------------------------------------
    // Thread state
    // -----------------------------------------------------------------------
    std::thread                 m_decodeThread;
    std::atomic<bool>           m_running{false};
    std::atomic<bool>           m_stopRequested{false};
    std::atomic<bool>           m_endOfStream{false};
    std::atomic<uint64_t>       m_cpuCopiedFrameCount{0};

    // Pause / resume mechanism
    std::atomic<bool>           m_pauseRequested{false};
    std::atomic<bool>           m_paused{false};
    std::mutex                  m_pauseMutex;
    std::condition_variable     m_pauseCV;
    std::mutex                  m_pauseConfirmMutex;
    std::condition_variable     m_pauseConfirmCV;

    // Callback
    EndOfStreamCallback         m_eosCallback;
};
