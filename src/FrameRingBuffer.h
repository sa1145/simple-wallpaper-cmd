#pragma once
// SPSC queue for either CPU BGRA bytes or MF-owned GPU surfaces.

#include "Common.h"

#include <mfidl.h>

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>

struct FrameInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t pixelFormat = 0; // 0=BGRA, 1=NV12
    double timestamp = 0.0;
    bool isEndOfStream = false;
};

enum class GpuFrameState : uint8_t { Free, Ready, InFlight };

[[nodiscard]] constexpr bool IsValidGpuFrameTransition(
    GpuFrameState from, GpuFrameState to) noexcept {
    return (from == GpuFrameState::Free && to == GpuFrameState::Ready)
        || (from == GpuFrameState::Ready && to == GpuFrameState::InFlight)
        || (from == GpuFrameState::InFlight && to == GpuFrameState::Free);
}

class FrameRingBuffer {
public:
    static constexpr int CAPACITY = 2;

    FrameRingBuffer() = default;
    FrameRingBuffer(const FrameRingBuffer&) = delete;
    FrameRingBuffer& operator=(const FrameRingBuffer&) = delete;

    void Allocate(size_t maxFrameBytes) {
        assert(maxFrameBytes > 0);
        Reset();
        m_maxFrameBytes = maxFrameBytes;
        for (auto& slot : m_slots) slot.buffer.resize(maxFrameBytes);
        m_gpuMode = false;
        m_allocated = true;
    }

    void PrepareGpu() {
        Reset();
        m_maxFrameBytes = 0;
        for (auto& slot : m_slots) {
            slot.buffer.clear();
            slot.buffer.shrink_to_fit();
        }
        m_gpuMode = true;
        m_allocated = true;
    }

    // Caller must pause producer and consumer and finish submitted GPU work.
    void Reset() {
        m_writeIdx.store(0, std::memory_order_relaxed);
        m_readIdx.store(0, std::memory_order_relaxed);
        for (auto& slot : m_slots) {
            slot.sample.Reset();
            slot.info = {};
            slot.completionFenceValue = 0;
            slot.state.store(GpuFrameState::Free, std::memory_order_release);
        }
    }

    struct WriteSlot {
        uint8_t* ptr = nullptr;
        size_t capacity = 0;
    };

    [[nodiscard]] WriteSlot TryAcquireWrite() {
        assert(m_allocated && !m_gpuMode);
        Slot& slot = m_slots[m_writeIdx.load(std::memory_order_relaxed)];
        if (slot.state.load(std::memory_order_acquire) != GpuFrameState::Free) return {};
        return {slot.buffer.data(), m_maxFrameBytes};
    }

    [[nodiscard]] bool CanWriteGpu() const {
        assert(m_allocated && m_gpuMode);
        return m_slots[m_writeIdx.load(std::memory_order_relaxed)]
            .state.load(std::memory_order_acquire) == GpuFrameState::Free;
    }

    void CommitWrite(const FrameInfo& info) {
        assert(!m_gpuMode);
        Publish(info, nullptr);
    }

    void CommitGpuWrite(const FrameInfo& info, IMFSample* sample) {
        assert(m_gpuMode && sample);
        Publish(info, sample);
    }

    struct ReadSlot {
        const uint8_t* ptr = nullptr;
        IMFSample* sample = nullptr;
        FrameInfo info{};
    };

    [[nodiscard]] ReadSlot TryPeekRead() const {
        assert(m_allocated);
        const Slot& slot = m_slots[m_readIdx.load(std::memory_order_relaxed)];
        if (slot.state.load(std::memory_order_acquire) != GpuFrameState::Ready) return {};
        return {m_gpuMode ? nullptr : slot.buffer.data(), slot.sample.Get(), slot.info};
    }

    void CommitRead() {
        assert(!m_gpuMode);
        Slot& slot = CurrentReadSlot();
        assert(slot.state.load(std::memory_order_relaxed) == GpuFrameState::Ready);
        slot.state.store(GpuFrameState::Free, std::memory_order_release);
        AdvanceRead();
    }

    void CommitGpuRead(uint64_t completionFenceValue) {
        assert(m_gpuMode && completionFenceValue > 0);
        Slot& slot = CurrentReadSlot();
        assert(IsValidGpuFrameTransition(
            slot.state.load(std::memory_order_relaxed), GpuFrameState::InFlight));
        slot.completionFenceValue = completionFenceValue;
        slot.state.store(GpuFrameState::InFlight, std::memory_order_release);
        AdvanceRead();
    }

    void RecycleCompletedGpuFrames(uint64_t completedFenceValue) {
        if (!m_gpuMode) return;
        for (auto& slot : m_slots) {
            if (slot.state.load(std::memory_order_acquire) == GpuFrameState::InFlight
                && slot.completionFenceValue <= completedFenceValue) {
                slot.sample.Reset();
                slot.completionFenceValue = 0;
                slot.state.store(GpuFrameState::Free, std::memory_order_release);
            }
        }
    }

    [[nodiscard]] int AvailableFrames() const {
        int count = 0;
        for (const auto& slot : m_slots) {
            if (slot.state.load(std::memory_order_relaxed) == GpuFrameState::Ready) ++count;
        }
        return count;
    }

    [[nodiscard]] bool IsFull() const {
        for (const auto& slot : m_slots) {
            if (slot.state.load(std::memory_order_relaxed) == GpuFrameState::Free) return false;
        }
        return true;
    }

    [[nodiscard]] bool IsEmpty() const { return AvailableFrames() == 0; }
    [[nodiscard]] size_t GetMaxFrameBytes() const noexcept { return m_maxFrameBytes; }
    [[nodiscard]] bool IsAllocated() const noexcept { return m_allocated; }
    [[nodiscard]] bool IsGpuMode() const noexcept { return m_gpuMode; }

private:
    struct Slot {
        std::vector<uint8_t> buffer;
        ComPtr<IMFSample> sample;
        FrameInfo info{};
        uint64_t completionFenceValue = 0;
        std::atomic<GpuFrameState> state{GpuFrameState::Free};
    };

    void Publish(const FrameInfo& info, IMFSample* sample) {
        const int idx = m_writeIdx.load(std::memory_order_relaxed);
        Slot& slot = m_slots[idx];
        assert(IsValidGpuFrameTransition(
            slot.state.load(std::memory_order_relaxed), GpuFrameState::Ready));
        slot.info = info;
        slot.sample = sample;
        slot.state.store(GpuFrameState::Ready, std::memory_order_release);
        m_writeIdx.store((idx + 1) % CAPACITY, std::memory_order_relaxed);
    }

    Slot& CurrentReadSlot() {
        return m_slots[m_readIdx.load(std::memory_order_relaxed)];
    }

    void AdvanceRead() {
        const int idx = m_readIdx.load(std::memory_order_relaxed);
        m_readIdx.store((idx + 1) % CAPACITY, std::memory_order_relaxed);
    }

    std::array<Slot, CAPACITY> m_slots;
    std::atomic<int> m_writeIdx{0};
    std::atomic<int> m_readIdx{0};
    size_t m_maxFrameBytes = 0;
    bool m_gpuMode = false;
    bool m_allocated = false;
};
