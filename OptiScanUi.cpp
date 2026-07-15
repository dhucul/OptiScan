// OptiScanUi.cpp - shared Win32 UI state and lifecycle helpers.

#include "framework.h"
#include "OptiScanUiInternal.h"

#include "Accessibility.h"
#include "GuiInput.h"
#include "GuiSink.h"
#include "OutputControl.h"
#include "Theme.h"
#include <commctrl.h>
#include <cmath>

// Progress.h (pulled in elsewhere) does `#undef min` / `#undef max`, which
// would otherwise break the legacy `min(a, b)` / `max(a, b)` calls in the GUI
// layout / paint code. Restore the Windows-style macros for this module too.
#undef min
#undef max
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) < (b)) ? (b) : (a))

HWND hInfoEdit = nullptr;                         // custom information area
HWND g_hMainWnd = nullptr;                        // top-level window (for IsDialogMessage)

// Accessibility: a standard read-only multiline EDIT that mirrors the log in a
// form screen readers (NVDA etc.) can read natively. When g_accessibleMode is
// on it replaces the custom-painted OutputControl on screen; the mirror EDIT is
// always populated either way so toggling it on mid-session shows full history.
HWND hAccessibleEdit  = nullptr;
HWND hAccessibleLabel = nullptr;
bool g_accessibleMode = false;

HWND hInfoButtons[COMMAND_BUTTON_COUNT];        // quick action buttons
HWND hSectionLabels[SECTION_LABEL_COUNT];       // command group labels
HWND hProgressText;                             // live progress status
HWND hProgressBar;                              // live progress meter
HFONT hCommandFont;
HFONT hHeaderFont;
HFONT hOutputFont;
HBRUSH hDarkEditBrush;
HBRUSH hOutputSolidBrush;       // solid fallback background for simple controls
HBITMAP hOutputBrushBitmap;
int gOutputBrushWidth;
int gOutputBrushHeight;
ULONG_PTR gGdiPlusToken;
Gdiplus::Image* gBackgroundImage;
Gdiplus::Image* gOutputBackgroundImage;
double gUiScale = 1.0;
HMONITOR gUiMonitor = nullptr;
static int gProfessionalNavIndex = 0;

int GetProfessionalNavIndex()
{
    return gProfessionalNavIndex;
}

bool HandleProfessionalSidebarClick(HWND hWnd, int x, int y)
{
    if (CurrentThemeId() != ThemeId::AppleLight || x < 0 || x >= ScalePx(360))
        return false;

    for (int i = 0; i < 6; ++i)
    {
        const int top = ScalePx(145 + i * 76);
        if (y < top || y >= top + ScalePx(58)) continue;

        gProfessionalNavIndex = i;
        LayoutMainControls(hWnd);
        InvalidateRect(hWnd, nullptr, TRUE);

        const int focusIndices[] = { 0, 0, 5, 12, 18, 25 };
        const int focusIndex = focusIndices[i];
        if (hInfoButtons[focusIndex] && IsWindowVisible(hInfoButtons[focusIndex]))
            SetFocus(hInfoButtons[focusIndex]);
        return true;
    }
    return false;
}

// Chrome colour table. Seeded from the active theme by OnThemeChangedUi();
// mutable so the runtime theme switch can re-tint the window chrome. Defaults
// are the Graphite values (overwritten before first paint by InitializeTheme).
extern COLORREF SoftOrange = RGB(146, 156, 166);
extern COLORREF AccentBlue = RGB(156, 168, 180);
extern COLORREF WarmText = RGB(208, 215, 220);
extern COLORREF MenuTextOrange = RGB(198, 178, 150);
extern COLORREF MenuTextGrey = RGB(216, 222, 226);
extern COLORREF MenuNumberGrey = RGB(178, 188, 196);
extern COLORREF OutputDark = RGB(14, 17, 22);   // Output background color.
extern const BYTE PanelSurfaceAlpha = 92;

extern const LPCWSTR CommandLabels[COMMAND_BUTTON_COUNT] =
{
    L"1. Copy disc *",
    L"2. Rip tracks (WAV/FLAC) *",
    L"3. Write disc (.bin/.cue/.sub files) *",
    L"4. Write tracks to disc using current disc's pregaps *",
    L"5. Recovery rip (drive-independent) *",
    L"6. Quality scan (C1/C2/CU graphs) *",
    L"7. C2 error scan *",
    L"8. BLER scan (detailed) *",
    L"9. Disc rot detection *",
    L"10. Generate surface map *",
    L"11. Multi-pass verification *",
    L"12. Compare disc CRCs (original vs. copy)",
    L"13. Audio content analysis *",
    L"14. Disc fingerprint (CDDB/MusicBrainz/AccurateRip IDs)",
    L"15. Lead area check",
    L"16. Subchannel integrity check *",
    L"17. Verify subchannel burn status *",
    L"18. Copy-protection check",
    L"19. Drive capabilities",
    L"20. Drive offset detection",
    L"21. C2 validation test",
    L"22. Speed comparison test",
    L"23. Seek time analysis",
    L"24. Chipset identification",
    L"25. Disc balance check *",
    L"26. Rescan disc",
    L"27. Check for updates",
    L"28. Help (test descriptions)",
    L"29. Pioneer CD Check (audio quality)",
    L"30. Jitter / beta scan (LiteOn)",
    L"31. Erase CD-RW (rewritable)",
    L"32. FE/TE servo scan (experimental, LiteOn)",
    L"33. Batch run (multiple ops, 1 prescan)",
    L"34. Clear info box",
    L"35. Exit"
};

extern const LPCWSTR SectionLabels[SECTION_LABEL_COUNT] =
{
    L"DISC QUALITY",
    L"DISC INFO",
    L"DRIVE",
    L"UTILITY"
};


void SetInitialAccessibleMode(bool enabled)
{
    g_accessibleMode = enabled;
    Accessibility::SetEnabled(enabled);
}

bool IsAccessibleMode()
{
    return g_accessibleMode;
}

void SetUiScale(double scale)
{
    gUiScale = scale;
}

void RegisterUiClasses(HINSTANCE hInstance)
{
    OutputControl::RegisterClass(hInstance);
}

void BindGuiSinksToMainWindow(HWND hWnd)
{
    // Bind streams and dialog parent to the new window and its output controls.
    GuiSink::SetOutputWindow(hInfoEdit);
    GuiSink::SetProgressWindows(hProgressText, hProgressBar);
    GuiSink::SetAccessibleMirror(hAccessibleEdit);
    GuiSink::SetDrainTarget(hWnd, WM_APP_DRAIN_OUTPUT);
    GuiSink::InstallStreamRedirect();
    GuiInput::SetOwnerWindow(hWnd);

    // Sync the menu check / control visibility with the auto-detected mode, and
    // land focus on the readable EDIT when we start out accessible.
    ApplyAccessibleMode(hWnd, g_accessibleMode, /*focusEdit=*/g_accessibleMode);
}

int ScalePx(int value)
{
    if (value == 0) return 0;
    const int scaled = (int)std::lround((double)value * gUiScale);
    return value > 0 ? max(1, scaled) : min(-1, scaled);
}

Gdiplus::REAL ScaleReal(int value)
{
    return (Gdiplus::REAL)((double)value * gUiScale);
}

BYTE BackgroundAlpha(BYTE alpha)
{
    // Lightly relax only the background veils. Scaled layouts need a bit more
    // compensation because down-sampled artwork reads denser; 5K can use the
    // artwork directly without an extra background veil.
    const double compensation = (gUiScale >= 0.999)
        ? 0.25
        : 0.75 + (0.25 * gUiScale);
    const int adjusted = (int)std::lround((double)alpha * compensation);
    return (BYTE)max(0, min(255, adjusted));
}

// True when the user is running a Windows High Contrast theme. In that case we
// must not override control colours — the system palette is the user's choice.
static bool IsHighContrastActive()
{
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
    {
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }
    return false;
}

static bool IsFiveKWorkArea(const RECT& workArea)
{
    const int width = workArea.right - workArea.left;
    const int height = workArea.bottom - workArea.top;
    return max(width, height) >= 5000 && min(width, height) >= 2700;
}

double ComputeUiScale(UINT dpi, const RECT& workArea)
{
    if (IsFiveKWorkArea(workArea))
    {
        return 1.0;
    }

    const int workWidth = max(1, workArea.right - workArea.left);
    const int workHeight = max(1, workArea.bottom - workArea.top);
    const UINT effectiveDpi = dpi < 96 ? 96 : dpi;
    double dpiScale = (double)effectiveDpi / 192.0;
    double fitScale = min((double)max(1, workWidth - 80) / 3200.0,
                          (double)max(1, workHeight - 80) / 1800.0);
    double scale = min(1.0, min(dpiScale, fitScale));
    scale = max(0.50, scale);
    return scale;
}

HMONITOR GetNearestMonitor(HWND hWnd)
{
    RECT rc{};
    if (hWnd)
    {
        GetWindowRect(hWnd, &rc);
    }
    else
    {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    }
    return MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
}

RECT GetMonitorWorkArea(HWND hWnd)
{
    HMONITOR monitor = GetNearestMonitor(hWnd);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
    {
        return monitorInfo.rcWork;
    }

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    return workArea;
}

// Disable every button except the Cancel/Clear one (commandIndex 29) so the
// user can't queue or interfere with a running workflow. Pass `true` to
// re-enable all buttons (workflow finished).
void SetMenuButtonsEnabled(bool enabled) {
    // Clear button stays clickable so it can act as Cancel while a workflow runs.
    for (int i = 0; i < COMMAND_BUTTON_COUNT; i++) {
        if (i == kClearButtonIndex) continue;
        if (hInfoButtons[i]) EnableWindow(hInfoButtons[i], enabled ? TRUE : FALSE);
    }
    // Keep the Operations menu items in lockstep with the buttons. EnableMenuItem
    // with MF_BYCOMMAND searches submenus, so the categorised items are covered.
    // Ids not present in the menu (Clear/Exit) simply no-op.
    if (HMENU bar = (g_hMainWnd ? GetMenu(g_hMainWnd) : nullptr)) {
        for (int i = 0; i < COMMAND_BUTTON_COUNT; i++) {
            if (i == kClearButtonIndex) continue;
            EnableMenuItem(bar, (UINT)(IDC_INFO_BUTTON1 + i),
                           MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
        }
    }
}

LRESULT HandleControlColorStatic(HWND hWnd, HDC hdc, HWND child)
{
    // The accessible mirror is a read-only EDIT (which sends
    // WM_CTLCOLORSTATIC, not WM_CTLCOLOREDIT). Paint it in the app's dark
    // console theme so it doesn't flash a white box over the dark UI, but
    // in High Contrast mode defer to the system colours the user chose.
    if (child == hAccessibleEdit)
    {
        if (IsHighContrastActive())
        {
            return DefWindowProc(hWnd, WM_CTLCOLORSTATIC, (WPARAM)hdc, (LPARAM)child);
        }
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, OutputDark);
        SetTextColor(hdc, WarmText);
        return (INT_PTR)(hOutputSolidBrush ? hOutputSolidBrush
                                           : (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    if (child == hProgressText)
    {
        // Progress strip sits on the same cool near-black as the output edit
        // so the lower panel reads as one surface.
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, OutputDark);
        SetTextColor(hdc, MenuTextOrange);
        return (INT_PTR)(hOutputSolidBrush ? hOutputSolidBrush
                                           : hDarkEditBrush);
    }
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, SoftOrange);
    return (INT_PTR)GetStockObject(NULL_BRUSH);
}

LRESULT HandleControlColorEdit(HDC hdc)
{
    // The output panel is no longer a RichEdit, so its WM_CTLCOLOREDIT never
    // fires here. Any other Win32 edit children (none currently) would use a
    // transparent dark theme.
    SetTextColor(hdc, WarmText);
    SetBkColor(hdc, OutputDark);
    SetBkMode(hdc, TRANSPARENT);
    return (INT_PTR)hDarkEditBrush;
}

// Single entry point for a theme change, called by SetActiveTheme(). Re-seeds
// the chrome colour table, propagates to the other modules' colour tables,
// rebuilds cached brushes that bake in the output background, and repaints.
// Safe to call before the window / controls exist (all HWND/HBRUSH use is
// guarded), so InitializeTheme() at startup lands the colours before first paint.
void OnThemeChangedUi()
{
    const Palette& p = ActiveTheme();
    SoftOrange     = p.chromeText;
    // A brighter lavender progress fill echoes the Apple theme's purple
    // instrumentation canvas; other themes retain their native accent.
    AccentBlue     = CurrentThemeId() == ThemeId::AppleLight
        ? RGB(174, 151, 255)
        : p.chromeAccent;
    WarmText       = p.fg;
    MenuTextOrange = p.accentWarm;
    MenuTextGrey   = p.btnLabel;
    MenuNumberGrey = p.btnNumber;
    OutputDark     = p.outputBg;

    // Propagate to the log, prompt-dialog and stream-sink colour tables.
    OutputControl::ApplyTheme(hInfoEdit);
    GuiInput::ApplyTheme();
    GuiSink::ApplyTheme();

    // Rebuild cached brushes that baked in the old OutputDark.
    if (hOutputSolidBrush)
    {
        DeleteObject(hOutputSolidBrush);
        hOutputSolidBrush = CreateSolidBrush(OutputDark);
    }
    if (hDarkEditBrush)
    {
        DeleteObject(hDarkEditBrush);
        hDarkEditBrush = nullptr;
        if (hOutputBrushBitmap)
        {
            DeleteObject(hOutputBrushBitmap);
            hOutputBrushBitmap = nullptr;
        }
        const int w = gOutputBrushWidth  > 0 ? gOutputBrushWidth  : ScalePx(960);
        const int h = gOutputBrushHeight > 0 ? gOutputBrushHeight : ScalePx(420);
        hDarkEditBrush = CreateOutputEditBrush(w, h);
    }

    if (hProgressBar)
    {
        SendMessageW(hProgressBar, PBM_SETBARCOLOR, 0, AccentBlue);
        SendMessageW(hProgressBar, PBM_SETBKCOLOR, 0, OutputDark);
    }

    // Repaint the whole window + owner-drawn children.
    if (g_hMainWnd)
    {
        LayoutMainControls(g_hMainWnd);
        InvalidateRect(g_hMainWnd, nullptr, TRUE);
        for (int i = 0; i < COMMAND_BUTTON_COUNT; ++i)
        {
            if (hInfoButtons[i]) InvalidateRect(hInfoButtons[i], nullptr, TRUE);
        }
        if (hProgressText)   InvalidateRect(hProgressText, nullptr, TRUE);
        if (hAccessibleEdit) InvalidateRect(hAccessibleEdit, nullptr, TRUE);
    }
}

void AppendInfoText(HWND /*hCtrl*/, LPCWSTR text)
{
    if (!text) return;
    // Op headers, batch step markers and cancellation notices are enqueued as
    // a pre-coloured line and applied on the UI thread during the drain. This
    // MUST NOT write to the OutputControl directly: AppendInfoText is called
    // from worker threads (e.g. batch step headers), and mutating the control's
    // vectors off the UI thread raced with the concurrent drain and corrupted
    // the log (random heap-corruption crash in vector reallocation). The queue
    // path also mirrors to the accessible EDIT, so no separate MirrorText call.
    GuiSink::AppendDirectColored(text, (size_t)lstrlenW(text), MenuTextOrange);
}

