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
extern Gdiplus::Image* gBackgroundImage;
extern Gdiplus::Image* gOutputBackgroundImage;
extern double gUiScale;

extern const COLORREF AccentOrange;
extern const COLORREF SoftOrange;
extern const COLORREF AccentBlue;
extern const COLORREF ConsoleGreen;
extern const COLORREF WarmText;
extern const COLORREF MenuTextOrange;
extern const COLORREF MenuTextGrey;
extern const COLORREF MenuNumberGrey;
extern const COLORREF MutedText;
extern const COLORREF PanelDark;
extern const COLORREF OutputDark;
extern const BYTE PanelSurfaceAlpha;
extern const LPCWSTR SectionLabels[SECTION_LABEL_COUNT];

Gdiplus::REAL ScaleReal(int value);
BYTE BackgroundAlpha(BYTE alpha);
RECT GetMonitorWorkArea(HWND hWnd);
HBRUSH CreateOutputEditBrush(int width, int height);
void UpdateOutputEditBrush(int width, int height);
