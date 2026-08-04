// ============================================================================
// AccessibleAnnounce.cpp - UIA notification announcer + the Accessibility flag.
//
// Implements both the lightweight Accessibility::IsEnabled flag and the UI
// Automation notification announcer. They live together so there is a single
// translation unit owning the accessibility state.
// ============================================================================
#define NOMINMAX
#include "AccessibleAnnounce.h"
#include "Accessibility.h"

#include <uiautomation.h>
#include <atomic>
#include <new>

#pragma comment(lib, "uiautomationcore.lib")

// ---------------------------------------------------------------------------
// Accessibility flag (read from worker threads, written from the UI thread).
// ---------------------------------------------------------------------------
namespace {
    std::atomic<bool> g_accessibilityEnabled{ false };
}

namespace Accessibility {
    void SetEnabled(bool enabled) {
        g_accessibilityEnabled.store(enabled, std::memory_order_relaxed);
    }
    bool IsEnabled() {
        return g_accessibilityEnabled.load(std::memory_order_relaxed);
    }
}  // namespace Accessibility

// ---------------------------------------------------------------------------
// Minimal server-side UIA provider for the main window.
//
// It carries no element tree of its own: get_HostRawElementProvider returns
// the default HWND provider, so the standard control children (command
// buttons, the output EDIT) stay fully exposed via the normal MSAA/UIA bridge.
// The provider exists only as the source object for notification events.
// ---------------------------------------------------------------------------
namespace {

HWND g_mainWnd = nullptr;

class RootProvider final : public IRawElementProviderSimple {
public:
    explicit RootProvider(HWND h) : m_hwnd(h) {}

    // IUnknown
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IRawElementProviderSimple)) {
            *ppv = static_cast<IRawElementProviderSimple*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IRawElementProviderSimple
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID, IUnknown** pRetVal) override {
        if (pRetVal) *pRetVal = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        pRetVal->vt = VT_EMPTY;
        if (propertyId == UIA_NamePropertyId) {
            pRetVal->vt = VT_BSTR;
            pRetVal->bstrVal = SysAllocString(L"OptiScan");
        }
        return S_OK;
    }
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) override {
        if (!pRetVal) return E_POINTER;
        return UiaHostProviderFromHwnd(m_hwnd, pRetVal);
    }

private:
    HWND m_hwnd;
    std::atomic<ULONG> m_ref{ 1 };
};

RootProvider* g_provider = nullptr;

}  // namespace

namespace AccessibleAnnounce {

void Init(HWND mainWindow) {
    g_mainWnd = mainWindow;
}

LRESULT HandleGetObject(HWND hwnd, WPARAM wParam, LPARAM lParam, bool& handled) {
    handled = false;
    if (static_cast<long>(lParam) == static_cast<long>(UiaRootObjectId)) {
        if (!g_provider) {
            g_provider = new (std::nothrow) RootProvider(hwnd);
        }
        if (!g_provider) return 0;
        handled = true;
        return UiaReturnRawElementProvider(hwnd, wParam, lParam, g_provider);
    }
    return 0;
}

void Announce(const std::wstring& text) {
    if (text.empty()) return;
    if (!Accessibility::IsEnabled()) return;     // mode off
    if (!g_provider) return;                       // no provider yet
    if (!UiaClientsAreListening()) return;         // no screen reader attached

    // Read every status message in order. NotificationProcessing_MostRecent
    // would let a later message cancel an earlier queued one with the same
    // activity id (e.g. "Started" dropped when "25 percent" arrives), so use
    // ImportantAll. NotificationKind_Other is the neutral "general status"
    // kind; the screen reader speaks the display string regardless.
    BSTR display  = SysAllocString(text.c_str());
    BSTR activity = SysAllocString(L"OptiScan.Status");
    UiaRaiseNotificationEvent(g_provider,
                              NotificationKind_Other,
                              NotificationProcessing_ImportantAll,
                              display, activity);
    if (display)  SysFreeString(display);
    if (activity) SysFreeString(activity);
}

void Shutdown() {
    if (g_provider) {
        UiaDisconnectProvider(g_provider);
        g_provider->Release();
        g_provider = nullptr;
    }
    g_mainWnd = nullptr;
}

}  // namespace AccessibleAnnounce
