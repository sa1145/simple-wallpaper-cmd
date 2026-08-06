#pragma once
// ============================================================================
// Common.h — Shared utilities for DX12 Wallpaper Engine
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <wrl/client.h>      // Microsoft::WRL::ComPtr

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// ComPtr alias for convenience
// ---------------------------------------------------------------------------
template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// ---------------------------------------------------------------------------
// ThrowIfFailed — wraps every HRESULT check (per spec: mandatory)
// ---------------------------------------------------------------------------
inline void ThrowIfFailed(HRESULT hr, const char* context = "") {
    if (FAILED(hr)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "[DX12 Error] 0x%08X at %s", hr, context);
        throw std::runtime_error(msg);
    }
}

// ---------------------------------------------------------------------------
// SafeRelease — fallback for non-ComPtr resources
// ---------------------------------------------------------------------------
template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}
