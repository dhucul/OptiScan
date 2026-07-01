// ============================================================================
// OutputControl.h - Custom text-display window for the OptiScan log panel.
//
// Replaces the RichEdit + custom-paint approach. Stores text as a list of
// (text, colour) segments organised into lines, and paints the visible
// portion directly. No layout queries to a foreign control means scrolling
// and colouring are both consistent — the control owns everything it paints.
//
// The control:
//   - has a vertical scrollbar
//   - auto-scrolls to bottom on Append unless the user has scrolled away
//   - applies a per-line colour heuristic to runs marked non-explicit
//     (so a cout flush that happens to contain "done" colours the *line*,
//     not the chunk)
//   - paints over a cached background image (optional)
// ============================================================================
#pragma once

#include <windows.h>
#include <cstddef>

namespace Gdiplus { class Image; }

namespace OutputControl {

    // Call once at startup.
    void RegisterClass(HINSTANCE hInstance);

    // Creates the control with WS_CHILD | WS_VISIBLE | WS_VSCROLL.
    HWND Create(HWND parent, int id, HINSTANCE hInstance);

    // Optional configuration.
    void SetFont(HWND hCtrl, HFONT font);
    void SetBackgroundImage(HWND hCtrl, Gdiplus::Image* image);
    void SetBackgroundTone(HWND hCtrl, BYTE toneAlpha, BYTE gridAlpha);

    // Append a styled text segment.
    //   isExplicit == true  → `color` is used as-is for this segment.
    //   isExplicit == false → `color` is ignored; at paint time the segment
    //                         inherits the line-level heuristic colour.
    // Embedded '\n' starts a new line. '\r' is ignored.
    void Append(HWND hCtrl, const wchar_t* text, size_t len,
                COLORREF color, bool isExplicit);

    // Convenience wrapper for null-terminated strings.
    void Append(HWND hCtrl, const wchar_t* text,
                COLORREF color, bool isExplicit);

    // Wipe all content. Scroll resets to top and auto-follow re-engages.
    void Clear(HWND hCtrl);

    // Re-seed the log colour roles from the active theme and repaint. Pass the
    // control HWND (may be null before the control exists - statics still
    // update, repaint is skipped).
    void ApplyTheme(HWND hCtrl);

}  // namespace OutputControl
