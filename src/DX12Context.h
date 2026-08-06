#pragma once
// ============================================================================
// DX12Context.h — Module 2: D3D12 Device, Command Queue, SwapChain (Flip Model)
// ============================================================================
//
// 職責：初始化 D3D12 設備、命令佇列、SwapChain（Flip Model）
//
// Key design decisions (per spec):
//   - DXGI_SWAP_EFFECT_FLIP_DISCARD   → zero-copy pointer swap, DWM 不額外複製
//   - DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT → 精確 FPS 控制
//   - Double buffering (BufferCount = 2)
//   - Single DIRECT command queue (sufficient for texture upload + present)
//
// Thread safety:
//   - Initialize() and Shutdown() from the render thread only.
//   - GetFrameLatencyWaitableObject() is thread-safe after initialization.
//   - BeginFrame / EndFrame / Present must be called from the render thread.
// ============================================================================

#include "Common.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <array>
#include <cstdint>
#include <string>

class DX12Context {
public:
    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------
    struct Config {
        HWND        hwnd            = nullptr;
        uint32_t    width           = 1920;
        uint32_t    height          = 1080;
        uint32_t    bufferCount     = 2;        // double buffering
        DXGI_FORMAT backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;  // matches MF RGB32 (BGRX)
        bool        enableDebugLayer = false;   // set true for development
    };

    static constexpr uint32_t MAX_BACK_BUFFERS = 3; // compile-time max

    DX12Context()  = default;
    ~DX12Context();

    // Non-copyable
    DX12Context(const DX12Context&)            = delete;
    DX12Context& operator=(const DX12Context&) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Full initialization: factory → adapter → device → queue → swap chain
    /// → RTV heap → command allocators → fence.
    void Initialize(const Config& cfg);

    /// Orderly teardown: flush GPU, release all resources.
    void Shutdown();

    /// Resize swap chain buffers (e.g. on WM_DISPLAYCHANGE).
    void ResizeBuffers(uint32_t newWidth, uint32_t newHeight);

    // -----------------------------------------------------------------------
    // Per-frame operations
    // -----------------------------------------------------------------------

    /// Wait on fence for the current back buffer, reset command allocator & list.
    /// Returns the command list ready for recording.
    ID3D12GraphicsCommandList* BeginFrame();

    /// Close the command list, execute it, and signal the fence.
    void EndFrame();

    /// Call SwapChain::Present. vSyncInterval=1 for VSYNC on.
    void Present(uint32_t vSyncInterval = 1);

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    [[nodiscard]] ID3D12Device*             GetDevice()         const noexcept { return m_device.Get(); }
    [[nodiscard]] IDXGIAdapter4*             GetAdapter()        const noexcept { return m_adapter.Get(); }
    [[nodiscard]] ID3D12CommandQueue*        GetCommandQueue()   const noexcept { return m_commandQueue.Get(); }
    [[nodiscard]] IDXGISwapChain3*           GetSwapChain()      const noexcept { return m_swapChain.Get(); }
    [[nodiscard]] ID3D12GraphicsCommandList* GetCommandList()    const noexcept { return m_commandList.Get(); }

    [[nodiscard]] ID3D12Resource*    GetCurrentBackBuffer()  const;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV() const;
    [[nodiscard]] uint32_t          GetCurrentBackBufferIndex() const;

    [[nodiscard]] uint32_t          GetWidth()              const noexcept { return m_width; }
    [[nodiscard]] uint32_t          GetHeight()             const noexcept { return m_height; }
    [[nodiscard]] uint32_t          GetBufferCount()        const noexcept { return m_bufferCount; }
    [[nodiscard]] DXGI_FORMAT       GetBackBufferFormat()   const noexcept { return m_backBufferFormat; }

    /// Returns the waitable handle for PresentLoop to WaitForSingleObjectEx on.
    [[nodiscard]] HANDLE GetFrameLatencyWaitableObject() const noexcept { return m_frameLatencyHandle; }
    [[nodiscard]] uint64_t GetCompletedFenceValue() const noexcept {
        return m_fence ? m_fence->GetCompletedValue() : 0;
    }
    [[nodiscard]] uint64_t GetNextFenceValue() const noexcept { return m_fenceValue + 1; }

    /// Blocks CPU until the GPU has finished all submitted work.
    void FlushGPU();

private:
    // Initialization sub-steps
    void CreateFactory();
    void SelectAdapter();
    void CreateDevice();
    void CreateCommandQueue();
    void CreateSwapChain(HWND hwnd);
    void CreateRTVHeap();
    void CreateBackBufferRTVs();
    void CreateCommandAllocatorsAndList();
    void CreateFence();

    // Wait until a specific fence value has been reached by the GPU.
    void WaitForFenceValue(uint64_t fenceValue);

    // -----------------------------------------------------------------------
    // Core objects
    // -----------------------------------------------------------------------
    ComPtr<IDXGIFactory6>               m_factory;
    ComPtr<IDXGIAdapter4>               m_adapter;
    ComPtr<ID3D12Device>                m_device;
    ComPtr<ID3D12CommandQueue>           m_commandQueue;
    ComPtr<IDXGISwapChain3>             m_swapChain;

    // RTV (Render Target View) descriptor heap
    ComPtr<ID3D12DescriptorHeap>        m_rtvHeap;
    uint32_t                            m_rtvDescriptorSize = 0;

    // Per-buffer resources
    std::array<ComPtr<ID3D12Resource>,          MAX_BACK_BUFFERS> m_backBuffers{};
    std::array<ComPtr<ID3D12CommandAllocator>,   MAX_BACK_BUFFERS> m_commandAllocators{};

    // Single command list (reset per frame with the appropriate allocator)
    ComPtr<ID3D12GraphicsCommandList>   m_commandList;

    // Fence for CPU-GPU synchronization
    ComPtr<ID3D12Fence>                 m_fence;
    HANDLE                              m_fenceEvent        = nullptr;
    uint64_t                            m_fenceValue        = 0;
    std::array<uint64_t, MAX_BACK_BUFFERS> m_frameFenceValues{};

    // Frame latency waitable object (per spec: no busy-wait)
    HANDLE                              m_frameLatencyHandle = nullptr;

    // Configuration snapshot
    uint32_t    m_width             = 0;
    uint32_t    m_height            = 0;
    uint32_t    m_bufferCount       = 2;
    DXGI_FORMAT m_backBufferFormat  = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool        m_debugLayer        = false;
    bool        m_initialized       = false;

    // Adapter info (for logging)
    std::wstring m_adapterDescription;
};
