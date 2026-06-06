// ============================================================================
// AccessibleAnnounce.h - UI Automation notification announcer for the main
// window.
//
// Declares the small AccessibleAnnounce facade implemented in
// AccessibleAnnounce.cpp. It lets the app push screen-reader announcements
// (UIA notification events) for status changes — "Started: Copy disc",
// "Operation finished.", etc. — without exposing any UIA types to callers.
//
// The separate, header-light Accessibility:: flag (SetEnabled / IsEnabled)
// lives in Accessibility.h and is intentionally NOT redeclared here, so this
// header can pull in <windows.h> without forcing Win32 onto the many
// translation units that only need the flag.
// ============================================================================
#pragma once

#include <windows.h>
#include <string>

namespace AccessibleAnnounce {

    // Bind the announcer to the main top-level window. Call once after the
    // window is created and before any Announce()/HandleGetObject() use.
    void Init(HWND mainWindow);

    // Service WM_GETOBJECT: supply the server-side UIA root provider when the
    // OS requests UiaRootObjectId. Sets `handled` to true and returns the
    // provider result in that case; otherwise leaves `handled` false and the
    // caller should fall through to DefWindowProc.
    LRESULT HandleGetObject(HWND hwnd, WPARAM wParam, LPARAM lParam, bool& handled);

    // Speak a status string via a UIA notification event. No-op when
    // accessible mode is off, no provider exists yet, or no client (screen
    // reader) is listening.
    void Announce(const std::wstring& text);

    // Release the UIA provider and clear the cached window handle. Call during
    // window destruction / app shutdown.
    void Shutdown();

}  // namespace AccessibleAnnounce
