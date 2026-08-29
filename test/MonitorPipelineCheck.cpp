#include "MonitorEnumerator.h"
#include "MonitorPipeline.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
bool SameRect(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top
        && left.right == right.right && left.bottom == right.bottom;
}

void RunFor(MonitorPipeline& pipeline, std::chrono::milliseconds duration, bool update = true) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)pipeline.PumpMessages();
        if (update) pipeline.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}

int wmain() {
    try {
        const auto monitors = EnumerateActiveMonitors();
        assert(!monitors.empty());
        const MonitorDescriptor target = monitors.front();
        constexpr wchar_t kVideo[] = L"D:\\test_cpp_build\\test.mp4";

        MonitorPipeline primary;
        MonitorPipeline::Config primaryConfig{target, kVideo};
        assert(primary.Initialize(primaryConfig));
        assert(primary.IsPrimary());
        assert(primary.GetRenderScale() == 1.0f);
        assert(SameRect(primary.GetWindowBounds(), target.bounds));
        primary.Start();
        RunFor(primary, std::chrono::seconds(3));
        assert(primary.GetPresentedFrameCount() > 0);
        assert(primary.GetUploadedFrameCount() > 0);
        assert(primary.GetCpuCopiedFrameCount() == 0);

        const HWND primaryWindow = primary.GetWindowHandle();
        const uint64_t hardCutsBeforeNoop = primary.GetHardCutCount();
        assert(primary.SwitchMedia(kVideo, 60));
        assert(primary.GetHardCutCount() == hardCutsBeforeNoop);
        assert(primary.GetWindowHandle() == primaryWindow && IsWindow(primaryWindow));

        assert(primary.SwitchMedia(kVideo, 30));
        assert(primary.GetHardCutCount() == hardCutsBeforeNoop + 1);
        assert(primary.GetTargetFPS() == 30);
        assert(primary.GetWindowHandle() == primaryWindow && IsWindow(primaryWindow));
        RunFor(primary, std::chrono::milliseconds(500));
        assert(primary.GetPresentedFrameCount() > 0);

        const uint64_t hardCutsBeforeRollback = primary.GetHardCutCount();
        assert(!primary.SwitchMedia(L"D:\\test_cpp_build\\missing-hard-cut.mp4", 31));
        assert(primary.GetHardCutCount() == hardCutsBeforeRollback);
        assert(primary.GetTargetFPS() == 30);
        assert(primary.GetWindowHandle() == primaryWindow && IsWindow(primaryWindow));
        RunFor(primary, std::chrono::milliseconds(500));
        assert(primary.GetPresentedFrameCount() > 0);

        primary.Pause();
        const uint64_t pausedFrames = primary.GetPresentedFrameCount();
        RunFor(primary, std::chrono::seconds(7)); // Cross the 8.63-second test video's end.
        assert(primary.GetPresentedFrameCount() == pausedFrames);
        primary.Resume();
        RunFor(primary, std::chrono::milliseconds(500));
        assert(primary.GetPresentedFrameCount() > pausedFrames);

        primary.Stop();
        primary.Shutdown();
        assert(primary.GetWindowBounds().right == 0);

        MonitorDescriptor secondary = target;
        secondary.primary = false;
        unsigned probeCalls = 0;
        MonitorPipeline secondaryPipeline;
        MonitorPipeline::Config secondaryConfig{secondary, kVideo};
        secondaryConfig.budgetQuery = [&probeCalls](IDXGIAdapter3*) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            info.Budget = 100;
            const unsigned call = ++probeCalls;
            info.CurrentUsage = (call == 1 || call == 4 || call == 6 || call == 8) ? 101 : 100;
            return info;
        };
        assert(secondaryPipeline.Initialize(secondaryConfig));
        assert(probeCalls == 2);
        assert(secondaryPipeline.GetRenderScale() == 0.75f);
        const uint32_t nativeWidth = static_cast<uint32_t>(target.bounds.right - target.bounds.left);
        const uint32_t nativeHeight = static_cast<uint32_t>(target.bounds.bottom - target.bounds.top);
        if (secondaryPipeline.GetRenderWidth() != ((nativeWidth * 3 / 4) & ~1u)
            || secondaryPipeline.GetRenderHeight() != ((nativeHeight * 3 / 4) & ~1u)) return 2;
        assert(SameRect(secondaryPipeline.GetWindowBounds(), target.bounds));

        secondaryPipeline.Start();
        RunFor(secondaryPipeline, std::chrono::milliseconds(500), false);
        const uint64_t withinBudgetFrames = secondaryPipeline.GetPresentedFrameCount();
        if (withinBudgetFrames == 0) return 3;

        assert(secondaryPipeline.SignalBudgetChangeForCheck()); // call 3: within budget
        secondaryPipeline.Update();
        assert(secondaryPipeline.GetRenderScale() == 0.75f);
        RunFor(secondaryPipeline, std::chrono::milliseconds(250), false);
        if (secondaryPipeline.GetPresentedFrameCount() <= withinBudgetFrames) return 4;

        assert(secondaryPipeline.SignalBudgetChangeForCheck()); // call 4: over; build call 5 accepts 50%
        secondaryPipeline.Update();
        assert(secondaryPipeline.GetRenderScale() == 0.5f);
        RunFor(secondaryPipeline, std::chrono::milliseconds(500), false);
        assert(secondaryPipeline.GetPresentedFrameCount() > 0);

        assert(secondaryPipeline.SignalBudgetChangeForCheck()); // call 6: over; build call 7 accepts 25%
        secondaryPipeline.Update();
        assert(secondaryPipeline.GetRenderScale() == 0.25f);
        assert(secondaryPipeline.SignalBudgetChangeForCheck()); // call 8: over; no lower scale remains
        secondaryPipeline.Update();
        assert(secondaryPipeline.GetRenderScale() == 0.0f);
        assert(secondaryPipeline.GetWindowBounds().right == 0);
        secondaryPipeline.Shutdown();
        assert(secondaryPipeline.GetWindowBounds().right == 0);

        // A runtime notification is allowed one adjacent-scale attempt only.  Here
        // 100% is rejected during initialization, 75% is accepted, and the next
        // 50% attempt is still over budget.  It must exhaust rather than try 25%.
        unsigned exhaustedProbeCalls = 0;
        MonitorPipeline exhaustedPipeline;
        MonitorPipeline::Config exhaustedConfig{secondary, kVideo};
        exhaustedConfig.budgetQuery = [&exhaustedProbeCalls](IDXGIAdapter3*) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            info.Budget = 100;
            const unsigned call = ++exhaustedProbeCalls;
            info.CurrentUsage = (call == 2) ? 100 : 101;
            return info;
        };
        assert(exhaustedPipeline.Initialize(exhaustedConfig));
        assert(exhaustedProbeCalls == 2);
        assert(exhaustedPipeline.GetRenderScale() == 0.75f);
        exhaustedPipeline.Start();
        assert(exhaustedPipeline.SignalBudgetChangeForCheck()); // call 3: over; build call 4: 50% still over
        exhaustedPipeline.Update();
        assert(exhaustedProbeCalls == 4); // A 25% attempt would make this 5.
        assert(exhaustedPipeline.GetRenderScale() == 0.0f);
        assert(exhaustedPipeline.GetWindowBounds().right == 0);
        exhaustedPipeline.Stop();
        exhaustedPipeline.Shutdown();
        assert(exhaustedPipeline.GetWindowBounds().right == 0);
        assert(FindWindowW(L"DX12WallpaperEngineClass", nullptr) == nullptr);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MonitorPipelineCheck failed: %s\\n", error.what());
        return 1;
    }
}
