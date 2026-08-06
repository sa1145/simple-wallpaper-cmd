// ============================================================================
// VideoDecoder.cpp — Module 4: CPU-side video decoding via Media Foundation
// ============================================================================

#include "VideoDecoder.h"

#include <d3d10.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>

// Link MF libraries (also add to CMakeLists/vcxproj)
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "d3d11.lib")

// MF timestamps are in 100-nanosecond units
static constexpr double HNS_TO_SECONDS = 1.0 / 10'000'000.0;
static constexpr LONGLONG SECONDS_TO_HNS = 10'000'000LL;

// ============================================================================
// Destructor
// ============================================================================

VideoDecoder::~VideoDecoder() {
    Stop();
    Close();
}

// ============================================================================
// Open — initialize MF, create source reader, configure output
// ============================================================================

void VideoDecoder::Open(const Config& cfg) {
    if (!cfg.ringBuffer) {
        throw std::runtime_error("VideoDecoder::Open — ringBuffer is null");
    }
    if (cfg.filePath.empty()) {
        throw std::runtime_error("VideoDecoder::Open — filePath is empty");
    }

    m_ringBuffer = cfg.ringBuffer;
    m_adapter = cfg.adapter;
    m_allowSoftwareFallback = cfg.allowSoftwareFallback;
    m_filePath = cfg.filePath;
    m_targetFPS = cfg.targetFPS > 0 ? cfg.targetFPS : 60;
    m_minFrameIntervalHns = SECONDS_TO_HNS / m_targetFPS;
    m_nextFrameTimestampHns = -1;
    m_timestampOffsetHns = 0;
    m_cpuCopiedFrameCount.store(0, std::memory_order_relaxed);

    InitializeMF();

    if (m_adapter) {
        try {
            CreateD3D11DeviceManager();
            CreateSourceReader(m_filePath, DecodePath::GpuDxva);
            ConfigureOutputType(DecodePath::GpuDxva);
            m_decodePath = DecodePath::GpuDxva;
            QueryVideoInfo();
            ProbeGpuSample();
            m_ringBuffer->PrepareGpu();
            printf("[VideoDecoder] Decode path: GPU-DXVA\n");
        } catch (const std::exception& e) {
            if (!m_allowSoftwareFallback) throw;
            OpenCpuPath(e.what());
        }
    } else if (m_allowSoftwareFallback) {
        OpenCpuPath("D3D12 adapter not provided");
    } else {
        throw std::runtime_error("VideoDecoder::Open — D3D12 adapter not provided");
    }

    printf("[VideoDecoder] Opened: %u x %u, %.2f FPS, %.2f sec, ~%llu frames\n",
           m_videoInfo.width, m_videoInfo.height,
           m_videoInfo.fps, m_videoInfo.durationSec,
           static_cast<unsigned long long>(m_videoInfo.frameCount));
}

// ============================================================================
// Close
// ============================================================================

void VideoDecoder::Close() {
    Stop();  // ensure thread is stopped first

    m_pendingSample.Reset();
    m_sourceReader.Reset();
    m_dxgiDeviceManager.Reset();
    m_d3d11Context.Reset();
    m_d3d11Device.Reset();

    ShutdownMF();

    m_ringBuffer    = nullptr;
    m_adapter       = nullptr;
    m_filePath.clear();
    m_videoInfo     = {};
    m_nextFrameTimestampHns = -1;
    m_timestampOffsetHns = 0;
}

void VideoDecoder::FallbackToCpu(const char* reason) {
    if (m_running.load(std::memory_order_acquire)) {
        throw std::runtime_error("VideoDecoder::FallbackToCpu — decoder already running");
    }
    if (!m_allowSoftwareFallback) {
        throw std::runtime_error(reason ? reason : "CPU fallback disabled");
    }
    OpenCpuPath(reason ? reason : "GPU path unavailable");
}

// ============================================================================
// Start — launch the decode thread
// ============================================================================

void VideoDecoder::Start() {
    if (m_running.load(std::memory_order_acquire)) {
        return;  // already running
    }
    if (!m_sourceReader) {
        throw std::runtime_error("VideoDecoder::Start — not opened");
    }

    m_stopRequested.store(false, std::memory_order_relaxed);
    m_pauseRequested.store(false, std::memory_order_relaxed);
    m_paused.store(false, std::memory_order_relaxed);
    m_endOfStream.store(false, std::memory_order_relaxed);
    m_playbackClockStart = std::chrono::steady_clock::now();
    m_running.store(true, std::memory_order_release);

    m_decodeThread = std::thread(&VideoDecoder::DecodeThreadFunc, this);

    printf("[VideoDecoder] Decode thread started\n");
}

// ============================================================================
// Stop — signal and join the decode thread
// ============================================================================

void VideoDecoder::Stop() {
    if (!m_running.load(std::memory_order_acquire)) {
        return;
    }

    // Unblock any pause wait first
    m_stopRequested.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_pauseMutex);
        m_pauseRequested.store(false, std::memory_order_release);
    }
    m_pauseCV.notify_all();

    if (m_decodeThread.joinable()) {
        m_decodeThread.join();
    }

    m_running.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);

    printf("[VideoDecoder] Decode thread stopped\n");
}

// ============================================================================
// Pause — request pause and BLOCK until decode thread confirms
// ============================================================================

void VideoDecoder::Pause() {
    if (!m_running.load(std::memory_order_acquire)) return;
    if (m_paused.load(std::memory_order_acquire))   return;  // already paused

    // Request pause
    m_pauseRequested.store(true, std::memory_order_release);

    // Wait for the decode thread to acknowledge (it sets m_paused = true)
    std::unique_lock<std::mutex> lock(m_pauseConfirmMutex);
    m_pauseConfirmCV.wait(lock, [this] {
        return m_paused.load(std::memory_order_acquire);
    });
    m_pauseStartedAt = std::chrono::steady_clock::now();

    printf("[VideoDecoder] Decode thread paused\n");
}

// ============================================================================
// Resume — wake up the paused decode thread
// ============================================================================

void VideoDecoder::Resume() {
    if (!m_running.load(std::memory_order_acquire)) return;
    if (!m_paused.load(std::memory_order_acquire))  return;  // not paused

    m_playbackClockStart += std::chrono::steady_clock::now() - m_pauseStartedAt;
    {
        std::lock_guard<std::mutex> lock(m_pauseMutex);
        m_pauseRequested.store(false, std::memory_order_release);
        m_paused.store(false, std::memory_order_release);
    }
    m_pauseCV.notify_one();

    printf("[VideoDecoder] Decode thread resumed\n");
}

// ============================================================================
// Seek — reposition source reader (MUST be called while paused)
// ============================================================================

void VideoDecoder::Seek(double positionSeconds) {
    // ★ PRECONDITION: decode thread must be paused
    assert(m_paused.load(std::memory_order_acquire) &&
           "VideoDecoder::Seek — DecodeThread must be paused before seeking");

    if (!m_sourceReader) {
        throw std::runtime_error("VideoDecoder::Seek — not opened");
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt    = VT_I8;
    var.hVal.QuadPart = static_cast<LONGLONG>(positionSeconds * SECONDS_TO_HNS);

    HRESULT hr = m_sourceReader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);

    ThrowIfFailed(hr, "SourceReader::SetCurrentPosition");

    // Clear end-of-stream flag so decoding can continue
    m_endOfStream.store(false, std::memory_order_release);
    m_nextFrameTimestampHns = static_cast<LONGLONG>(positionSeconds * SECONDS_TO_HNS);
    m_timestampOffsetHns = m_nextFrameTimestampHns;
    m_playbackClockStart = std::chrono::steady_clock::now()
        - std::chrono::nanoseconds(m_nextFrameTimestampHns * 100);
    m_pauseStartedAt = std::chrono::steady_clock::now();

    printf("[VideoDecoder] Seeked to %.3f sec\n", positionSeconds);
}

// ============================================================================
// DecodeThreadFunc — main loop running on the decode thread
// ============================================================================

void VideoDecoder::DecodeThreadFunc() {
    printf("[VideoDecoder] DecodeThread enter\n");

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

            if (m_stopRequested.load(std::memory_order_acquire)) {
                break;
            }
            continue;  // re-check conditions after wakeup
        }

        // --- End of stream: signal callback, then wait ---
        if (m_endOfStream.load(std::memory_order_acquire)) {
            // Don't busy-wait; sleep briefly and re-check
            // (The main thread will Pause → Reset → Seek → Resume us)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // --- Decode one sample and push to ring buffer ---
        if (!DecodeOneSample()) {
            // EOS reached — notify callback
            m_endOfStream.store(true, std::memory_order_release);

            if (m_eosCallback) {
                m_eosCallback();
            }
        }
    }

    printf("[VideoDecoder] DecodeThread exit\n");
}

// ============================================================================
// DecodeOneSample — read one frame, convert, push to ring buffer
// ============================================================================

bool VideoDecoder::DecodeOneSample() {
    FrameRingBuffer::WriteSlot writeSlot{};
    if (m_decodePath == DecodePath::GpuDxva) {
        if (!m_ringBuffer->CanWriteGpu()) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            return true;
        }
    } else {
        writeSlot = m_ringBuffer->TryAcquireWrite();
        if (!writeSlot.ptr) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            return true;
        }
    }

    DWORD streamIndex = 0;
    DWORD flags       = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;

    HRESULT hr = S_OK;
    if (m_pendingSample) {
        sample = std::move(m_pendingSample);
        flags = m_pendingFlags;
        timestamp = m_pendingTimestamp;
        m_pendingFlags = 0;
        m_pendingTimestamp = 0;
    } else {
        hr = m_sourceReader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            0, &streamIndex, &flags, &timestamp, &sample);
    }

    if (FAILED(hr)) {
        printf("[VideoDecoder] ReadSample failed: 0x%08X\n", static_cast<unsigned>(hr));
        return false;
    }

    // Check for end-of-stream
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        return false;
    }

    // ★ 修正（BUG 2 + BUG 3）：Media type change — 重新查詢格式、檢查 ring buffer 容量、跳過此 sample
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        printf("[VideoDecoder] Media type changed, re-querying info\n");
        QueryVideoInfo();

        if (m_decodePath == DecodePath::CpuRgb32) {
            const size_t newFrameBytes =
                static_cast<size_t>(m_videoInfo.stride) * m_videoInfo.height;
            if (newFrameBytes > m_ringBuffer->GetMaxFrameBytes()) {
                m_ringBuffer->Reset();
                m_ringBuffer->Allocate(newFrameBytes);
            }
        }

        return true;  // ★ 跳過此 sample，下一幀才是新格式
    }

    // Null sample can happen (e.g. gap in stream) — skip
    if (!sample) {
        return true;
    }

    const LONGLONG sourceTimestamp = timestamp + m_timestampOffsetHns;

    // Output-side frame dropping: never seek during playback. Frames inside
    // the same output interval are discarded before RGB copy and presentation.
    if (m_nextFrameTimestampHns >= 0 && sourceTimestamp < m_nextFrameTimestampHns) {
        return true;
    }
    m_nextFrameTimestampHns = sourceTimestamp + m_minFrameIntervalHns;

    FrameInfo info{};
    info.width = m_videoInfo.width;
    info.height = m_videoInfo.height;
    info.timestamp = static_cast<double>(sourceTimestamp) * HNS_TO_SECONDS;

    if (m_decodePath == DecodePath::GpuDxva) {
        ComPtr<IMFMediaBuffer> mediaBuffer;
        ComPtr<IMFDXGIBuffer> dxgiBuffer;
        ComPtr<ID3D11Texture2D> texture;
        hr = sample->GetBufferByIndex(0, &mediaBuffer);
        if (SUCCEEDED(hr)) hr = mediaBuffer.As(&dxgiBuffer);
        if (SUCCEEDED(hr)) hr = dxgiBuffer->GetResource(IID_PPV_ARGS(&texture));
        if (FAILED(hr)) {
            printf("[VideoDecoder] GPU sample lost DXGI surface: 0x%08X\n",
                   static_cast<unsigned>(hr));
            return false;
        }
        info.pixelFormat = 1;
        m_ringBuffer->CommitGpuWrite(info, sample.Get());
        return true;
    }

    // Lock the media buffer and copy to ring buffer slot
    ComPtr<IMFMediaBuffer> mediaBuffer;
    hr = sample->ConvertToContiguousBuffer(&mediaBuffer);
    if (FAILED(hr)) {
        printf("[VideoDecoder] ConvertToContiguousBuffer failed: 0x%08X\n",
               static_cast<unsigned>(hr));
        return true;  // skip this frame, not EOS
    }

    BYTE*  srcData   = nullptr;
    DWORD  srcMaxLen = 0;
    DWORD  srcCurLen = 0;
    hr = mediaBuffer->Lock(&srcData, &srcMaxLen, &srcCurLen);
    if (FAILED(hr)) {
        printf("[VideoDecoder] Buffer Lock failed: 0x%08X\n",
               static_cast<unsigned>(hr));
        return true;
    }

    // Copy decoded pixels into the pre-allocated ring buffer slot
    const size_t copySize = (std::min)(static_cast<size_t>(srcCurLen),
                                       writeSlot.capacity);
    std::memcpy(writeSlot.ptr, srcData, copySize);
    m_cpuCopiedFrameCount.fetch_add(1, std::memory_order_relaxed);

    mediaBuffer->Unlock();

    info.stride      = m_videoInfo.stride;
    info.pixelFormat = 0;  // BGRA

    // Commit the frame to the ring buffer (publish to consumer)
    m_ringBuffer->CommitWrite(info);

    return true;
}

// ============================================================================
// InitializeMF / ShutdownMF
// ============================================================================

void VideoDecoder::InitializeMF() {
    if (m_mfInitialized) return;

    ThrowIfFailed(
        MFStartup(MF_VERSION, MFSTARTUP_LITE),
        "MFStartup"
    );
    m_mfInitialized = true;
}

void VideoDecoder::ShutdownMF() {
    if (!m_mfInitialized) return;

    MFShutdown();
    m_mfInitialized = false;
}

void VideoDecoder::CreateD3D11DeviceManager() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel{};
    ThrowIfFailed(D3D11CreateDevice(
        m_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, &m_d3d11Device, &featureLevel, &m_d3d11Context),
        "D3D11CreateDevice(video support)");

    ComPtr<ID3D10Multithread> multithread;
    ThrowIfFailed(m_d3d11Context.As(&multithread), "QI ID3D10Multithread");
    multithread->SetMultithreadProtected(TRUE);

    ThrowIfFailed(MFCreateDXGIDeviceManager(
        &m_deviceManagerResetToken, &m_dxgiDeviceManager),
        "MFCreateDXGIDeviceManager");
    ThrowIfFailed(m_dxgiDeviceManager->ResetDevice(
        m_d3d11Device.Get(), m_deviceManagerResetToken),
        "IMFDXGIDeviceManager::ResetDevice");

    DXGI_ADAPTER_DESC3 expected{};
    m_adapter->GetDesc3(&expected);
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> actualAdapter;
    ThrowIfFailed(m_d3d11Device.As(&dxgiDevice), "QI IDXGIDevice");
    ThrowIfFailed(dxgiDevice->GetAdapter(&actualAdapter), "IDXGIDevice::GetAdapter");
    DXGI_ADAPTER_DESC actual{};
    actualAdapter->GetDesc(&actual);
    if (expected.AdapterLuid.HighPart != actual.AdapterLuid.HighPart
        || expected.AdapterLuid.LowPart != actual.AdapterLuid.LowPart) {
        throw std::runtime_error("D3D11/D3D12 adapter LUID mismatch");
    }
    wprintf(L"[VideoDecoder] D3D11 video device: %ls, LUID=%08X:%08X\n",
            expected.Description, static_cast<unsigned>(expected.AdapterLuid.HighPart),
            expected.AdapterLuid.LowPart);
}

void VideoDecoder::CreateSourceReader(const std::wstring& filePath, DecodePath path) {
    ComPtr<IMFAttributes> attrs;
    ThrowIfFailed(MFCreateAttributes(&attrs, 6), "MFCreateAttributes");
    ThrowIfFailed(attrs->SetUINT32(MF_LOW_LATENCY, TRUE), "SetUINT32(MF_LOW_LATENCY)");

    if (path == DecodePath::GpuDxva) {
        ThrowIfFailed(attrs->SetUnknown(
            MF_SOURCE_READER_D3D_MANAGER, m_dxgiDeviceManager.Get()),
            "SetUnknown(MF_SOURCE_READER_D3D_MANAGER)");
        ThrowIfFailed(attrs->SetUINT32(
            MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE),
            "SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS)");
    } else {
        ThrowIfFailed(attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE),
                      "SetUINT32(MF_SOURCE_READER_DISABLE_DXVA)");
        ThrowIfFailed(attrs->SetUINT32(
            MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE),
            "SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING)");
    }

    ThrowIfFailed(MFCreateSourceReaderFromURL(
        filePath.c_str(), attrs.Get(), &m_sourceReader),
        "MFCreateSourceReaderFromURL");
}

void VideoDecoder::ConfigureOutputType(DecodePath path) {
    ComPtr<IMFMediaType> outputType;
    ThrowIfFailed(MFCreateMediaType(&outputType), "MFCreateMediaType");
    ThrowIfFailed(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video),
                  "SetGUID(MF_MT_MAJOR_TYPE)");
    const GUID subtype = path == DecodePath::GpuDxva ? MFVideoFormat_NV12 : MFVideoFormat_RGB32;
    ThrowIfFailed(outputType->SetGUID(MF_MT_SUBTYPE, subtype), "SetGUID(MF_MT_SUBTYPE)");

    ThrowIfFailed(m_sourceReader->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.Get()),
        path == DecodePath::GpuDxva ? "SetCurrentMediaType(NV12)" : "SetCurrentMediaType(RGB32)");
    printf("[VideoDecoder] Output configured: %s\n",
           path == DecodePath::GpuDxva ? "NV12 GPU surface" : "RGB32 CPU buffer");
}

void VideoDecoder::ProbeGpuSample() {
    for (int attempt = 0; attempt < 32; ++attempt) {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        ThrowIfFailed(m_sourceReader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
            &streamIndex, &flags, &timestamp, &sample),
            "ReadSample(GPU probe)");
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            throw std::runtime_error("GPU probe reached end of stream");
        }
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFDXGIBuffer> dxgiBuffer;
        ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(sample->GetBufferByIndex(0, &buffer), "GPU probe GetBufferByIndex");
        ThrowIfFailed(buffer.As(&dxgiBuffer), "GPU probe QI IMFDXGIBuffer");
        ThrowIfFailed(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture)),
                      "GPU probe GetResource(ID3D11Texture2D)");
        UINT subresource = 0;
        ThrowIfFailed(dxgiBuffer->GetSubresourceIndex(&subresource),
                      "GPU probe GetSubresourceIndex");
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_NV12) {
            throw std::runtime_error("GPU probe returned non-NV12 texture");
        }

        m_pendingSample = std::move(sample);
        m_pendingFlags = flags;
        m_pendingTimestamp = timestamp;
        printf("[VideoDecoder] GPU probe: %ux%u NV12, subresource=%u\n",
               desc.Width, desc.Height, subresource);
        return;
    }
    throw std::runtime_error("GPU probe produced no sample after 32 reads");
}

void VideoDecoder::OpenCpuPath(const char* reason) {
    m_pendingSample.Reset();
    m_sourceReader.Reset();
    m_dxgiDeviceManager.Reset();
    m_d3d11Context.Reset();
    m_d3d11Device.Reset();
    m_decodePath = DecodePath::CpuRgb32;

    CreateSourceReader(m_filePath, DecodePath::CpuRgb32);
    ConfigureOutputType(DecodePath::CpuRgb32);
    QueryVideoInfo();
    const size_t frameBytes = static_cast<size_t>(m_videoInfo.stride) * m_videoInfo.height;
    m_ringBuffer->Allocate(frameBytes);
    printf("[VideoDecoder] Decode path: CPU-RGB32 (reason=%s)\n",
           reason ? reason : "unspecified");
}

// ============================================================================
// QueryVideoInfo — extract width, height, stride, FPS, duration
// ============================================================================

void VideoDecoder::QueryVideoInfo() {
    ComPtr<IMFMediaType> currentType;
    ThrowIfFailed(
        m_sourceReader->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            &currentType),
        "GetCurrentMediaType"
    );

    // Width and height
    UINT32 width = 0, height = 0;
    ThrowIfFailed(
        MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &width, &height),
        "MFGetAttributeSize"
    );

    // ★ 修正（BUG 1）：使用 MFGetStrideForBitmapInfoHeader 取代 type punning
    //   舊版使用 reinterpret_cast<UINT32*>(&stride) 讀取 LONG，
    //   是未定義行為，在 MSVC x64 可導致崩潰。
    LONG stride = 0;
    HRESULT hr = S_OK;
    if (m_decodePath == DecodePath::CpuRgb32) {
        hr = MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &stride);
        if (FAILED(hr) || stride == 0) stride = static_cast<LONG>(width) * 4;
        if (stride < 0) stride = -stride;
    }

    // Frame rate
    UINT32 fpsNum = 0, fpsDen = 1;
    MFGetAttributeRatio(currentType.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
    double fps = (fpsDen > 0) ? static_cast<double>(fpsNum) / fpsDen : 30.0;

    // Duration (from presentation descriptor)
    PROPVARIANT varDuration;
    PropVariantInit(&varDuration);
    double durationSec = 0.0;

    hr = m_sourceReader->GetPresentationAttribute(
        static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
        MF_PD_DURATION,
        &varDuration);
    if (SUCCEEDED(hr) && varDuration.vt == VT_UI8) {
        durationSec = static_cast<double>(varDuration.uhVal.QuadPart) * HNS_TO_SECONDS;
    }
    PropVariantClear(&varDuration);

    // Populate video info
    m_videoInfo.width       = width;
    m_videoInfo.height      = height;
    m_videoInfo.stride      = static_cast<uint32_t>(stride);
    m_videoInfo.fps         = fps;
    m_videoInfo.durationSec = durationSec;
    m_videoInfo.frameCount  = (durationSec > 0.0 && fps > 0.0)
                                ? static_cast<uint64_t>(durationSec * fps)
                                : 0;
}
