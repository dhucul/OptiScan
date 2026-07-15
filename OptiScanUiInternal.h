#pragma once

#include "OptiScanUi.h"
#include "Resource.h"

extern HWND hAccessibleEdit;
extern HWND hAccessibleLabel;
extern bool g_accessibleMode;

extern HWND hInfoButtons[COMMAND_BUTTON_COUNT];
extern HWND hProgressText;
extern HWND hProgressBar;
extern HFONT hCommandFont;
extern HFONT hHeaderFont;
extern HFONT hOutputFont;
extern HBRUSH hDarkEditBrush;
extern HBRUSH hOutputSolidBrush;
extern HBITMAP hOutputBrushBitmap;
extern int gOutputBrushWidth;
extern int gOutputBrushHeight;
extern ULONG_PTR gGdiPlusToken;
extern double gUiScale;

extern COLORREF SoftOrange;
extern COLORREF AccentBlue;
extern COLORREF WarmText;
extern COLORREF MenuTextOrange;
extern COLORREF OutputDark;

// Navigation-rail geometry. Painted by OptiScanUiPaint, reserved by
// OptiScanUiLayout, and hit-tested by OptiScanUi -- all three must agree or
// clicks land on the wrong nav item, so it lives in exactly one place.
int SidebarWidth();
int NavItemTop(int index);
constexpr int kNavItemCount = 6;
constexpr int kNavItemHeight = 58;

Gdiplus::REAL ScaleReal(int value);
BYTE BackgroundAlpha(BYTE alpha);
RECT GetMonitorWorkArea(HWND hWnd);
HBRUSH CreateOutputEditBrush(int width, int height);
void UpdateOutputEditBrush(int width, int height);
