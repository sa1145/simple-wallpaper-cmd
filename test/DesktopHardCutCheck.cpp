#include "MonitorEnumerator.h"
#include "MonitorPipeline.h"
#include "VirtualDesktopTracker.h"

#include <chrono>
#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
constexpr wchar_t kVideo[] = L"D:\\test_cpp_build\\test.mp4";
constexpr unsigned kRequiredEvents = 20;
constexpr auto kEventTimeout = std::chrono::seconds(45);
constexpr auto kFrameProgressTimeout = std::chrono::seconds(10);
constexpr auto kOverallTimeout = std::chrono::minutes(20);

struct EngineWindowCount {
    UINT value = 0;
};

BOOL CALLBACK CountEngineChild(HWND hwnd, LPARAM parameter) {
    wchar_t className[64]{};
    if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className)))
        && wcscmp(className, L"DX12WallpaperEngineClass") == 0) {
        ++reinterpret_cast<EngineWindowCount*>(parameter)->value;
    }
    return TRUE;
}

BOOL CALLBACK CountEngineChildren(HWND hwnd, LPARAM parameter) {
    EnumChildWindows(hwnd, CountEngineChild, parameter);
    return TRUE;
}

UINT CountEngineWindows() {
    EngineWindowCount count;
    EnumWindows(CountEngineChildren, reinterpret_cast<LPARAM>(&count));
    return count.value;
}

void PrintGuid(const GUID& desktopId) {
    wchar_t text[40]{};
    StringFromGUID2(desktopId, text, static_cast<int>(std::size(text)));
    std::printf("%ls", text);
}

struct ExplorerIdentity {
    HWND shellWindow = nullptr;
    HANDLE process = nullptr;
    DWORD processId = 0;
    DWORD sessionId = 0;
    FILETIME startTime{};
};

bool GetProcessStartTime(HANDLE process, FILETIME* startTime) {
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    return GetProcessTimes(process, startTime, &exitTime, &kernelTime, &userTime) != FALSE;
}

bool CaptureExplorer(ExplorerIdentity* explorer) {
    explorer->shellWindow = GetShellWindow();
    if (!explorer->shellWindow || !IsWindow(explorer->shellWindow)) return false;
    GetWindowThreadProcessId(explorer->shellWindow, &explorer->processId);
    if (!explorer->processId || !ProcessIdToSessionId(explorer->processId, &explorer->sessionId)) return false;
    explorer->process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                    explorer->processId);
    return explorer->process && GetProcessStartTime(explorer->process, &explorer->startTime);
}

bool IsOriginalExplorerAlive(const ExplorerIdentity& explorer) {
    if (!explorer.process || WaitForSingleObject(explorer.process, 0) != WAIT_TIMEOUT) return false;
    const HWND shellWindow = GetShellWindow();
    if (shellWindow != explorer.shellWindow || !IsWindow(shellWindow)) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(shellWindow, &processId);
    DWORD sessionId = 0;
    FILETIME startTime{};
    return processId == explorer.processId
        && ProcessIdToSessionId(processId, &sessionId)
        && sessionId == explorer.sessionId
        && GetProcessStartTime(explorer.process, &startTime)
        && CompareFileTime(&startTime, &explorer.startTime) == 0;
}

}

int wmain() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    MonitorPipeline pipeline;
    VirtualDesktopTracker tracker;
    ExplorerIdentity explorer;
    HWND pipelineWindow = nullptr;
    DWORD ownerThread = GetCurrentThreadId();
    unsigned eventCount = 0;
    uint64_t hardCutCount = 0;
    std::string failure;
    int exitCode = 1;
    try {
        const auto monitors = EnumerateActiveMonitors();
        if (monitors.empty() || !monitors.front().primary)
            throw std::runtime_error("DesktopHardCutCheck requires an active primary monitor");
        if (!CaptureExplorer(&explorer))
            throw std::runtime_error("DesktopHardCutCheck could not capture the interactive Explorer identity");

        MonitorPipeline::Config config{monitors.front(), kVideo};
        config.targetFPS = 30;
        if (!pipeline.Initialize(config))
            throw std::runtime_error("DesktopHardCutCheck failed to initialize the primary pipeline");
        pipeline.Start();
        pipelineWindow = pipeline.GetWindowHandle();
        if (!pipelineWindow || !IsWindow(pipelineWindow))
            throw std::runtime_error("DesktopHardCutCheck did not create its primary HWND");

        GUID previousDesktop{};
        bool awaitingFrameProgress = true;
        uint64_t uploadedBefore = pipeline.GetUploadedFrameCount();
        uint64_t presentedBefore = pipeline.GetPresentedFrameCount();
        const auto startedAt = std::chrono::steady_clock::now();
        auto eventDeadline = startedAt + kEventTimeout;
        auto frameProgressDeadline = startedAt + kFrameProgressTimeout;
        if (!tracker.Start([&](const GUID& desktopId) {
                const DWORD dispatchThread = GetCurrentThreadId();
                std::printf("[DesktopHardCutCheck] event=%u owner=%lu dispatch=%lu guid=",
                            eventCount + 1, ownerThread, dispatchThread);
                PrintGuid(desktopId);
                std::printf("\n");
                if (dispatchThread != ownerThread || InlineIsEqualGUID(desktopId, GUID{})
                    || InlineIsEqualGUID(desktopId, previousDesktop)) {
                    failure = "desktop event failed owner-thread or GUID validation";
                    return;
                }

                const uint32_t targetFPS = (eventCount % 2 == 0) ? 31 : 30;
                const uint64_t cutsBefore = pipeline.GetHardCutCount();
                if (!pipeline.SwitchMedia(kVideo, targetFPS)
                    || pipeline.GetHardCutCount() != cutsBefore + 1) {
                    failure = "desktop event hard cut failed";
                    return;
                }
                previousDesktop = desktopId;
                ++eventCount;
                awaitingFrameProgress = true;
                uploadedBefore = pipeline.GetUploadedFrameCount();
                presentedBefore = pipeline.GetPresentedFrameCount();
                frameProgressDeadline = std::chrono::steady_clock::now() + kFrameProgressTimeout;
                std::printf("[DesktopHardCutCheck] hard-cut=%u fps=%u awaiting frame progress\n",
                            eventCount, targetFPS);
            })) {
            const auto& diagnostic = tracker.GetDiagnostic();
            std::printf("[DesktopHardCutCheck] tracker unavailable: build=%lu revision=%ls HRESULT=0x%08lX\n",
                        diagnostic.windowsBuild, diagnostic.interfaceRevision.c_str(),
                        static_cast<unsigned long>(diagnostic.hresult));
            failure = "tracker unavailable";
        } else {
            const auto& ready = tracker.GetDiagnostic();
            std::printf("[DesktopHardCutCheck] Explorer: pid=%lu session=%lu start=%08lX:%08lX hwnd=%p\n",
                        explorer.processId, explorer.sessionId, explorer.startTime.dwHighDateTime,
                        explorer.startTime.dwLowDateTime, explorer.shellWindow);
            std::printf("[DesktopHardCutCheck] waiting for startup frame progress; owner=%lu cookie=%lu hwnd=%p\n",
                        ownerThread, ready.notificationCookie, pipelineWindow);

            while (failure.empty() && (eventCount < kRequiredEvents || awaitingFrameProgress)) {
                if (!pipeline.PumpMessages()) {
                    failure = "pipeline message pump returned false";
                    break;
                }
                if (!IsWindow(pipelineWindow)) {
                    failure = "pipeline HWND disappeared";
                    break;
                }
                if (!IsOriginalExplorerAlive(explorer)) {
                    failure = "Explorer PID/session/start-time/HWND changed";
                    break;
                }

                pipeline.Update();
                const auto now = std::chrono::steady_clock::now();
                if (now - startedAt > kOverallTimeout) {
                    failure = "overall watchdog expired";
                    break;
                }
                if (awaitingFrameProgress) {
                    if (pipeline.GetUploadedFrameCount() > uploadedBefore
                        && pipeline.GetPresentedFrameCount() > presentedBefore) {
                        awaitingFrameProgress = false;
                        if (eventCount == kRequiredEvents) break;
                        eventDeadline = now + kEventTimeout;
                        std::printf("[DesktopHardCutCheck] READY_FOR_SWITCH event=%u uploaded=%llu presented=%llu\n",
                                    eventCount + 1,
                                    static_cast<unsigned long long>(pipeline.GetUploadedFrameCount()),
                                    static_cast<unsigned long long>(pipeline.GetPresentedFrameCount()));
                    } else if (now >= frameProgressDeadline) {
                        failure = "frame-progress watchdog expired";
                        break;
                    }
                } else if (now >= eventDeadline) {
                    failure = "desktop-event watchdog expired";
                    break;
                } else {
                    (void)tracker.PumpPending(); // One owner-thread dispatch per loop; never drain queued cuts.
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (failure.empty() && eventCount == kRequiredEvents && !awaitingFrameProgress) exitCode = 0;
        }
    } catch (const std::exception& error) {
        failure = error.what();
    } catch (...) {
        failure = "unexpected diagnostic exception";
    }

    tracker.Stop();
    hardCutCount = pipeline.GetHardCutCount();
    try {
        pipeline.Stop();
        pipeline.Shutdown();
    } catch (const std::exception& error) {
        if (failure.empty()) failure = std::string("pipeline shutdown failed: ") + error.what();
    }
    const auto& stopped = tracker.GetDiagnostic();
    DWORD handleCount = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handleCount);
    const bool explorerAlive = explorer.process && IsOriginalExplorerAlive(explorer);
    const bool clean = stopped.notificationCookie == 0 && SUCCEEDED(stopped.unregisterHresult)
        && (!pipelineWindow || !IsWindow(pipelineWindow)) && CountEngineWindows() == 0;
    if (exitCode == 0 && (!failure.empty() || !clean || !explorerAlive)) {
        if (failure.empty()) failure = !explorerAlive ? "Explorer changed during diagnostic"
                                                       : "cleanup residue detected";
        exitCode = 1;
    }
    std::printf("[DesktopHardCutCheck] Stop: result=%s events=%u hard-cuts=%llu cookie=%lu unregister=0x%08lX hwnd=%s engineHwnds=%u Explorer=%s handles=%lu reason=%s\n",
                exitCode == 0 ? "PASS" : "FAIL", eventCount,
                static_cast<unsigned long long>(hardCutCount), stopped.notificationCookie,
                static_cast<unsigned long>(stopped.unregisterHresult),
                pipelineWindow && IsWindow(pipelineWindow) ? "present" : "none", CountEngineWindows(),
                explorerAlive ? "original" : "changed", handleCount,
                failure.empty() ? "none" : failure.c_str());
    if (explorer.process) CloseHandle(explorer.process);
    return exitCode;
}
