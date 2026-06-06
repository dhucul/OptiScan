// ============================================================================
// Accessibility.h - Lightweight, thread-safe "accessible mode" flag.
//
// Deliberately free of <windows.h> and any heavy include so it can be pulled
// into widely-shared headers (e.g. ConsoleGraph.h) without dragging Win32 into
// every translation unit. The flag is owned by the UI thread (toggled via the
// View > Accessible output menu) and read by the scan worker threads so they
// can emit screen-reader-friendly text instead of visual-only graphics.
// ============================================================================
#pragma once

namespace Accessibility {

    // Set from the UI thread when accessible mode is turned on/off.
    void SetEnabled(bool enabled);

    // Thread-safe read. True when output should favour plain text that a
    // screen reader can read over decorative bar/graph rendering.
    bool IsEnabled();

}  // namespace Accessibility
