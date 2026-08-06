#pragma once
// ============================================================================
// UploadPipeline.h — Module 5: CPU→GPU frame upload via Upload Heap
// ============================================================================
//
// 職責：將 FrameRingBuffer 中的 CPU 端 raw frame 搬移到 GPU back buffer，
//       使用 Upload Heap（D3D12_HEAP_TYPE_UPLOAD）實現最低開銷路徑。
//
// 像素格式對齊：
//   - VideoDecoder 輸出 MFVideoFormat_RGB32 → 記憶體排列 BGRX
//   - Upload Texture 格式：DXGI_FORMAT_B8G8R8A8_UNORM（完美匹配）
//   - SwapChain back buffer：DXGI_FORMAT_B8G8R8A8_UNORM + ALPHA_MODE_IGNORE
//   - Alpha channel 無效，不需處理
//
// Stride 注意事項：
//   ★ VideoDecoder 的 info.stride 可能含 row padding（對齊到 64 bytes 等），
//     memcpy 必須按 info.stride * info.height 計算，不能用 width * 4 * height。
//   ★ D3D12 Upload Heap 的 RowPitch 必須對齊 D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
//     (256 bytes)，因此 src stride 和 dst rowPitch 通常不同，需逐行拷貝。
//
// 上傳流程（在 BeginFrame 與 EndFrame 之間執行）：
//
//   1. TryPeekRead() → 取得 CPU 端 frame 指標 + FrameInfo
//   2. memcpy: ring buffer → upload heap（逐行，處理 stride 差異）
//   3. 錄製 barrier: back buffer PRESENT/COMMON → COPY_DEST
//      ★ 第一幀特殊處理：SwapChain 建立後 back buffer 初始狀態為
//        D3D12_RESOURCE_STATE_COMMON，非 PRESENT。首次使用每個
//        back buffer 時必須從 COMMON 轉換，後續從 PRESENT 轉換。
//   4. 錄製 CopyTextureRegion: upload heap → back buffer（DMA，不佔 Shader 核心）
//   5. 錄製 barrier: back buffer COPY_DEST → PRESENT
//   6. CommitRead() → 釋放 ring buffer slot
//
// Thread safety:
//   - All methods must be called from the RenderThread only.
//   - Ring buffer consumer API (TryPeekRead/CommitRead) is SPSC-safe.
// ============================================================================

#include "Common.h"
#include "DX12Context.h"
#include "FrameRingBuffer.h"
#include "VideoDecoder.h"

#include <d3d12.h>
#include <d3d11_4.h>
#include <array>
#include <cstdint>

class UploadPipeline {
public:
    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------
    struct Config {
        DX12Context*     dx12         = nullptr;  // DX12 infrastructure
        FrameRingBuffer* ringBuffer   = nullptr;  // frame source (Consumer)
        VideoDecoder*    decoder      = nullptr;
        uint32_t         frameWidth   = 1920;
        uint32_t         frameHeight  = 1080;
    };

    UploadPipeline()  = default;
    ~UploadPipeline();

    // Non-copyable
    UploadPipeline(const UploadPipeline&)            = delete;
    UploadPipeline& operator=(const UploadPipeline&) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Create upload heap buffers (one per swap chain back buffer).
    /// Must be called after DX12Context::Initialize().
    void Initialize(const Config& cfg);

    /// Release all GPU resources.
    void Shutdown();

    /// Re-create upload buffers for a new frame size (e.g. on resize).
    void Resize(uint32_t newWidth, uint32_t newHeight);

    /// Reset the first-use flags for all back buffers.
    /// Call after DX12Context::ResizeBuffers() or swap chain recreation.
    void ResetFirstUseFlags();

    // -----------------------------------------------------------------------
    // Per-frame operation
    // -----------------------------------------------------------------------

    /// Attempt to upload one frame from the ring buffer to the current
    /// back buffer. Records all commands onto the provided command list.
    ///
    /// Call this BETWEEN DX12Context::BeginFrame() and EndFrame():
    ///
    ///   cmdList = dx12.BeginFrame();
    ///   bool uploaded = uploadPipeline.UploadFrame(cmdList);
    ///   dx12.EndFrame();
    ///   dx12.Present();
    ///   WaitUntilNextFrame();
    ///
    /// Returns true if a new frame was uploaded, false if the ring buffer
    /// was empty (previous frame stays on screen — no flicker).
    bool UploadFrame(ID3D12GraphicsCommandList* cmdList);

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    [[nodiscard]] uint32_t GetFrameWidth()  const noexcept { return m_frameWidth; }
    [[nodiscard]] uint32_t GetFrameHeight() const noexcept { return m_frameHeight; }

    /// Number of frames successfully uploaded since Initialize().
    [[nodiscard]] uint64_t GetUploadedFrameCount() const noexcept { return m_uploadedFrameCount; }

private:
    // Create a single upload buffer for the given back buffer index.
    void CreateUploadBuffer(uint32_t index);
    void InitializeGpuResources();
    void ReleaseGpuResources();
    void CreateSharedTextures();
    bool UploadGpuFrame(ID3D12GraphicsCommandList* cmdList);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    DX12Context*        m_dx12       = nullptr;
    FrameRingBuffer*    m_ringBuffer = nullptr;
    VideoDecoder*       m_decoder    = nullptr;
    VideoDecoder::DecodePath m_decodePath = VideoDecoder::DecodePath::CpuRgb32;

    // One upload buffer per swap chain back buffer
    // (can't overwrite an upload buffer while GPU is still reading it)
    std::array<ComPtr<ID3D12Resource>, DX12Context::MAX_BACK_BUFFERS> m_uploadBuffers{};

    // Persistently mapped pointers (upload heaps stay mapped for lifetime)
    std::array<uint8_t*, DX12Context::MAX_BACK_BUFFERS> m_mappedPtrs{};

    ComPtr<ID3D11VideoDevice> m_videoDevice;
    ComPtr<ID3D11VideoContext> m_videoContext;
    ComPtr<ID3D11Device5> m_d3d11Device5;
    ComPtr<ID3D11DeviceContext4> m_d3d11Context4;
    ComPtr<ID3D11VideoProcessorEnumerator> m_videoEnumerator;
    ComPtr<ID3D11VideoProcessor> m_videoProcessor;
    ComPtr<ID3D12Fence> m_sharedFence12;
    ComPtr<ID3D11Fence> m_sharedFence11;
    std::array<ComPtr<ID3D11Texture2D>, DX12Context::MAX_BACK_BUFFERS> m_sharedTextures11{};
    std::array<ComPtr<ID3D11VideoProcessorOutputView>, DX12Context::MAX_BACK_BUFFERS> m_outputViews{};
    std::array<ComPtr<ID3D12Resource>, DX12Context::MAX_BACK_BUFFERS> m_sharedTextures12{};
    uint64_t m_sharedFenceValue = 0;
    uint32_t m_outputWidth = 0;
    uint32_t m_outputHeight = 0;

    // D3D12 texture layout info (computed once, reused every frame)
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT  m_footprint{};
    uint64_t                            m_uploadBufferSize = 0;

    // Frame dimensions
    uint32_t    m_frameWidth    = 0;
    uint32_t    m_frameHeight   = 0;

    // Stats
    uint64_t    m_uploadedFrameCount = 0;

    bool        m_initialized   = false;

    // Per-back-buffer first-use tracking.
    // ★ SwapChain back buffers start in D3D12_RESOURCE_STATE_COMMON.
    //   On the first use of each buffer, we transition COMMON → COPY_DEST.
    //   On subsequent uses, we transition PRESENT → COPY_DEST.
    //   Reset on Initialize(), Resize(), and ResetFirstUseFlags().
    std::array<bool, DX12Context::MAX_BACK_BUFFERS> m_bufferFirstUse{};

    // Target texture format (matches VideoDecoder BGRX output)
    static constexpr DXGI_FORMAT UPLOAD_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM;
};
