// ============================================================================
// GuiSink.h - Routes std::cout / std::wcout / std::cerr / std::wcerr and the
// Console:: helpers into the OutputControl log panel.
//
// Call InstallStreamRedirect() once after the control is created.
// ============================================================================
#pragma once

#include <windows.h>
#include <string>

namespace GuiSink {

    // Bind the destination output control (created via OutputControl::Create).
    // Must be called before any output is generated.
    void SetOutputWindow(HWND hOutputControl);
    HWND GetOutputWindow();

    // Bind native progress controls used for live carriage-return updates.
    // CR-overwritten lines are routed here instead of into the log so the
    // log doesn't flicker.
    void SetProgressWindows(HWND hText, HWND hBar);

    // Bind a standard read-only multiline EDIT control that mirrors the log
    // text in a form a screen reader (NVDA etc.) can read natively. Every
    // character that reaches the visual OutputControl is also appended here.
    // Pass nullptr to detach. Safe to leave unset (mirroring is a no-op).
    void SetAccessibleMirror(HWND hEdit);

    // Append plain text directly to the accessible mirror EDIT. Used for the
    // few lines (e.g. ">>> <op>" headers) that are written straight to the
    // OutputControl and therefore bypass the cout/Console:: stream path.
    // Must be called on the UI thread. Lone '\n' is normalised to "\r\n".
    void MirrorText(const wchar_t* text, size_t len);
    inline void MirrorText(const std::wstring& s) { MirrorText(s.data(), s.size()); }

    // Bind the message + HWND used to wake the UI thread when worker-thread
    // output has been queued. The window's WndProc should call
    // DrainOutputQueue() when it receives this message.
    void SetDrainTarget(HWND hwnd, UINT message);

    // Replace the cout/wcout/cerr/wcerr buffers so anything written to those
    // streams is appended to the bound output control. Safe to call once.
    void InstallStreamRedirect();
    void UninstallStreamRedirect();

    // Direct append entry points (used by Console:: helpers).
    void AppendUtf8(const char* utf8, size_t len);
    void AppendWide(const wchar_t* wide, size_t len);

    inline void AppendUtf8(const std::string& s) { AppendUtf8(s.data(), s.size()); }
    inline void AppendWide(const std::wstring& s) { AppendWide(s.data(), s.size()); }

    // Append a pre-coloured line (fixed explicit colour, no ANSI/progress
    // parsing) through the same FIFO queue as stream output. SAFE TO CALL FROM
    // ANY THREAD: the OutputControl mutation and accessible-mirror update both
    // happen on the UI thread during DrainOutputQueue, preserving ordering with
    // surrounding cout/Console:: output. Used for op headers / batch markers.
    void AppendDirectColored(const wchar_t* text, size_t len, COLORREF color);
    inline void AppendDirectColored(const std::wstring& s, COLORREF color) {
        AppendDirectColored(s.data(), s.size(), color);
    }

    // Called on the UI thread (in response to the drain message) to move any
    // queued text into the bound output control.
    void DrainOutputQueue();

    // UI-thread: clear the output control and reset internal progress-line
    // tracking.
    void ClearOutput();

    // Re-seed the fallback text colour from the active theme.
    void ApplyTheme();

}  // namespace GuiSink
