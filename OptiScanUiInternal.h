#pragma once

#include "OptiScanUi.h"
#include "Resource.h"

extern HWND hAccessibleEdit;
extern HWND hAccessibleLabel;
extern bool g_accessibleMode;

extern HWND hInfoButtons[COMMAND_BUTTON_COUNT];
extern HWND hSectionLabels[SECTION_LABEL_COUNT];
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
extern COLORREF MenuTextGrey;
extern COLORREF MenuNumberGrey;
extern COLORREF OutputDark;
extern const BYTE PanelSurfaceAlpha;
extern const LPCWSTR SectionLabels[SECTION_LABEL_COUNT];

Gdiplus::REAL ScaleReal(int value);
BYTE BackgroundAlpha(BYTE alpha);
RECT GetMonitorWorkArea(HWND hWnd);
HBRUSH CreateOutputEditBrush(int width, int height);
void UpdateOutputEditBrush(int width, int height);
