#include "MonitorCoordinator.h"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>

namespace {
bool SamePath(const std::wstring& left, const std::wstring& right) {
    return !left.empty() && CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
        right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool SameDescriptor(const MonitorDescriptor& left, const MonitorDescriptor& right) {
    return SamePath(left.devicePath, right.devicePath)
        && left.bounds.left == right.bounds.left && left.bounds.top == right.bounds.top
        && left.bounds.right == right.bounds.right && left.bounds.bottom == right.bounds.bottom
        && left.primary == right.primary && left.adapterId.LowPart == right.adapterId.LowPart
        && left.adapterId.HighPart == right.adapterId.HighPart && left.targetId == right.targetId;
}

} // namespace

void MonitorCoordinator::ShutdownEntries(std::vector<Entry>& entries) noexcept {
    for (auto item = entries.rbegin(); item != entries.rend(); ++item) {
        if (!item->pipeline) continue;
        try { item->pipeline->Stop(); } catch (...) {}
        try { item->pipeline->Shutdown(); } catch (...) {}
    }
}

MonitorCoordinator::~MonitorCoordinator() { try { Shutdown(); } catch (...) {} }

void MonitorCoordinator::CheckOwnerThread() const {
    if (m_ownerThreadId && GetCurrentThreadId() != m_ownerThreadId)
        throw std::runtime_error("MonitorCoordinator called from non-owner thread");
}

void MonitorCoordinator::Initialize(const Config& config) {
    if (m_initialized || config.videoPath.empty())
        throw std::runtime_error("MonitorCoordinator::Initialize — invalid state or video path");
    m_ownerThreadId = GetCurrentThreadId();
    try {
        m_config = config;
        m_desktopId = config.desktopId;
        auto monitors = EnumerateActiveMonitors();
        if (!config.autoDetectSize) {
            if (monitors.empty()) throw std::runtime_error("MonitorCoordinator::Initialize — no primary monitor");
            monitors.resize(1);
            monitors[0].bounds.right = monitors[0].bounds.left + static_cast<LONG>(config.manualWidth);
            monitors[0].bounds.bottom = monitors[0].bounds.top + static_cast<LONG>(config.manualHeight);
        }
        if (!Reconcile(monitors)) throw std::runtime_error("MonitorCoordinator::Initialize — pipeline staging failed");
        m_initialized = true;
    } catch (...) {
        try { Cleanup(true); } catch (...) {}
        throw;
    }
}

void MonitorCoordinator::Start() {
    CheckOwnerThread();
    if (!m_initialized) throw std::runtime_error("MonitorCoordinator::Start — not initialized");
    if (m_running) return;
    try {
        for (auto& entry : m_entries) entry.pipeline->Start();
    } catch (...) {
        try { Cleanup(true); } catch (...) {}
        throw;
    }
    m_running = true;
    m_paused = false;
}

bool MonitorCoordinator::Update() {
    CheckOwnerThread();
    if (!m_initialized) return false;
    for (auto& entry : m_entries) {
        if (!entry.pipeline->PumpMessages()) {
            m_quitRequested = true;
            return false;
        }
    }
    if (m_displayChangePending) {
        m_displayChangePending = false;
        if (m_config.autoDetectSize && !Reconcile(EnumerateActiveMonitors()))
            std::printf("[MonitorCoordinator] Display change staging failed\n");
    }
    for (auto& entry : m_entries) if (entry.pipeline) entry.pipeline->Update();
    return !m_quitRequested;
}

void MonitorCoordinator::Cleanup(bool shutdown) {
    CheckOwnerThread();
    std::exception_ptr error;
    for (auto item = m_entries.rbegin(); item != m_entries.rend(); ++item) {
        if (!item->pipeline) continue;
        try { item->pipeline->Stop(); } catch (...) { if (!error) error = std::current_exception(); }
        if (shutdown) try { item->pipeline->Shutdown(); } catch (...) { if (!error) error = std::current_exception(); }
    }
    m_running = false;
    m_paused = false;
    if (shutdown) {
        m_entries.clear();
        m_initialized = false;
        m_displayChangePending = false;
        m_quitRequested = false;
        m_ownerThreadId = 0;
    }
    if (error) std::rethrow_exception(error);
}

void MonitorCoordinator::Stop() { Cleanup(false); }
void MonitorCoordinator::Shutdown() { if (m_ownerThreadId) Cleanup(true); }

void MonitorCoordinator::Pause() {
    CheckOwnerThread();
    if (m_running && !m_paused) {
        for (auto& entry : m_entries) if (entry.pipeline) entry.pipeline->Pause();
        m_paused = true;
    }
}

void MonitorCoordinator::Resume() {
    CheckOwnerThread();
    if (m_running && m_paused) {
        for (auto& entry : m_entries) if (entry.pipeline) entry.pipeline->Resume();
        m_paused = false;
    }
}

void MonitorCoordinator::SetCurrentDesktopId(const GUID& desktopId) {
    CheckOwnerThread();
    if (InlineIsEqualGUID(m_desktopId, desktopId)) return;
    m_desktopId = desktopId;
    for (auto& entry : m_entries) {
        if (!entry.pipeline) continue;
        const ResolvedWallpaper wallpaper = ResolveWallpaperForMonitor(
            m_config, entry.descriptor, m_desktopId);
        if (!entry.pipeline->SwitchMedia(wallpaper.videoPath.wstring(), wallpaper.fps)) {
            std::printf("[MonitorCoordinator] Hard cut failed for %ls\n",
                        entry.descriptor.devicePath.c_str());
        }
    }
}

ResolvedWallpaper MonitorCoordinator::ResolveWallpaperForMonitor(
    const Config& config, const MonitorDescriptor& monitor, const GUID& desktopId) {
    if (config.wallpaperConfig) {
        if (const auto resolved = config.wallpaperConfig->Resolve(monitor.devicePath, desktopId)) {
            return *resolved;
        }
    }
    return {std::filesystem::path(config.videoPath), config.targetFPS};
}

bool MonitorCoordinator::Reconcile(const std::vector<MonitorDescriptor>& monitors) {
    CheckOwnerThread();
    if (monitors.empty()) return false;
    size_t primaries = 0;
    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& descriptor = monitors[i];
        if (descriptor.devicePath.empty() || descriptor.bounds.right <= descriptor.bounds.left
            || descriptor.bounds.bottom <= descriptor.bounds.top) return false;
        primaries += descriptor.primary;
        for (size_t j = 0; j < i; ++j) if (SamePath(descriptor.devicePath, monitors[j].devicePath)) return false;
    }
    if (primaries != 1) return false;

    struct Step { const MonitorDescriptor* descriptor; size_t existing; bool unchanged; };
    std::vector<Step> plan;
    plan.reserve(monitors.size());
    std::vector<bool> matched(m_entries.size());
    for (const auto& descriptor : monitors) {
        size_t existing = 0;
        while (existing < m_entries.size()
            && (matched[existing] || !SamePath(m_entries[existing].descriptor.devicePath, descriptor.devicePath))) ++existing;
        if (existing < m_entries.size()) {
            matched[existing] = true;
        }
        plan.push_back({&descriptor, existing, existing < m_entries.size()
            && m_entries[existing].pipeline && SameDescriptor(m_entries[existing].descriptor, descriptor)});
    }

    std::vector<Entry> staged;
    try {
        staged.reserve(plan.size());
        for (const auto& step : plan) {
            if (step.unchanged) continue;
            auto pipeline = std::make_unique<MonitorPipeline>();
            const ResolvedWallpaper wallpaper = ResolveWallpaperForMonitor(
                m_config, *step.descriptor, m_desktopId);
            if (!pipeline->Initialize({*step.descriptor, wallpaper.videoPath.wstring(), wallpaper.fps,
                    m_config.enableDebugLayer, m_config.budgetQuery, [this] { ScheduleDisplayChange(); }})) {
                ShutdownEntries(staged);
                return false;
            }
            if (m_running) {
                pipeline->Start();
                if (m_paused) pipeline->Pause();
            }
            staged.push_back({*step.descriptor, std::move(pipeline)});
        }
    } catch (...) {
        ShutdownEntries(staged);
        return false;
    }

    std::vector<Entry> next;
    try {
        next.reserve(plan.size());
        for (const auto& step : plan) next.push_back({*step.descriptor, nullptr});
    } catch (...) {
        ShutdownEntries(staged);
        throw;
    }
    size_t stagedIndex = 0;
    for (size_t i = 0; i < plan.size(); ++i) {
        next[i].pipeline = plan[i].unchanged ? std::move(m_entries[plan[i].existing].pipeline)
                                             : std::move(staged[stagedIndex++].pipeline);
    }
    m_entries.swap(next);
    ShutdownEntries(next);
    return true;
}

const PresentLoop::Stats MonitorCoordinator::GetPrimaryRenderStats() const noexcept {
    for (const auto& entry : m_entries)
        if (entry.descriptor.primary && entry.pipeline) return entry.pipeline->GetRenderStats();
    return {};
}

HWND MonitorCoordinator::GetPipelineWindowForCheck(const std::wstring& devicePath) const noexcept {
    for (const auto& entry : m_entries) {
        if (entry.pipeline && SamePath(entry.descriptor.devicePath, devicePath))
            return entry.pipeline->GetWindowHandle();
    }
    return nullptr;
}
