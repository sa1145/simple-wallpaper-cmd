#pragma once

#include <servprov.h>
#include <windows.h>
#include <winstring.h>

// Build 26200 notification ABI. Trace: ramensoftware/windhawk-mods,
// taskbar-desktop-indicator.wh.cpp and virtual-desktop-helper.wh.cpp,
// consulted 2026-08-23. This private ABI is intentionally limited to the
// notification service, its sink, and IVirtualDesktop::GetId.
namespace VirtualDesktopPrivate {

inline constexpr DWORD kSupportedWindowsBuild = 26200;
inline constexpr const wchar_t kRevision[] = L"win11-26200-notification-v1";

inline constexpr CLSID kImmersiveShellClsid{
    0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39},
};

inline constexpr GUID kNotificationServiceSid{
    0xA501FDEC, 0x4A09, 0x464C, {0xAE, 0x4E, 0x1B, 0x9C, 0x21, 0xB8, 0x49, 0x18},
};
inline constexpr IID kNotificationServiceIid{
    0x0CD45E71, 0xD927, 0x4F15, {0x8B, 0x0A, 0x8F, 0xEF, 0x52, 0x53, 0x37, 0xBF},
};
inline constexpr IID kNotificationSinkIid{
    0xB9E5E94D, 0x233E, 0x49AB, {0xAF, 0x5C, 0x2B, 0x45, 0x41, 0xC3, 0xAA, 0xDE},
};
inline constexpr IID kVirtualDesktopIid{
    0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C},
};

inline constexpr unsigned kNotificationVtableSlots = 14;
inline constexpr unsigned kCurrentDesktopChangedSlot = 10;

struct IApplicationView;

MIDL_INTERFACE("3F07F4BE-B107-441A-AF0F-39D82529072C")
IVirtualDesktop : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE IsViewVisible(IUnknown* view, BOOL* visible) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetId(GUID* desktopId) = 0;
};

enum class VirtualDesktopSwitchType : int {};

// IUnknown occupies slots 0-2. The 11 methods below occupy slots 3-13;
// CurrentVirtualDesktopChanged is deliberately slot 10 on build 26200.
MIDL_INTERFACE("B9E5E94D-233E-49AB-AF5C-2B4541C3AADE")
IVirtualDesktopNotification : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopCreated(IVirtualDesktop* desktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopDestroyBegin(
        IVirtualDesktop* destroyed, IVirtualDesktop* fallback) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopDestroyFailed(
        IVirtualDesktop* destroyed, IVirtualDesktop* fallback) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopDestroyed(
        IVirtualDesktop* destroyed, IVirtualDesktop* fallback) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopMoved(
        IVirtualDesktop* desktop, INT64 oldIndex, INT64 newIndex) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopNameChanged11(
        IVirtualDesktop* desktop, HSTRING name) = 0;
    virtual HRESULT STDMETHODCALLTYPE ViewVirtualDesktopChanged11(IApplicationView* view) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentVirtualDesktopChanged(
        IVirtualDesktop* oldDesktop, IVirtualDesktop* newDesktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopWallpaperChanged(
        IVirtualDesktop* desktop, HSTRING name) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopSwitched(
        IVirtualDesktop* desktop, VirtualDesktopSwitchType type) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoteVirtualDesktopConnected(IVirtualDesktop* desktop) = 0;
};

MIDL_INTERFACE("0CD45E71-D927-4F15-8B0A-8FEF525337BF")
IVirtualDesktopNotificationService : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE Register(
        IVirtualDesktopNotification* notification, DWORD* cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE Unregister(DWORD cookie) = 0;
};

} // namespace VirtualDesktopPrivate
