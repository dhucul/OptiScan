#pragma once

#include "framework.h"

constexpr int MAX_LOADSTRING = 100;
constexpr int COMMAND_BUTTON_COUNT = 34;
constexpr int SECTION_LABEL_COUNT = 4;

// Button-index constants. Kept in sync with CommandLabels[].
// Indices 31..33 are the "special" buttons that don't dispatch a single menu
// choice via DispatchMenuChoice: Batch, Clear/Cancel, and Exit.
constexpr int kBatchButtonIndex = 31;
constexpr int kClearButtonIndex = 32;
constexpr int kExitButtonIndex  = 33;

// Posted by GuiSink and worker threads back to the UI thread.
constexpr UINT WM_APP_DRAIN_OUTPUT = WM_APP + 1;
constexpr UINT WM_APP_WORKER_DONE  = WM_APP + 2;

extern HINSTANCE hInst;
extern HWND hInfoEdit;
extern HWND g_hMainWnd;
extern HMONITOR gUiMonitor;
extern const LPCWSTR CommandLabels[COMMAND_BUTTON_COUNT];

int ScalePx(int value);
double ComputeUiScale(UINT dpi, const RECT& workArea);
HMONITOR GetNearestMonitor(HWND hWnd);

void InitializeUiResources();
void DestroyUiResources();
void RegisterUiClasses(HINSTANCE hInstance);

void SetInitialAccessibleMode(bool enabled);
bool IsAccessibleMode();
void SetUiScale(double scale);
void ApplyAccessibleMode(HWND hWnd, bool enabled, bool focusEdit);
void BindGuiSinksToMainWindow(HWND hWnd);

void SetMenuButtonsEnabled(bool enabled);
void CreateUiFonts();
void ApplyUiFonts();
void ApplyUiVisualTone();
bool UpdateUiScale(HWND hWnd, UINT dpi);
void CreateMainControls(HWND hWnd);
void LayoutMainControls(HWND hWnd);
void ApplyAccessibleVisibility();
void BuildOperationsMenu(HWND hWnd);

void AppendInfoText(HWND hCtrl, LPCWSTR text);
LRESULT HandleControlColorStatic(HWND hWnd, HDC hdc, HWND child);
LRESULT HandleControlColorEdit(HDC hdc);

void DrawMainBackground(HWND hWnd, HDC hdc);
void DrawCommandButton(const DRAWITEMSTRUCT* drawItem);
