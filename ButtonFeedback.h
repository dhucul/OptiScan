#pragma once
#include <windows.h>
#include <commctrl.h>

// Shared pointer feedback for owner-drawn command and prompt buttons.
inline LRESULT CALLBACK CommandFeedbackProc(HWND hwnd, UINT msg, WPARAM wParam,
                                            LPARAM lParam, UINT_PTR id, DWORD_PTR)
{
    switch (msg) {
    case WM_MOUSEMOVE:
        if (IsWindowEnabled(hwnd) && !GetPropW(hwnd, L"OptiScan.CommandHover")) {
            SetPropW(hwnd, L"OptiScan.CommandHover", reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tracking);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
    case WM_ENABLE:
    case WM_SHOWWINDOW:
        RemovePropW(hwnd, L"OptiScan.CommandHover");
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        RemovePropW(hwnd, L"OptiScan.CommandHover");
        RemoveWindowSubclass(hwnd, CommandFeedbackProc, id);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

inline void InstallButtonFeedback(HWND hwnd) {
    if (hwnd) SetWindowSubclass(hwnd, CommandFeedbackProc, 1, 0);
}
