#include "MonitorPipeline.h"

#include "DX12Context.h"
#include "FrameRingBuffer.h"
#include "PresentLoop.h"
#include "UploadPipeline.h"
#include "VideoDecoder.h"
#include "WindowManager.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>

struct MonitorPipeline::Runtime {
    WindowManager window;
    DX12Context dx12;
    FrameRingBuffer ring;
    VideoDecoder decoder;
    UploadPipeline upload;
    PresentLoop present;
    ComPtr<IDXGIAdapter3> adapter;
    HANDLE budgetEvent = nullptr;
    DWORD budgetCookie = 0;
    bool windowCreated = false;
    bool dx12Initialized = false;
    bool decoderOpened = false;
    bool uploadInitialized = false;
    bool presentInitialized = false;
};

namespace {
constexpr std::array<float, 4> kSecondaryScales{1.0f, 0.75f, 0.5f, 0.25f};

uint32_t ScaledEven(uint32_t native, float scale) {
    const uint32_t scaled = static_cast<uint32_t>(std::lround(native * scale));
    return std::min(native, std::max(2u, scaled & ~1u));
}

DXGI_QUERY_VIDEO_MEMORY_INFO DefaultBudgetQuery(IDXGIAdapter3* adapter) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    ThrowIfFailed(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info),
                  "IDXGIAdapter3::QueryVideoMemoryInfo");
    return info;
}

bool SameMedia(const std::wstring& leftPath, uint32_t leftFPS,
               const std::wstring& rightPath, uint32_t rightFPS) {
    return leftFPS == rightFPS && CompareStringOrdinal(
        leftPath.c_str(), static_cast<int>(leftPath.size()),
        rightPath.c_str(), static_cast<int>(rightPath.size()), TRUE) == CSTR_EQUAL;
}
}

MonitorPipeline::~MonitorPipeline() noexcept {
    try { Shutdown(); } catch (...) {}
}

MonitorPipeline::MonitorPipeline() = default;

bool MonitorPipeline::Initialize(const Config& config) {
    if (m_initialized) throw std::runtime_error("MonitorPipeline::Initialize — already initialized");
    if (config.videoPath.empty()) throw std::runtime_error("MonitorPipeline::Initialize — videoPath is empty");
    if (config.target.bounds.right <= config.target.bounds.left
        || config.target.bounds.bottom <= config.target.bounds.top) {
        throw std::runtime_error("MonitorPipeline::Initialize — target bounds are invalid");
    }

    m_config = config;
    m_config.targetFPS = std::clamp(m_config.targetFPS, 1u, 60u);
    m_hardCutCount = 0;
    const auto tryScale = [this](float scale) {
        try { return Build(scale); }
        catch (const std::exception& error) {
            std::printf("[MonitorPipeline] Build failed at scale %.2f for %ls: %s\n",
                        scale, m_config.target.devicePath.c_str(), error.what());
            TearDownRuntime();
            return false;
        } catch (...) {
            std::printf("[MonitorPipeline] Build failed at scale %.2f for %ls: unknown exception\n",
                        scale, m_config.target.devicePath.c_str());
            TearDownRuntime();
            return false;
        }
    };

    if (m_config.target.primary) {
        if (!tryScale(1.0f)) throw std::runtime_error("MonitorPipeline::Initialize — primary creation failed");
    } else {
        for (float scale : kSecondaryScales) {
            if (tryScale(scale)) break;
        }
        if (!m_runtime) return false;
    }
    m_initialized = true;
    return true;
}

bool MonitorPipeline::Build(float scale) {
    const uint32_t nativeWidth = static_cast<uint32_t>(m_config.target.bounds.right - m_config.target.bounds.left);
    const uint32_t nativeHeight = static_cast<uint32_t>(m_config.target.bounds.bottom - m_config.target.bounds.top);
    auto runtime = std::make_unique<Runtime>();

    WindowManager::Config windowConfig;
    windowConfig.autoDetectSize = false;
    windowConfig.width = nativeWidth;
    windowConfig.height = nativeHeight;
    windowConfig.posX = m_config.target.bounds.left;
    windowConfig.posY = m_config.target.bounds.top;
    windowConfig.monitorDevicePath = m_config.target.devicePath;
    windowConfig.windowTitle = L"DX12 Wallpaper Engine";
    runtime->window.SetDisplayChangeCallback(m_config.onDisplayChange);
    runtime->window.Create(windowConfig);
    runtime->windowCreated = true;

    DX12Context::Config dxConfig;
    dxConfig.hwnd = runtime->window.GetHwnd();
    dxConfig.width = ScaledEven(nativeWidth, scale);
    dxConfig.height = ScaledEven(nativeHeight, scale);
    dxConfig.bufferCount = 2;
    dxConfig.backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    dxConfig.enableDebugLayer = m_config.enableDebugLayer;
    runtime->dx12.Initialize(dxConfig);
    runtime->dx12Initialized = true;

    ThrowIfFailed(runtime->dx12.GetAdapter()->QueryInterface(IID_PPV_ARGS(&runtime->adapter)),
                  "IDXGIAdapter4::QueryInterface(IDXGIAdapter3)");

    OpenMedia(*runtime, m_config.videoPath, m_config.targetFPS);

    m_runtime = std::move(runtime);
    const auto info = m_config.budgetQuery ? m_config.budgetQuery(m_runtime->adapter.Get())
                                           : DefaultBudgetQuery(m_runtime->adapter.Get());
    if (!m_config.target.primary && info.CurrentUsage > info.Budget) {
        TearDownRuntime();
        return false;
    }
    m_renderScale = scale;
    RegisterBudgetEvent();
    return true;
}

void MonitorPipeline::Start() {
    if (!m_initialized || !m_runtime) throw std::runtime_error("MonitorPipeline::Start — not initialized");
    if (m_running) return;
    m_loopSeekRequested.store(false, std::memory_order_release);
    try {
        m_runtime->decoder.Start();
        m_runtime->present.Start();
        m_running = true;
    } catch (...) {
        m_runtime->present.Stop();
        m_runtime->decoder.Stop();
        m_running = false;
        throw;
    }
}

void MonitorPipeline::Update() {
    if (!m_initialized || !m_runtime) return;
    if (m_runtime->budgetEvent && WaitForSingleObject(m_runtime->budgetEvent, 0) == WAIT_OBJECT_0) {
        const auto info = m_config.budgetQuery ? m_config.budgetQuery(m_runtime->adapter.Get())
                                               : DefaultBudgetQuery(m_runtime->adapter.Get());
        if (m_config.target.primary) {
            std::printf("[MonitorPipeline] Primary budget changed; retaining native scale\n");
        } else if (info.CurrentUsage > info.Budget) {
            RebuildBelowCurrentScale();
        }
    }
    if (m_running && !m_paused
        && ((m_loopSeekRequested.load(std::memory_order_acquire) && m_runtime->ring.IsEmpty())
                      || m_runtime->decoder.IsPlaybackTimeElapsed())) {
        PerformLoopSeek();
        m_loopSeekRequested.store(false, std::memory_order_release);
    }
}

void MonitorPipeline::Pause() {
    if (!m_runtime || !m_running || m_paused) return;
    m_runtime->decoder.Pause();
    m_runtime->present.Pause();
    m_paused = true;
}

void MonitorPipeline::Resume() {
    if (!m_runtime || !m_running || !m_paused) return;
    m_runtime->decoder.Resume();
    m_runtime->present.Resume();
    m_paused = false;
}

bool MonitorPipeline::SwitchMedia(const std::wstring& videoPath, uint32_t targetFPS) {
    if (!m_initialized || !m_runtime) {
        std::printf("[MonitorPipeline] Hard cut rejected: pipeline is not initialized\n");
        return false;
    }
    if (videoPath.empty() || targetFPS == 0 || targetFPS > 60) {
        std::printf("[MonitorPipeline] Hard cut rejected: invalid target media\n");
        return false;
    }
    if (SameMedia(m_config.videoPath, m_config.targetFPS, videoPath, targetFPS)) return true;

    const std::wstring oldPath = m_config.videoPath;
    const uint32_t oldFPS = m_config.targetFPS;
    const bool wasRunning = m_running;
    const bool wasPaused = m_paused;
    try {
        // Hard-cut invariant: quiesce both threads before flushing/resetting their shared queue.
        Pause();
        m_runtime->dx12.FlushGPU();
        m_runtime->ring.Reset();
        m_loopSeekRequested.store(false, std::memory_order_release);
        CloseMedia(*m_runtime);

        OpenMedia(*m_runtime, videoPath, targetFPS);
        RestoreMediaState(wasRunning, wasPaused);
        m_config.videoPath = videoPath;
        m_config.targetFPS = targetFPS;
        ++m_hardCutCount;
        std::printf("[MonitorPipeline] Hard cut %llu: %ls @ %u FPS\n",
                    static_cast<unsigned long long>(m_hardCutCount), videoPath.c_str(), targetFPS);
        return true;
    } catch (const std::exception& error) {
        std::printf("[MonitorPipeline] Hard cut failed for %ls @ %u FPS: %s; rolling back\n",
                    videoPath.c_str(), targetFPS, error.what());
    } catch (...) {
        std::printf("[MonitorPipeline] Hard cut failed for %ls @ %u FPS; rolling back\n",
                    videoPath.c_str(), targetFPS);
    }

    try {
        CloseMedia(*m_runtime);
        OpenMedia(*m_runtime, oldPath, oldFPS);
        m_config.videoPath = oldPath;
        m_config.targetFPS = oldFPS;
        RestoreMediaState(wasRunning, wasPaused);
        std::printf("[MonitorPipeline] Hard cut rollback restored %ls @ %u FPS\n",
                    oldPath.c_str(), oldFPS);
    } catch (const std::exception& error) {
        CloseMedia(*m_runtime);
        m_running = false;
        m_paused = false;
        std::printf("[MonitorPipeline] Hard cut rollback failed: %s; pipeline stopped\n", error.what());
    } catch (...) {
        CloseMedia(*m_runtime);
        m_running = false;
        m_paused = false;
        std::printf("[MonitorPipeline] Hard cut rollback failed; pipeline stopped\n");
    }
    return false;
}

void MonitorPipeline::Stop() {
    if (!m_runtime || !m_running) return;
    m_runtime->present.Stop();
    m_runtime->decoder.Stop();
    m_running = false;
    m_paused = false;
}

void MonitorPipeline::Shutdown() {
    Stop();
    TearDownRuntime();
    m_initialized = false;
    m_renderScale = 0.0f;
    m_loopSeekRequested.store(false, std::memory_order_release);
    m_paused = false;
    m_hardCutCount = 0;
}

bool MonitorPipeline::PumpMessages() {
    if (!m_runtime) return true;
    if (!m_runtime->window.PumpMessages()) return false;
    if (!m_runtime->window.IsValid() && m_config.onDisplayChange) m_config.onDisplayChange();
    return true;
}

bool MonitorPipeline::SignalBudgetChangeForCheck() {
    return m_runtime && m_runtime->budgetEvent && SetEvent(m_runtime->budgetEvent) != FALSE;
}

bool MonitorPipeline::RebuildBelowCurrentScale() {
    if (m_config.target.primary) return true;
    const bool wasRunning = m_running;
    auto current = std::find(kSecondaryScales.begin(), kSecondaryScales.end(), m_renderScale);
    Stop();
    TearDownRuntime();
    m_initialized = false;
    m_running = false;
    if (current == kSecondaryScales.end()) return false;
    ++current;
    if (current == kSecondaryScales.end()) {
        m_renderScale = 0.0f;
        m_loopSeekRequested.store(false, std::memory_order_release);
        return false;
    }
    try {
        if (Build(*current)) {
            m_initialized = true;
            if (wasRunning) Start();
            return true;
        }
    } catch (...) {
        TearDownRuntime();
    }
    m_renderScale = 0.0f;
    m_loopSeekRequested.store(false, std::memory_order_release);
    return false;
}

void MonitorPipeline::OpenMedia(Runtime& runtime, const std::wstring& videoPath, uint32_t targetFPS) {
    VideoDecoder::Config decoderConfig;
    decoderConfig.filePath = videoPath;
    decoderConfig.ringBuffer = &runtime.ring;
    decoderConfig.adapter = runtime.dx12.GetAdapter();
    decoderConfig.allowSoftwareFallback = true;
    decoderConfig.targetFPS = targetFPS;
    runtime.decoder.Open(decoderConfig);
    runtime.decoderOpened = true;
    runtime.decoder.SetEndOfStreamCallback([this] { OnEndOfStream(); });

    const auto& video = runtime.decoder.GetVideoInfo();
    UploadPipeline::Config uploadConfig;
    uploadConfig.dx12 = &runtime.dx12;
    uploadConfig.ringBuffer = &runtime.ring;
    uploadConfig.decoder = &runtime.decoder;
    uploadConfig.frameWidth = video.width;
    uploadConfig.frameHeight = video.height;
    runtime.upload.Initialize(uploadConfig);
    runtime.uploadInitialized = true;

    PresentLoop::Config presentConfig;
    presentConfig.dx12 = &runtime.dx12;
    presentConfig.uploadPipeline = &runtime.upload;
    presentConfig.targetFPS = targetFPS;
    runtime.present.Initialize(presentConfig);
    runtime.presentInitialized = true;
}

void MonitorPipeline::CloseMedia(Runtime& runtime) {
    if (runtime.presentInitialized) runtime.present.Shutdown();
    if (runtime.uploadInitialized) runtime.upload.Shutdown();
    if (runtime.decoderOpened) runtime.decoder.Close();
    runtime.presentInitialized = false;
    runtime.uploadInitialized = false;
    runtime.decoderOpened = false;
}

void MonitorPipeline::RestoreMediaState(bool wasRunning, bool wasPaused) {
    m_running = false;
    m_paused = false;
    if (!wasRunning) return;
    m_runtime->decoder.Start();
    try {
        m_runtime->present.Start();
    } catch (...) {
        m_runtime->decoder.Stop();
        throw;
    }
    m_running = true;
    if (wasPaused) Pause();
}

void MonitorPipeline::TearDownRuntime() {
    if (!m_runtime) return;
    UnregisterBudgetEvent();
    auto& runtime = *m_runtime;
    CloseMedia(runtime);
    if (runtime.dx12Initialized) runtime.dx12.Shutdown();
    if (runtime.windowCreated) runtime.window.Destroy();
    m_runtime.reset();
}

void MonitorPipeline::PerformLoopSeek() {
    assert(m_runtime && m_running);
    m_runtime->decoder.Pause();
    m_runtime->present.Pause();
    m_runtime->dx12.FlushGPU();
    m_runtime->ring.Reset();
    m_runtime->decoder.Seek(0.0);
    m_runtime->decoder.Resume();
    m_runtime->present.Resume();
}

void MonitorPipeline::OnEndOfStream() {
    m_loopSeekRequested.store(true, std::memory_order_release);
}

void MonitorPipeline::RegisterBudgetEvent() {
    if (!m_runtime || !m_runtime->adapter) return;
    m_runtime->budgetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_runtime->budgetEvent) throw std::runtime_error("CreateEventW(video budget) failed");
    ThrowIfFailed(m_runtime->adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(
                      m_runtime->budgetEvent, &m_runtime->budgetCookie),
                  "IDXGIAdapter3::RegisterVideoMemoryBudgetChangeNotificationEvent");
}

void MonitorPipeline::UnregisterBudgetEvent() {
    if (!m_runtime) return;
    if (m_runtime->budgetCookie) {
        m_runtime->adapter->UnregisterVideoMemoryBudgetChangeNotification(m_runtime->budgetCookie);
        m_runtime->budgetCookie = 0;
    }
    if (m_runtime->budgetEvent) {
        CloseHandle(m_runtime->budgetEvent);
        m_runtime->budgetEvent = nullptr;
    }
}

uint64_t MonitorPipeline::GetCpuCopiedFrameCount() const noexcept {
    return m_runtime ? m_runtime->decoder.GetCpuCopiedFrameCount() : 0;
}

uint64_t MonitorPipeline::GetPresentedFrameCount() const noexcept {
    return m_runtime ? m_runtime->present.GetStats().totalFrames : 0;
}

uint64_t MonitorPipeline::GetUploadedFrameCount() const noexcept {
    return m_runtime ? m_runtime->upload.GetUploadedFrameCount() : 0;
}

PresentLoop::Stats MonitorPipeline::GetRenderStats() const noexcept {
    return m_runtime ? m_runtime->present.GetStats() : PresentLoop::Stats{};
}

uint32_t MonitorPipeline::GetRenderWidth() const noexcept {
    return m_runtime ? m_runtime->dx12.GetWidth() : 0;
}

uint32_t MonitorPipeline::GetRenderHeight() const noexcept {
    return m_runtime ? m_runtime->dx12.GetHeight() : 0;
}

RECT MonitorPipeline::GetWindowBounds() const noexcept {
    RECT bounds{};
    if (m_runtime && m_runtime->window.GetHwnd()) GetWindowRect(m_runtime->window.GetHwnd(), &bounds);
    return bounds;
}

HWND MonitorPipeline::GetWindowHandle() const noexcept {
    return m_runtime ? m_runtime->window.GetHwnd() : nullptr;
}

bool MonitorPipeline::IsWindowHealthy() const noexcept {
    return m_runtime && m_runtime->window.IsAttachedToCurrentDesktopHost();
}
