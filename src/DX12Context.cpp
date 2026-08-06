// ============================================================================
// DX12Context.cpp — Module 2: D3D12 Device, Command Queue, SwapChain
// ============================================================================

#include "DX12Context.h"

#include <d3d12sdklayers.h>   // ID3D12Debug
#include <cassert>
#include <cstdio>

// Link libraries (MSVC pragma — also add to CMakeLists/vcxproj)
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// ============================================================================
// Destructor
// ============================================================================

DX12Context::~DX12Context() {
    Shutdown();
}

// ============================================================================
// Initialize — top-level orchestration
// ============================================================================

void DX12Context::Initialize(const Config& cfg) {
    if (m_initialized) {
        throw std::runtime_error("DX12Context::Initialize — already initialized");
    }
    if (!cfg.hwnd) {
        throw std::runtime_error("DX12Context::Initialize — HWND is null");
    }
    if (cfg.bufferCount < 2 || cfg.bufferCount > MAX_BACK_BUFFERS) {
        throw std::runtime_error("DX12Context::Initialize — bufferCount must be 2 or 3");
    }

    m_width            = cfg.width;
    m_height           = cfg.height;
    m_bufferCount      = cfg.bufferCount;
    m_backBufferFormat = cfg.backBufferFormat;
    m_debugLayer       = cfg.enableDebugLayer;

    // Initialize in dependency order
    CreateFactory();
    SelectAdapter();
    CreateDevice();
    CreateCommandQueue();
    CreateSwapChain(cfg.hwnd);
    CreateRTVHeap();
    CreateBackBufferRTVs();
    CreateCommandAllocatorsAndList();
    CreateFence();

    m_initialized = true;

    wprintf(L"[DX12Context] Initialized on adapter: %s\n",
            m_adapterDescription.c_str());
    printf("[DX12Context] Back buffer: %u x %u, %u buffers, FLIP_DISCARD\n",
           m_width, m_height, m_bufferCount);
}

// ============================================================================
// Shutdown — orderly teardown
// ============================================================================

void DX12Context::Shutdown() {
    if (!m_initialized) return;

    // Ensure the GPU is completely idle before releasing resources
    FlushGPU();

    // Close event handles
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    // Note: m_frameLatencyHandle is owned by the swap chain, do NOT close it.
    m_frameLatencyHandle = nullptr;

    // Release back buffer references (must happen before swap chain release)
    for (auto& buf : m_backBuffers) {
        buf.Reset();
    }

    // ComPtr releases in reverse order automatically via Reset
    m_commandList.Reset();
    for (auto& alloc : m_commandAllocators) {
        alloc.Reset();
    }
    m_fence.Reset();
    m_rtvHeap.Reset();
    m_swapChain.Reset();
    m_commandQueue.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_factory.Reset();

    m_initialized = false;
}

// ============================================================================
// CreateFactory
// ============================================================================

void DX12Context::CreateFactory() {
    UINT factoryFlags = 0;

    // Enable debug layer BEFORE creating the factory
    if (m_debugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            printf("[DX12Context] Debug layer enabled\n");
        }
    }

    ThrowIfFailed(
        CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)),
        "CreateDXGIFactory2"
    );
}

// ============================================================================
// SelectAdapter — pick the best GPU (prefer discrete, skip software)
// ============================================================================

void DX12Context::SelectAdapter() {
    ComPtr<IDXGIAdapter1> adapter1;

    // Try high-performance preference first (DXGI 1.6)
    for (UINT i = 0;
         m_factory->EnumAdapterByGpuPreference(
             i,
             DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter1)) != DXGI_ERROR_NOT_FOUND;
         ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter1->GetDesc1(&desc);

        // Skip software adapters (WARP)
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }

        // Check if this adapter supports D3D12 (feature level 11_0 minimum)
        if (SUCCEEDED(D3D12CreateDevice(
                adapter1.Get(),
                D3D_FEATURE_LEVEL_11_0,
                __uuidof(ID3D12Device),
                nullptr)))   // nullptr = don't actually create yet
        {
            ThrowIfFailed(
                adapter1.As(&m_adapter),
                "Adapter1 → Adapter4 QI"
            );
            m_adapterDescription = desc.Description;
            return;
        }
    }

    throw std::runtime_error(
        "DX12Context::SelectAdapter — no DX12-capable GPU found");
}

// ============================================================================
// CreateDevice
// ============================================================================

void DX12Context::CreateDevice() {
    ThrowIfFailed(
        D3D12CreateDevice(
            m_adapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&m_device)),
        "D3D12CreateDevice"
    );

    // Optional: configure debug info queue for break-on-error
    if (m_debugLayer) {
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(m_device.As(&infoQueue))) {
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,      TRUE);

            // Suppress noisy warnings if needed
            D3D12_MESSAGE_SEVERITY suppressSeverities[] = {
                D3D12_MESSAGE_SEVERITY_INFO
            };
            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumSeverities = _countof(suppressSeverities);
            filter.DenyList.pSeverityList = suppressSeverities;
            infoQueue->PushStorageFilter(&filter);
        }
    }
}

// ============================================================================
// CreateCommandQueue — single DIRECT queue for upload + present
// ============================================================================

void DX12Context::CreateCommandQueue() {
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    ThrowIfFailed(
        m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)),
        "CreateCommandQueue"
    );
}

// ============================================================================
// CreateSwapChain — FLIP_DISCARD with waitable object (core spec requirement)
// ============================================================================

void DX12Context::CreateSwapChain(HWND hwnd) {
    // ----- Swap chain descriptor per spec -----
    // REQUIRED:
    //   desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD  (zero-copy flip)
    //   desc.Flags      = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
    // PROHIBITED:
    //   DXGI_SWAP_EFFECT_DISCARD / SEQUENTIAL (old Blit Model)
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width              = m_width;
    desc.Height             = m_height;
    desc.Format             = m_backBufferFormat;
    desc.Stereo             = FALSE;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount        = m_bufferCount;
    desc.Scaling            = DXGI_SCALING_STRETCH;
    desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;   // ★ REQUIRED
    desc.AlphaMode          = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags              = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT; // ★ REQUIRED

    // Create swap chain for HWND (not CoreWindow)
    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(
        m_factory->CreateSwapChainForHwnd(
            m_commandQueue.Get(),   // DX12: swap chain takes command queue, not device
            hwnd,
            &desc,
            nullptr,                // no fullscreen desc
            nullptr,                // no restrict-to-output
            &swapChain1),
        "CreateSwapChainForHwnd"
    );

    // Disable Alt+Enter fullscreen toggle (we're a wallpaper, not a game)
    ThrowIfFailed(
        m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER),
        "MakeWindowAssociation"
    );

    // QI to IDXGISwapChain3 for GetCurrentBackBufferIndex()
    ThrowIfFailed(
        swapChain1.As(&m_swapChain),
        "SwapChain1 → SwapChain3 QI"
    );

    // Set maximum frame latency to 1 (minimal input lag, per spec target)
    ThrowIfFailed(
        m_swapChain->SetMaximumFrameLatency(1),
        "SetMaximumFrameLatency"
    );

    // Obtain the waitable handle — PresentLoop will WaitForSingleObjectEx on this
    m_frameLatencyHandle = m_swapChain->GetFrameLatencyWaitableObject();
    if (!m_frameLatencyHandle) {
        throw std::runtime_error(
            "DX12Context::CreateSwapChain — GetFrameLatencyWaitableObject returned null");
    }
}

// ============================================================================
// CreateRTVHeap — descriptor heap for Render Target Views
// ============================================================================

void DX12Context::CreateRTVHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = m_bufferCount;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heapDesc.NodeMask       = 0;

    ThrowIfFailed(
        m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap)),
        "CreateDescriptorHeap (RTV)"
    );

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

// ============================================================================
// CreateBackBufferRTVs — get swap chain buffers and create RTVs
// ============================================================================

void DX12Context::CreateBackBufferRTVs() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < m_bufferCount; ++i) {
        ThrowIfFailed(
            m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])),
            "SwapChain::GetBuffer"
        );

        m_device->CreateRenderTargetView(
            m_backBuffers[i].Get(),
            nullptr,        // default RTV desc
            rtvHandle
        );

        rtvHandle.ptr += m_rtvDescriptorSize;
    }
}

// ============================================================================
// CreateCommandAllocatorsAndList
// ============================================================================

void DX12Context::CreateCommandAllocatorsAndList() {
    // One allocator per back buffer (can't reset while GPU is using it)
    for (UINT i = 0; i < m_bufferCount; ++i) {
        ThrowIfFailed(
            m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&m_commandAllocators[i])),
            "CreateCommandAllocator"
        );
    }

    // Single command list, initially associated with allocator 0
    ThrowIfFailed(
        m_device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_commandAllocators[0].Get(),
            nullptr,    // no initial PSO
            IID_PPV_ARGS(&m_commandList)),
        "CreateCommandList"
    );

    // Command lists are created in the recording state — close it.
    // BeginFrame() will reset + re-open.
    ThrowIfFailed(m_commandList->Close(), "Initial CommandList::Close");
}

// ============================================================================
// CreateFence
// ============================================================================

void DX12Context::CreateFence() {
    ThrowIfFailed(
        m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)),
        "CreateFence"
    );

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        throw std::runtime_error("DX12Context::CreateFence — CreateEvent failed");
    }

    m_fenceValue = 0;
    m_frameFenceValues.fill(0);
}

// ============================================================================
// BeginFrame — wait for GPU, reset allocator/list for current back buffer
// ============================================================================

ID3D12GraphicsCommandList* DX12Context::BeginFrame() {
    const uint32_t idx = GetCurrentBackBufferIndex();

    // Wait until the GPU has finished with this buffer's previous frame
    WaitForFenceValue(m_frameFenceValues[idx]);

    // Reset the allocator (reclaims memory) and the command list
    ThrowIfFailed(
        m_commandAllocators[idx]->Reset(),
        "CommandAllocator::Reset"
    );
    ThrowIfFailed(
        m_commandList->Reset(m_commandAllocators[idx].Get(), nullptr),
        "CommandList::Reset"
    );

    return m_commandList.Get();
}

// ============================================================================
// EndFrame — close list, execute, signal fence
// ============================================================================

void DX12Context::EndFrame() {
    ThrowIfFailed(m_commandList->Close(), "CommandList::Close");

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // Advance fence and record which value this buffer must reach
    ++m_fenceValue;
    const uint32_t idx = GetCurrentBackBufferIndex();
    m_frameFenceValues[idx] = m_fenceValue;

    ThrowIfFailed(
        m_commandQueue->Signal(m_fence.Get(), m_fenceValue),
        "CommandQueue::Signal"
    );
}

// ============================================================================
// Present
// ============================================================================

void DX12Context::Present(uint32_t vSyncInterval) {
    // Per spec: swapChain->Present(1, 0) for VSYNC on
    ThrowIfFailed(
        m_swapChain->Present(vSyncInterval, 0),
        "SwapChain::Present"
    );
}

// ============================================================================
// ResizeBuffers — call on WM_DISPLAYCHANGE
// ============================================================================

void DX12Context::ResizeBuffers(uint32_t newWidth, uint32_t newHeight) {
    if (!m_initialized) return;
    if (newWidth == 0 || newHeight == 0) return;

    // Must flush before touching swap chain buffers
    FlushGPU();

    // Release existing back buffer references
    for (uint32_t i = 0; i < m_bufferCount; ++i) {
        m_backBuffers[i].Reset();
        m_frameFenceValues[i] = m_fenceValue;
    }

    // Resize
    ThrowIfFailed(
        m_swapChain->ResizeBuffers(
            m_bufferCount,
            newWidth,
            newHeight,
            m_backBufferFormat,
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),
        "SwapChain::ResizeBuffers"
    );

    m_width  = newWidth;
    m_height = newHeight;

    // Re-create RTVs for the new buffers
    CreateBackBufferRTVs();

    printf("[DX12Context] Resized to %u x %u\n", newWidth, newHeight);
}

// ============================================================================
// FlushGPU — wait until ALL submitted work is complete
// ============================================================================

void DX12Context::FlushGPU() {
    ++m_fenceValue;
    ThrowIfFailed(
        m_commandQueue->Signal(m_fence.Get(), m_fenceValue),
        "FlushGPU::Signal"
    );
    WaitForFenceValue(m_fenceValue);
}

// ============================================================================
// WaitForFenceValue
// ============================================================================

void DX12Context::WaitForFenceValue(uint64_t fenceValue) {
    if (m_fence->GetCompletedValue() < fenceValue) {
        ThrowIfFailed(
            m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent),
            "Fence::SetEventOnCompletion"
        );
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }
}

// ============================================================================
// Accessors
// ============================================================================

ID3D12Resource* DX12Context::GetCurrentBackBuffer() const {
    const uint32_t idx = GetCurrentBackBufferIndex();
    return m_backBuffers[idx].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Context::GetCurrentRTV() const {
    const uint32_t idx = GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(idx) * m_rtvDescriptorSize;
    return handle;
}

uint32_t DX12Context::GetCurrentBackBufferIndex() const {
    return m_swapChain->GetCurrentBackBufferIndex();
}
