#include "VirtualDesktopTracker.h"

#include "VirtualDesktopInterfaces.h"

#include <atomic>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

DWORD GetWindowsBuild() noexcept {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (!rtlGetVersion) return 0;

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return rtlGetVersion(&version) == 0 ? version.dwBuildNumber : 0;
}

bool IsZeroGuid(const GUID& guid) noexcept {
    return InlineIsEqualGUID(guid, GUID{});
}

bool ReadGuidValue(const std::wstring& keyPath, GUID* desktopId) noexcept {
    DWORD type = 0;
    DWORD size = 0;
    const LSTATUS queryStatus = RegGetValueW(
        HKEY_CURRENT_USER, keyPath.c_str(), L"CurrentVirtualDesktop",
        RRF_RT_REG_BINARY | RRF_RT_REG_SZ, &type, nullptr, &size);
    if (queryStatus != ERROR_SUCCESS || size == 0) return false;

    std::vector<BYTE> value;
    try {
        value.resize(size);
    } catch (...) {
        return false;
    }
    DWORD readSize = size;
    if (RegGetValueW(HKEY_CURRENT_USER, keyPath.c_str(), L"CurrentVirtualDesktop",
                     RRF_RT_REG_BINARY | RRF_RT_REG_SZ, &type, value.data(),
                     &readSize) != ERROR_SUCCESS || readSize != size) {
        return false;
    }

    GUID parsed{};
    if (type == REG_BINARY && size == sizeof(parsed)) {
        std::memcpy(&parsed, value.data(), sizeof(parsed));
    } else if (type == REG_SZ && size >= sizeof(wchar_t) &&
               size % sizeof(wchar_t) == 0) {
        const auto* text = reinterpret_cast<const wchar_t*>(value.data());
        const size_t length = size / sizeof(wchar_t);
        if (text[length - 1] != L'\0' || FAILED(CLSIDFromString(text, &parsed))) {
            return false;
        }

        wchar_t canonical[39]{};
        if (StringFromGUID2(parsed, canonical,
                            static_cast<int>(sizeof(canonical) / sizeof(canonical[0]))) != 39 ||
            _wcsicmp(text, canonical) != 0) {
            return false;
        }
    } else {
        return false;
    }

    if (IsZeroGuid(parsed)) return false;
    *desktopId = parsed;
    return true;
}

bool ReadCurrentDesktopGuid(GUID* desktopId) noexcept {
    DWORD sessionId = 0;
    if (!desktopId || !ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) return false;

    const std::wstring sessionPath =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SessionInfo\\" +
        std::to_wstring(sessionId) + L"\\VirtualDesktops";
    return ReadGuidValue(sessionPath, desktopId) || ReadGuidValue(
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops", desktopId);
}

class DesktopNotificationSink final
    : public VirtualDesktopPrivate::IVirtualDesktopNotification {
public:
    explicit DesktopNotificationSink(std::function<void(const GUID&)> queue)
        : m_queue(std::move(queue)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (!InlineIsEqualGUID(iid, IID_IUnknown) &&
            !InlineIsEqualGUID(iid, VirtualDesktopPrivate::kNotificationSinkIid)) {
            return E_NOINTERFACE;
        }
        *object = static_cast<VirtualDesktopPrivate::IVirtualDesktopNotification*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_references.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = m_references.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (references == 0) delete this;
        return references;
    }

    HRESULT STDMETHODCALLTYPE VirtualDesktopCreated(
        VirtualDesktopPrivate::IVirtualDesktop*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE VirtualDesktopDestroyBegin(
        VirtualDesktopPrivate::IVirtualDesktop*, VirtualDesktopPrivate::IVirtualDesktop*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE VirtualDesktopDestroyFailed(
        VirtualDesktopPrivate::IVirtualDesktop*, VirtualDesktopPrivate::IVirtualDesktop*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE VirtualDesktopDestroyed(
        VirtualDesktopPrivate::IVirtualDesktop*, VirtualDesktopPrivate::IVirtualDesktop*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE VirtualDesktopMoved(
        VirtualDesktopPrivate::IVirtualDesktop*, INT64, INT64) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE VirtualDesktopNameChanged11(
        VirtualDesktopPrivate::IVirtualDesktop*, HSTRING) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ViewVirtualDesktopChanged11(
        VirtualDesktopPrivate::IApplicationView*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CurrentVirtualDesktopChanged(
        VirtualDesktopPrivate::IVirtualDesktop*,
        VirtualDesktopPrivate::IVirtualDesktop* newDesktop) override {
        if (!newDesktop) return E_POINTER;

        GUID desktopId{};
        const HRESULT getIdResult = newDesktop->GetId(&desktopId);
        if (FAILED(getIdResult) || IsZeroGuid(desktopId)) {
            return FAILED(getIdResult) ? getIdResult : E_FAIL;
        }

        std::lock_guard lock(m_queueMutex);
        if (m_queue) m_queue(desktopId);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE VirtualDesktopWallpaperChanged(
        VirtualDesktopPrivate::IVirtualDesktop*, HSTRING) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE VirtualDesktopSwitched(
        VirtualDesktopPrivate::IVirtualDesktop*,
        VirtualDesktopPrivate::VirtualDesktopSwitchType) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RemoteVirtualDesktopConnected(
        VirtualDesktopPrivate::IVirtualDesktop*) override { return S_OK; }

    void Detach() noexcept {
        std::lock_guard lock(m_queueMutex);
        m_queue = {};
    }

private:
    std::atomic<ULONG> m_references{1};
    std::mutex m_queueMutex;
    std::function<void(const GUID&)> m_queue;
};

} // namespace

struct VirtualDesktopTracker::Impl {
    DWORD ownerThreadId = 0;
    bool tracking = false;
    GUID currentDesktopId{};
    ChangedCallback callback;
    Diagnostic diagnostic;
    std::mutex pendingMutex;
    std::deque<GUID> pendingDesktopIds;
    IServiceProvider* serviceProvider = nullptr;
    VirtualDesktopPrivate::IVirtualDesktopNotificationService* notificationService = nullptr;
    DesktopNotificationSink* notificationSink = nullptr;
    bool apartmentInitialized = false;

    HRESULT ReleaseResources() noexcept;
};

HRESULT VirtualDesktopTracker::Impl::ReleaseResources() noexcept {
    HRESULT cleanupResult = S_OK;
    diagnostic.unregisterHresult = S_OK;
    if (notificationService && diagnostic.notificationCookie != 0) {
        diagnostic.unregisterHresult = notificationService->Unregister(diagnostic.notificationCookie);
        if (FAILED(diagnostic.unregisterHresult)) cleanupResult = diagnostic.unregisterHresult;
    }

    if (notificationSink) notificationSink->Detach();
    if (notificationService) notificationService->Release();
    if (notificationSink) notificationSink->Release();
    if (serviceProvider) serviceProvider->Release();
    notificationService = nullptr;
    notificationSink = nullptr;
    serviceProvider = nullptr;
    diagnostic.notificationCookie = 0;

    {
        std::lock_guard lock(pendingMutex);
        pendingDesktopIds.clear();
    }
    callback = {};
    currentDesktopId = GUID{};
    tracking = false;
    ownerThreadId = 0;
    if (apartmentInitialized) {
        apartmentInitialized = false;
        CoUninitialize();
    }
    return cleanupResult;
}

VirtualDesktopTracker::VirtualDesktopTracker() : m_impl(std::make_unique<Impl>()) {}

VirtualDesktopTracker::~VirtualDesktopTracker() noexcept {
    Stop();
}

bool VirtualDesktopTracker::Start(ChangedCallback callback) {
    if (m_impl->tracking || m_impl->ownerThreadId != 0) return false;

    m_impl->diagnostic = {};
    m_impl->diagnostic.windowsBuild = GetWindowsBuild();
    if (m_impl->diagnostic.windowsBuild != VirtualDesktopPrivate::kSupportedWindowsBuild) {
        m_impl->diagnostic.interfaceRevision = L"unsupported-build";
        m_impl->diagnostic.hresult = E_NOTIMPL;
        return false;
    }
    m_impl->diagnostic.interfaceRevision = VirtualDesktopPrivate::kRevision;
    m_impl->diagnostic.interfaceIid = VirtualDesktopPrivate::kNotificationSinkIid;
    if (!callback) {
        m_impl->diagnostic.hresult = E_INVALIDARG;
        return false;
    }

    m_impl->ownerThreadId = GetCurrentThreadId();
    m_impl->callback = std::move(callback);
    const auto fail = [this](HRESULT result) {
        m_impl->diagnostic.hresult = result;
        const HRESULT cleanupResult = m_impl->ReleaseResources();
        if (FAILED(cleanupResult)) m_impl->diagnostic.hresult = cleanupResult;
        return false;
    };

    const HRESULT apartmentResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartmentResult)) return fail(apartmentResult);
    m_impl->apartmentInitialized = true;

    GUID initialDesktopId{};
    if (!ReadCurrentDesktopGuid(&initialDesktopId)) return fail(E_FAIL);
    m_impl->currentDesktopId = initialDesktopId;

    const HRESULT activationResult = CoCreateInstance(
        VirtualDesktopPrivate::kImmersiveShellClsid, nullptr, CLSCTX_LOCAL_SERVER,
        IID_IServiceProvider, reinterpret_cast<void**>(&m_impl->serviceProvider));
    if (FAILED(activationResult) || !m_impl->serviceProvider) {
        return fail(FAILED(activationResult) ? activationResult : E_FAIL);
    }

    const HRESULT queryServiceResult = m_impl->serviceProvider->QueryService(
        VirtualDesktopPrivate::kNotificationServiceSid,
        VirtualDesktopPrivate::kNotificationServiceIid,
        reinterpret_cast<void**>(&m_impl->notificationService));
    if (FAILED(queryServiceResult) || !m_impl->notificationService) {
        return fail(FAILED(queryServiceResult) ? queryServiceResult : E_FAIL);
    }

    m_impl->notificationSink = new (std::nothrow) DesktopNotificationSink(
        [this](const GUID& desktopId) { QueueDesktopChange(desktopId); });
    if (!m_impl->notificationSink) return fail(E_OUTOFMEMORY);

    const HRESULT registerResult = m_impl->notificationService->Register(
        m_impl->notificationSink, &m_impl->diagnostic.notificationCookie);
    if (FAILED(registerResult) || m_impl->diagnostic.notificationCookie == 0) {
        return fail(FAILED(registerResult) ? registerResult : E_FAIL);
    }

    if (!ReadCurrentDesktopGuid(&m_impl->currentDesktopId)) return fail(E_FAIL);
    m_impl->tracking = true;
    m_impl->diagnostic.hresult = S_OK;
    return true;
}

bool VirtualDesktopTracker::PumpPending() {
    if (!m_impl->tracking || m_impl->ownerThreadId != GetCurrentThreadId()) return false;

    for (;;) {
        GUID desktopId{};
        {
            std::lock_guard lock(m_impl->pendingMutex);
            if (m_impl->pendingDesktopIds.empty()) return false;
            desktopId = m_impl->pendingDesktopIds.front();
            m_impl->pendingDesktopIds.pop_front();
        }
        if (InlineIsEqualGUID(desktopId, m_impl->currentDesktopId)) continue;

        m_impl->currentDesktopId = desktopId;
        m_impl->callback(m_impl->currentDesktopId);
        return true;
    }
}

void VirtualDesktopTracker::Stop() noexcept {
    if (m_impl->ownerThreadId == 0) return;
    if (m_impl->ownerThreadId != GetCurrentThreadId()) return;

    const HRESULT cleanupResult = m_impl->ReleaseResources();
    if (FAILED(cleanupResult)) m_impl->diagnostic.hresult = cleanupResult;
}

bool VirtualDesktopTracker::IsTracking() const noexcept {
    return m_impl->tracking;
}

GUID VirtualDesktopTracker::GetCurrentDesktopId() const noexcept {
    return m_impl->currentDesktopId;
}

const VirtualDesktopTracker::Diagnostic& VirtualDesktopTracker::GetDiagnostic() const noexcept {
    return m_impl->diagnostic;
}

void VirtualDesktopTracker::QueueDesktopChange(const GUID& desktopId) noexcept {
    if (IsZeroGuid(desktopId)) return;
    std::lock_guard lock(m_impl->pendingMutex);
    if (m_impl->pendingDesktopIds.empty() ||
        !InlineIsEqualGUID(m_impl->pendingDesktopIds.back(), desktopId)) {
        m_impl->pendingDesktopIds.push_back(desktopId);
    }
}
