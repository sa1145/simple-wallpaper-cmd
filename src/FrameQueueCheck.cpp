#include "FrameRingBuffer.h"

#include <cassert>

int main() {
    assert(IsValidGpuFrameTransition(GpuFrameState::Free, GpuFrameState::Ready));
    assert(IsValidGpuFrameTransition(GpuFrameState::Ready, GpuFrameState::InFlight));
    assert(IsValidGpuFrameTransition(GpuFrameState::InFlight, GpuFrameState::Free));
    assert(!IsValidGpuFrameTransition(GpuFrameState::Free, GpuFrameState::InFlight));
    assert(!IsValidGpuFrameTransition(GpuFrameState::Ready, GpuFrameState::Free));
}
