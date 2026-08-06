// ============================================================================
// UploadPipeline.cpp — Module 5: CPU→GPU frame upload via Upload Heap
// ============================================================================

#include "UploadPipeline.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

// ============================================================================
// Destructor
// ============================================================================

UploadPipeline::~UploadPipeline() {
    Shutdown();
}

// ============================================================================
// Initialize
// ============================================================================

void UploadPipeline::Initialize(const Config& cfg) {
    if (m_initialized) {
        throw std::runtime_error("UploadPipeline::Initialize — already initialized");
    }
    if (!cfg.dx12 || !cfg.ringBuffer || !cfg.decoder) {
        throw std::runtime_error("UploadPipeline::Initialize — null dependency");
    }

    m_dx12       = cfg.dx12;
    m_ringBuffer = cfg.ringBuffer;
    m_decoder    = cfg.decoder;
    m_decodePath = m_decoder->GetDecodePath();
    m_frameWidth = cfg.frameWidth;
    m_frameHeight = cfg.frameHeight;
    m_outputWidth = m_dx12->GetWidth();
    m_outputHeight = m_dx12->GetHeight();

    if (m_decodePath == VideoDecoder::DecodePath::GpuDxva) {
        try {
            InitializeGpuResources();
            m_uploadedFrameCount = 0;
            m_bufferFirstUse.fill(true);
            m_initialized = true;
            printf("[UploadPipeline] Initialized GPU-only NV12->BGRA path\n");
            return;
        } catch (const std::exception& e) {
            ReleaseGpuResources();
            m_decoder->FallbackToCpu(e.what());
            m_decodePath = VideoDecoder::DecodePath::CpuRgb32;
            m_frameWidth = m_decoder->GetVideoInfo().width;
            m_frameHeight = m_decoder->GetVideoInfo().height;
        }
    }

    // Query the D3D12 texture layout for our frame dimensions.
    // This tells us the required RowPitch (aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256)
    // and the total upload buffer size.
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment          = 0;
    texDesc.Width              = m_frameWidth;
    texDesc.Height             = m_frameHeight;
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = UPLOAD_FORMAT;  // B8G8R8A8_UNORM
    texDesc.SampleDesc.Count   = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    UINT64 totalBytes = 0;
    m_dx12->GetDevice()->GetCopyableFootprints(
        &texDesc,
        0, 1,           // first subresource, count = 1
        0,              // base offset
        &m_footprint,
        nullptr,        // row count (not needed, we know it's m_frameHeight)
        nullptr,        // row size in bytes (not needed)
        &totalBytes
    );

    m_uploadBufferSize = totalBytes;

    printf("[UploadPipeline] Footprint: RowPitch=%u, TotalBytes=%llu "
           "(frame=%ux%u, format=B8G8R8A8)\n",
           m_footprint.Footprint.RowPitch,
           static_cast<unsigned long long>(totalBytes),
           m_frameWidth, m_frameHeight);

    // Create one upload buffer per swap chain back buffer
    const uint32_t bufferCount = m_dx12->GetBufferCount();
    for (uint32_t i = 0; i < bufferCount; ++i) {
        CreateUploadBuffer(i);
    }

    m_uploadedFrameCount = 0;
    m_bufferFirstUse.fill(true);  // all back buffers start in COMMON state
    m_initialized = true;

    printf("[UploadPipeline] Initialized with %u upload buffers\n", bufferCount);
}

// ============================================================================
// Shutdown
// ============================================================================

void UploadPipeline::Shutdown() {
    if (!m_initialized) return;

    if (m_decodePath == VideoDecoder::DecodePath::GpuDxva) {
        m_dx12->FlushGPU();
        ReleaseGpuResources();
    }

    // Unmap and release upload buffers
    const uint32_t bufferCount = m_dx12 ? m_dx12->GetBufferCount() : 0;
    for (uint32_t i = 0; i < bufferCount; ++i) {
        if (m_uploadBuffers[i]) {
            // Unmap (pass nullptr range = entire resource)
            m_uploadBuffers[i]->Unmap(0, nullptr);
            m_mappedPtrs[i] = nullptr;
            m_uploadBuffers[i].Reset();
        }
    }

    m_dx12       = nullptr;
    m_ringBuffer = nullptr;
    m_decoder    = nullptr;
    m_initialized = false;
}

// ============================================================================
// Resize — re-create upload buffers for a new frame size
// ============================================================================

void UploadPipeline::Resize(uint32_t newWidth, uint32_t newHeight) {
    if (!m_initialized) return;

    if (m_decodePath == VideoDecoder::DecodePath::GpuDxva) {
        if (newWidth == m_outputWidth && newHeight == m_outputHeight) return;
        m_dx12->FlushGPU();
        m_ringBuffer->RecycleCompletedGpuFrames(m_dx12->GetCompletedFenceValue());
        ReleaseGpuResources();
        m_outputWidth = newWidth;
        m_outputHeight = newHeight;
        InitializeGpuResources();
        m_bufferFirstUse.fill(true);
        return;
    }

    if (newWidth == m_frameWidth && newHeight == m_frameHeight) return;

    printf("[UploadPipeline] Resizing upload buffers: %ux%u → %ux%u\n",
           m_frameWidth, m_frameHeight, newWidth, newHeight);

    // Release existing upload buffers
    const uint32_t bufferCount = m_dx12->GetBufferCount();
    for (uint32_t i = 0; i < bufferCount; ++i) {
        if (m_uploadBuffers[i]) {
            m_uploadBuffers[i]->Unmap(0, nullptr);
            m_mappedPtrs[i] = nullptr;
            m_uploadBuffers[i].Reset();
        }
    }

    m_frameWidth  = newWidth;
    m_frameHeight = newHeight;

    // Recompute footprint
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width              = m_frameWidth;
    texDesc.Height             = m_frameHeight;
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = UPLOAD_FORMAT;
    texDesc.SampleDesc.Count   = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    UINT64 totalBytes = 0;
    m_dx12->GetDevice()->GetCopyableFootprints(
        &texDesc, 0, 1, 0,
        &m_footprint, nullptr, nullptr, &totalBytes
    );
    m_uploadBufferSize = totalBytes;

    // Re-create upload buffers
    for (uint32_t i = 0; i < bufferCount; ++i) {
        CreateUploadBuffer(i);
    }

    // After resize, back buffers are in COMMON state again
    m_bufferFirstUse.fill(true);
}

// ============================================================================
// UploadFrame — the core per-frame operation
// ============================================================================

bool UploadPipeline::UploadFrame(ID3D12GraphicsCommandList* cmdList) {
    assert(m_initialized && "UploadPipeline not initialized");
    assert(cmdList && "cmdList is null");

    if (m_decodePath == VideoDecoder::DecodePath::GpuDxva) {
        return UploadGpuFrame(cmdList);
    }

    // ----- Step 1: Read from ring buffer (Consumer: TryPeekRead) -----
    auto readSlot = m_ringBuffer->TryPeekRead();
    if (!readSlot.ptr) {
        // No frame available — the previous frame stays on screen (no flicker).
        // Still need to transition back buffer for Present to work correctly.
        return false;
    }

    const FrameInfo& info = readSlot.info;
    const uint32_t backBufIdx = m_dx12->GetCurrentBackBufferIndex();

    // ----- Step 2: memcpy ring buffer → upload heap (row-by-row) -----
    //
    // ★ CRITICAL: Stride 可能含 row padding
    //   - Source stride:  info.stride（VideoDecoder 設定，可能含 padding）
    //   - Dest row pitch: m_footprint.Footprint.RowPitch（D3D12 aligned to 256）
    //   - 這兩個值通常不同，因此必須逐行拷貝
    //   - 總拷貝量基於 info.stride * info.height，不能用 width * 4 * height
    //
    uint8_t* dstBase = m_mappedPtrs[backBufIdx] + m_footprint.Offset;
    const uint8_t* srcBase = readSlot.ptr;

    const uint32_t srcStride   = info.stride;                          // from VideoDecoder
    const uint32_t dstRowPitch = m_footprint.Footprint.RowPitch;      // D3D12 aligned
    const uint32_t rowBytes    = (std::min)(srcStride, dstRowPitch);   // actual pixel data per row
    const uint32_t rowCount    = info.height;

    if (srcStride == dstRowPitch) {
        // Fast path: strides match, single memcpy
        // Total bytes = info.stride * info.height (NOT width * 4 * height)
        std::memcpy(dstBase, srcBase,
                    static_cast<size_t>(srcStride) * rowCount);
    } else {
        // Slow path: row-by-row copy to handle stride mismatch
        for (uint32_t row = 0; row < rowCount; ++row) {
            std::memcpy(
                dstBase + static_cast<size_t>(row) * dstRowPitch,
                srcBase + static_cast<size_t>(row) * srcStride,
                rowBytes
            );
        }
    }

    // ----- Step 3: Barrier — back buffer PRESENT/COMMON → COPY_DEST -----
    //
    // ★ 第一幀特殊處理：SwapChain 建立/Resize 後，back buffer 的初始狀態為
    //   D3D12_RESOURCE_STATE_COMMON，而非 PRESENT。首次使用每個 back buffer
    //   時必須從 COMMON 轉換，後續從 PRESENT 轉換。
    //
    ID3D12Resource* backBuffer = m_dx12->GetCurrentBackBuffer();

    const D3D12_RESOURCE_STATES stateBefore =
        m_bufferFirstUse[backBufIdx]
            ? D3D12_RESOURCE_STATE_COMMON
            : D3D12_RESOURCE_STATE_PRESENT;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = backBuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = stateBefore;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;

    cmdList->ResourceBarrier(1, &barrier);

    // ----- Step 4: CopyTextureRegion — upload heap → back buffer (DMA) -----
    //
    // This is a DMA transfer that doesn't occupy shader cores.
    // Source: upload buffer with the placed footprint layout
    // Dest:   back buffer (B8G8R8A8_UNORM, same format = direct copy)

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource       = m_uploadBuffers[backBufIdx].Get();
    srcLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = m_footprint;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource        = backBuffer;
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // ----- Step 5: Barrier — back buffer COPY_DEST → PRESENT -----
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;

    cmdList->ResourceBarrier(1, &barrier);

    // ----- Step 6: Commit read — release ring buffer slot -----
    m_ringBuffer->CommitRead();

    // Mark this back buffer as no longer first-use
    m_bufferFirstUse[backBufIdx] = false;

    ++m_uploadedFrameCount;
    return true;
}

void UploadPipeline::InitializeGpuResources() {
    ID3D11Device* device11 = m_decoder->GetD3D11Device();
    ID3D11DeviceContext* context11 = m_decoder->GetD3D11Context();
    if (!device11 || !context11) {
        throw std::runtime_error("GPU decoder did not expose a D3D11 device");
    }

    ThrowIfFailed(device11->QueryInterface(IID_PPV_ARGS(&m_videoDevice)),
                  "QI ID3D11VideoDevice");
    ThrowIfFailed(context11->QueryInterface(IID_PPV_ARGS(&m_videoContext)),
                  "QI ID3D11VideoContext");
    ThrowIfFailed(device11->QueryInterface(IID_PPV_ARGS(&m_d3d11Device5)),
                  "QI ID3D11Device5");
    ThrowIfFailed(context11->QueryInterface(IID_PPV_ARGS(&m_d3d11Context4)),
                  "QI ID3D11DeviceContext4");

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.InputWidth = m_frameWidth;
    content.InputHeight = m_frameHeight;
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = m_outputWidth;
    content.OutputHeight = m_outputHeight;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ThrowIfFailed(m_videoDevice->CreateVideoProcessorEnumerator(
        &content, &m_videoEnumerator), "CreateVideoProcessorEnumerator");

    UINT inputFlags = 0;
    UINT outputFlags = 0;
    ThrowIfFailed(m_videoEnumerator->CheckVideoProcessorFormat(
        DXGI_FORMAT_NV12, &inputFlags), "CheckVideoProcessorFormat(NV12)");
    ThrowIfFailed(m_videoEnumerator->CheckVideoProcessorFormat(
        UPLOAD_FORMAT, &outputFlags), "CheckVideoProcessorFormat(BGRA)");
    if (!(inputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)
        || !(outputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        throw std::runtime_error("D3D11 video processor lacks NV12/BGRA support");
    }
    ThrowIfFailed(m_videoDevice->CreateVideoProcessor(
        m_videoEnumerator.Get(), 0, &m_videoProcessor), "CreateVideoProcessor");

    ThrowIfFailed(m_dx12->GetDevice()->CreateFence(
        0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_sharedFence12)),
        "CreateFence(shared D3D11/D3D12)");
    HANDLE fenceHandle = nullptr;
    ThrowIfFailed(m_dx12->GetDevice()->CreateSharedHandle(
        m_sharedFence12.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle),
        "CreateSharedHandle(fence)");
    const HRESULT openFenceHr = m_d3d11Device5->OpenSharedFence(
        fenceHandle, IID_PPV_ARGS(&m_sharedFence11));
    CloseHandle(fenceHandle);
    ThrowIfFailed(openFenceHr, "ID3D11Device5::OpenSharedFence");

    CreateSharedTextures();
    m_sharedFenceValue = 0;
    printf("[UploadPipeline] GPU resources: NV12 %ux%u -> BGRA %ux%u\n",
           m_frameWidth, m_frameHeight, m_outputWidth, m_outputHeight);
}

void UploadPipeline::CreateSharedTextures() {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_outputWidth;
    desc.Height = m_outputHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = UPLOAD_FORMAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC viewDesc{};
    viewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

    for (uint32_t i = 0; i < m_dx12->GetBufferCount(); ++i) {
        ThrowIfFailed(m_dx12->GetDevice()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&m_sharedTextures12[i])),
            "CreateCommittedResource(shared BGRA)");
        HANDLE textureHandle = nullptr;
        ThrowIfFailed(m_dx12->GetDevice()->CreateSharedHandle(
            m_sharedTextures12[i].Get(), nullptr, GENERIC_ALL, nullptr, &textureHandle),
            "ID3D12Device::CreateSharedHandle(texture)");
        const HRESULT openTextureHr = m_d3d11Device5->OpenSharedResource1(
            textureHandle, IID_PPV_ARGS(&m_sharedTextures11[i]));
        CloseHandle(textureHandle);
        ThrowIfFailed(openTextureHr, "ID3D11Device1::OpenSharedResource1(texture)");
        ThrowIfFailed(m_videoDevice->CreateVideoProcessorOutputView(
            m_sharedTextures11[i].Get(), m_videoEnumerator.Get(),
            &viewDesc, &m_outputViews[i]), "CreateVideoProcessorOutputView");
    }
}

void UploadPipeline::ReleaseGpuResources() {
    if (m_d3d11Context4) m_d3d11Context4->Flush();
    for (auto& view : m_outputViews) view.Reset();
    for (auto& texture : m_sharedTextures12) texture.Reset();
    for (auto& texture : m_sharedTextures11) texture.Reset();
    m_sharedFence11.Reset();
    m_sharedFence12.Reset();
    m_videoProcessor.Reset();
    m_videoEnumerator.Reset();
    m_d3d11Context4.Reset();
    m_d3d11Device5.Reset();
    m_videoContext.Reset();
    m_videoDevice.Reset();
    m_sharedFenceValue = 0;
}

bool UploadPipeline::UploadGpuFrame(ID3D12GraphicsCommandList* cmdList) {
    m_ringBuffer->RecycleCompletedGpuFrames(m_dx12->GetCompletedFenceValue());
    const auto readSlot = m_ringBuffer->TryPeekRead();
    if (!readSlot.sample) return false;

    ComPtr<IMFMediaBuffer> mediaBuffer;
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    ComPtr<ID3D11Texture2D> inputTexture;
    ThrowIfFailed(readSlot.sample->GetBufferByIndex(0, &mediaBuffer),
                  "GPU frame GetBufferByIndex");
    ThrowIfFailed(mediaBuffer.As(&dxgiBuffer), "GPU frame QI IMFDXGIBuffer");
    ThrowIfFailed(dxgiBuffer->GetResource(IID_PPV_ARGS(&inputTexture)),
                  "GPU frame GetResource");
    UINT subresource = 0;
    ThrowIfFailed(dxgiBuffer->GetSubresourceIndex(&subresource),
                  "GPU frame GetSubresourceIndex");
    D3D11_TEXTURE2D_DESC inputDesc{};
    inputTexture->GetDesc(&inputDesc);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.MipSlice = subresource % inputDesc.MipLevels;
    inputViewDesc.Texture2D.ArraySlice = subresource / inputDesc.MipLevels;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    ThrowIfFailed(m_videoDevice->CreateVideoProcessorInputView(
        inputTexture.Get(), m_videoEnumerator.Get(), &inputViewDesc, &inputView),
        "CreateVideoProcessorInputView");

    const RECT sourceRect{0, 0, static_cast<LONG>(readSlot.info.width),
                          static_cast<LONG>(readSlot.info.height)};
    const RECT outputRect{0, 0, static_cast<LONG>(m_outputWidth),
                          static_cast<LONG>(m_outputHeight)};
    m_videoContext->VideoProcessorSetStreamFrameFormat(
        m_videoProcessor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    m_videoContext->VideoProcessorSetStreamSourceRect(
        m_videoProcessor.Get(), 0, TRUE, &sourceRect);
    m_videoContext->VideoProcessorSetStreamDestRect(
        m_videoProcessor.Get(), 0, TRUE, &outputRect);
    m_videoContext->VideoProcessorSetOutputTargetRect(
        m_videoProcessor.Get(), TRUE, &outputRect);

    const uint32_t backBufferIndex = m_dx12->GetCurrentBackBufferIndex();
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    ThrowIfFailed(m_videoContext->VideoProcessorBlt(
        m_videoProcessor.Get(), m_outputViews[backBufferIndex].Get(),
        static_cast<UINT>(m_uploadedFrameCount), 1, &stream),
        "VideoProcessorBlt");

    const uint64_t sharedFenceValue = ++m_sharedFenceValue;
    ThrowIfFailed(m_d3d11Context4->Signal(m_sharedFence11.Get(), sharedFenceValue),
                  "ID3D11DeviceContext4::Signal");
    // ponytail: per-frame flush guarantees AMD interop progress; batch only if profiling proves it costly.
    m_d3d11Context4->Flush();
    ThrowIfFailed(m_dx12->GetCommandQueue()->Wait(
        m_sharedFence12.Get(), sharedFenceValue), "ID3D12CommandQueue::Wait(shared fence)");

    ID3D12Resource* backBuffer = m_dx12->GetCurrentBackBuffer();
    ID3D12Resource* sharedTexture = m_sharedTextures12[backBufferIndex].Get();
    const D3D12_RESOURCE_STATES backBufferBefore = m_bufferFirstUse[backBufferIndex]
        ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_PRESENT;
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (auto& barrier : barriers) barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = {sharedTexture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {backBuffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              backBufferBefore, D3D12_RESOURCE_STATE_COPY_DEST};
    cmdList->ResourceBarrier(2, barriers);

    D3D12_TEXTURE_COPY_LOCATION src{sharedTexture,
        D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION dst{backBuffer,
        D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmdList->ResourceBarrier(2, barriers);

    m_ringBuffer->CommitGpuRead(m_dx12->GetNextFenceValue());
    m_bufferFirstUse[backBufferIndex] = false;
    ++m_uploadedFrameCount;
    return true;
}

// ============================================================================
// ResetFirstUseFlags — call after ResizeBuffers or swap chain recreation
// ============================================================================

void UploadPipeline::ResetFirstUseFlags() {
    m_bufferFirstUse.fill(true);
    printf("[UploadPipeline] First-use flags reset (back buffers assumed COMMON)\n");
}

// ============================================================================
// CreateUploadBuffer — allocate a single upload heap buffer
// ============================================================================

void UploadPipeline::CreateUploadBuffer(uint32_t index) {
    assert(index < DX12Context::MAX_BACK_BUFFERS);

    auto* device = m_dx12->GetDevice();

    // Upload heap properties — CPU can write directly, GPU can read directly
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type                 = D3D12_HEAP_TYPE_UPLOAD;  // ★ per spec
    heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask     = 0;
    heapProps.VisibleNodeMask      = 0;

    // Upload buffers are created as BUFFER (not texture) — D3D12 requirement
    // for CopyTextureRegion source from upload heap.
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Alignment          = 0;
    bufDesc.Width              = m_uploadBufferSize;
    bufDesc.Height             = 1;
    bufDesc.DepthOrArraySize   = 1;
    bufDesc.MipLevels          = 1;
    bufDesc.Format             = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count   = 1;
    bufDesc.SampleDesc.Quality = 0;
    bufDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    ThrowIfFailed(
        device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,  // upload heaps start as GENERIC_READ
            nullptr,                             // no clear value for buffers
            IID_PPV_ARGS(&m_uploadBuffers[index])),
        "CreateCommittedResource (upload buffer)"
    );

    // Persistently map — upload heaps can stay mapped for their entire lifetime.
    // This avoids the overhead of Map/Unmap per frame.
    D3D12_RANGE readRange{0, 0};  // we don't read back from GPU
    void* mapped = nullptr;
    ThrowIfFailed(
        m_uploadBuffers[index]->Map(0, &readRange, &mapped),
        "Upload buffer Map"
    );
    m_mappedPtrs[index] = static_cast<uint8_t*>(mapped);

    printf("[UploadPipeline] Upload buffer [%u]: %llu bytes, mapped at %p\n",
           index, static_cast<unsigned long long>(m_uploadBufferSize), mapped);
}
