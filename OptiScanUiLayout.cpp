// OptiScanUiLayout.cpp - control creation, layout, fonts, and accessibility visibility.

#include "framework.h"
#include "OptiScanUiInternal.h"

#include "Accessibility.h"
#include "OutputControl.h"
#include "UiSound.h"
#include <commctrl.h>
#include <cmath>

// Progress.h (pulled in elsewhere) does `#undef min` / `#undef max`, which
// would otherwise break the legacy `min(a, b)` / `max(a, b)` calls in the GUI
// layout / paint code. Restore the Windows-style macros for this module too.
#undef min
#undef max
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) < (b)) ? (b) : (a))

void CreateUiFonts()
{
    if (hCommandFont) DeleteObject(hCommandFont);
    if (hHeaderFont) DeleteObject(hHeaderFont);
    if (hOutputFont) DeleteObject(hOutputFont);

    hCommandFont = CreateFontW(-ScalePx(24), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    hHeaderFont = CreateFontW(-ScalePx(25), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    // Cascadia Mono ships with Windows 11 and Windows Terminal; it has much
    // broader Unicode coverage than Consolas (covers General Punctuation,
    // box drawing, block elements, geometric shapes, arrows, miscellaneous
    // symbols, etc.) so most of what the workflows draw renders without
    // needing the fallback chain. If Cascadia Mono isn't installed, Windows
    // substitutes another FIXED_PITCH | FF_MODERN face.
    hOutputFont = CreateFontW(-ScalePx(24), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
}

void ApplyUiFonts()
{
    if (hInfoEdit) OutputControl::SetFont(hInfoEdit, hOutputFont);
    if (hProgressText) SendMessageW(hProgressText, WM_SETFONT, (WPARAM)hOutputFont, TRUE);
    if (hAccessibleEdit) SendMessageW(hAccessibleEdit, WM_SETFONT, (WPARAM)hOutputFont, TRUE);
    if (hAccessibleLabel) SendMessageW(hAccessibleLabel, WM_SETFONT, (WPARAM)hOutputFont, TRUE);

    for (int i = 0; i < SECTION_LABEL_COUNT; ++i)
    {
        if (hSectionLabels[i]) SendMessageW(hSectionLabels[i], WM_SETFONT, (WPARAM)hHeaderFont, TRUE);
    }

    for (int i = 0; i < COMMAND_BUTTON_COUNT; ++i)
    {
        if (hInfoButtons[i]) SendMessageW(hInfoButtons[i], WM_SETFONT, (WPARAM)hCommandFont, TRUE);
    }
}

void ApplyUiVisualTone()
{
    if (hInfoEdit) OutputControl::SetBackgroundTone(hInfoEdit, BackgroundAlpha(84), BackgroundAlpha(22));
}

bool UpdateUiScale(HWND hWnd, UINT dpi)
{
    const RECT workArea = GetMonitorWorkArea(hWnd);
    const double oldScale = gUiScale;
    gUiMonitor = GetNearestMonitor(hWnd);
    gUiScale = ComputeUiScale(dpi, workArea);
    return std::fabs(oldScale - gUiScale) > 0.001;
}

void CreateMainControls(HWND hWnd)
{
    // Children are created hidden so they don't paint at 0,0 before the
    // first LayoutMainControls pass positions them. They get SW_SHOW'd at
    // the end of LayoutMainControls.
    const DWORD buttonStyle = WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW;
    const DWORD labelStyle = WS_CHILD | SS_LEFT;
    CreateUiFonts();
    hDarkEditBrush = CreateOutputEditBrush(ScalePx(960), ScalePx(420));
    if (!hOutputSolidBrush) hOutputSolidBrush = CreateSolidBrush(OutputDark);

    hInfoEdit = OutputControl::Create(hWnd, IDC_INFO_EDIT, hInst);
    if (hInfoEdit)
    {
        OutputControl::SetFont(hInfoEdit, hOutputFont);
        OutputControl::SetBackgroundImage(hInfoEdit, gOutputBackgroundImage);
        ApplyUiVisualTone();
    }

    // Accessible mirror: a standard read-only multiline EDIT that screen
    // readers can read natively. Created hidden; shown only in accessible
    // mode. The label is created immediately before the EDIT so MSAA/UIA can
    // use it as the EDIT's accessible name.
    hAccessibleLabel = CreateWindowExW(
        0, L"STATIC", L"Scan output (read-only)",
        WS_CHILD | SS_LEFT | SS_ENDELLIPSIS,
        0, 0, 0, 0,
        hWnd, (HMENU)IDC_ACCESSIBLE_LABEL, hInst, nullptr);
    if (hAccessibleLabel)
    {
        SendMessageW(hAccessibleLabel, WM_SETFONT, (WPARAM)hOutputFont, TRUE);
    }

    hAccessibleEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | WS_VSCROLL
            | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
        0, 0, 0, 0,
        hWnd, (HMENU)IDC_ACCESSIBLE_EDIT, hInst, nullptr);
    if (hAccessibleEdit)
    {
        SendMessageW(hAccessibleEdit, WM_SETFONT, (WPARAM)hOutputFont, TRUE);
        SendMessageW(hAccessibleEdit, EM_SETLIMITTEXT, 0, 0);  // lift length cap
    }

    hProgressText = CreateWindowExW(
        0,
        L"STATIC",
        L"Idle",
        WS_CHILD | SS_LEFT | SS_ENDELLIPSIS,
        0, 0, 0, 0,
        hWnd,
        (HMENU)IDC_PROGRESS_TEXT,
        hInst,
        nullptr);
    if (hProgressText)
    {
        SendMessageW(hProgressText, WM_SETFONT, (WPARAM)hOutputFont, TRUE);
    }

    hProgressBar = CreateWindowExW(
        0,
        PROGRESS_CLASSW,
        nullptr,
        WS_CHILD | PBS_SMOOTH,
        0, 0, 0, 0,
        hWnd,
        (HMENU)IDC_PROGRESS_BAR,
        hInst,
        nullptr);
    if (hProgressBar)
    {
        SendMessageW(hProgressBar, PBM_SETRANGE32, 0, 1000);
        SendMessageW(hProgressBar, PBM_SETPOS, 0, 0);
        SendMessageW(hProgressBar, PBM_SETBARCOLOR, 0, AccentBlue);
        SendMessageW(hProgressBar, PBM_SETBKCOLOR, 0, OutputDark);
    }

    for (int i = 0; i < SECTION_LABEL_COUNT; ++i)
    {
        hSectionLabels[i] = CreateWindowW(
            L"STATIC",
            SectionLabels[i],
            labelStyle,
            0, 0, 0, 0,
            hWnd,
            (HMENU)(INT_PTR)(IDC_DISC_QUALITY_LABEL + i),
            hInst,
            nullptr);

        if (hSectionLabels[i])
        {
            SendMessageW(hSectionLabels[i], WM_SETFONT, (WPARAM)hHeaderFont, TRUE);
        }
    }

    for (int i = 0; i < COMMAND_BUTTON_COUNT; ++i)
    {
        hInfoButtons[i] = CreateWindowW(
            L"BUTTON",
            CommandLabels[i],
            buttonStyle,
            0, 0, 0, 0,
            hWnd,
            (HMENU)(INT_PTR)(IDC_INFO_BUTTON1 + i),
            hInst,
            nullptr);

        if (hInfoButtons[i])
        {
            SendMessageW(hInfoButtons[i], WM_SETFONT, (WPARAM)hCommandFont, TRUE);
        }
    }

    // Open the waveOut device and warm the audio session now so the very
    // first menu-button click plays in full instead of being eaten by
    // session-startup latency.
    UiSound::Prewarm();

    LayoutMainControls(hWnd);
}

void LayoutMainControls(HWND hWnd)
{
    RECT rc;
    GetClientRect(hWnd, &rc);

    // Batch every child move into one DeferWindowPos transaction so children
    // don't repaint individually as the layout walks across them. Total
    // children: 1 output + 2 progress + 4 section labels + 33 buttons = 40.
    //
    // On the first pass, children were created hidden — fold SWP_SHOWWINDOW
    // into the batch so positioning and reveal happen as one atomic update.
    static bool s_firstLayoutDone = false;
    const bool firstPass = !s_firstLayoutDone;
    const UINT swpFlags = SWP_NOZORDER | SWP_NOACTIVATE
                        | (firstPass ? SWP_SHOWWINDOW : 0);

    HDWP hdwp = BeginDeferWindowPos(44);
    auto move = [&](HWND h, int x, int y, int w, int hgt) {
        if (!h) return;
        if (hdwp) {
            hdwp = DeferWindowPos(hdwp, h, nullptr, x, y, w, hgt, swpFlags);
        } else {
            // BeginDeferWindowPos failed or a prior DeferWindowPos failed
            // (which frees the HDWP). Fall back to per-window move + show.
            MoveWindow(h, x, y, w, hgt, TRUE);
            if (firstPass) ShowWindow(h, SW_SHOWNOACTIVATE);
        }
    };

    const int margin = ScalePx(40);
    const int top = ScalePx(145);
    const int gap = ScalePx(8);
    const int buttonHeight = ScalePx(46);
    const int labelHeight = ScalePx(30);
    const int panelGap = ScalePx(20);
    const int middleColumnHeight =
        (labelHeight + gap) +
        (6 * (buttonHeight + gap)) +
        (labelHeight + gap) +
        (7 * (buttonHeight + gap));
    const int minimumOutputTop = top + middleColumnHeight + ScalePx(32);
    const int requestedOutputHeight = max(ScalePx(420), min(ScalePx(680), ((rc.bottom - rc.top) * 38) / 100));
    const int outputTop = max(minimumOutputTop, rc.bottom - margin - requestedOutputHeight);
    const int outputHeight = max(ScalePx(300), rc.bottom - margin - outputTop);
    const int outputWidth = max(ScalePx(400), rc.right - rc.left - (margin * 2));
    const int panelLeft = margin;
    const int panelWidth = max(ScalePx(900), rc.right - rc.left - (margin * 2));
    const int columnWidth = max(ScalePx(360), (panelWidth - (panelGap * 2)) / 3);
    const int middleColumnLeft = panelLeft + columnWidth + panelGap;
    const int rightColumnLeft = middleColumnLeft + columnWidth + panelGap;

    if (hInfoEdit)
    {
        const int outputInset = ScalePx(22);
        const int consoleHeaderHeight = ScalePx(40);
        const int progressTop = outputTop + consoleHeaderHeight + ScalePx(8);
        const int progressHeight = ScalePx(28);
        const int progressGap = ScalePx(8);
        const int editWidth = max(ScalePx(200), outputWidth - (outputInset * 2));
        const int progressTextWidth = max(ScalePx(180), (editWidth * 58) / 100);
        const int progressBarWidth = max(ScalePx(160), editWidth - progressTextWidth - ScalePx(14));
        const int editTop = progressTop + progressHeight + progressGap;
        const int editHeight = max(ScalePx(120), outputTop + outputHeight - editTop - ScalePx(5));

        move(hProgressText,
             margin + outputInset, progressTop,
             progressTextWidth, progressHeight);

        move(hProgressBar,
             margin + outputInset + progressTextWidth + ScalePx(14), progressTop + ScalePx(4),
             progressBarWidth, max(ScalePx(18), progressHeight - ScalePx(8)));

        UpdateOutputEditBrush(editWidth, editHeight);
        move(hInfoEdit,
             margin + outputInset, editTop,
             editWidth, editHeight);

        // Accessible mirror occupies the same rectangle as the OutputControl,
        // with a heading label up in the console-header band above the
        // progress strip. Only one of {OutputControl, mirror} is shown at a
        // time (see ApplyAccessibleVisibility).
        move(hAccessibleLabel,
             margin + outputInset, outputTop + ScalePx(6),
             editWidth, consoleHeaderHeight - ScalePx(8));
        move(hAccessibleEdit,
             margin + outputInset, editTop,
             editWidth, editHeight);
    }

    int leftY = top;
    int middleY = top;
    int rightY = top;

    // Left column — Ripping (buttons 0..4, incl. Recovery rip) then DISC QUALITY (5..11)
    for (int i = 0; i < 5; ++i)
    {
        move(hInfoButtons[i], panelLeft, leftY, columnWidth, buttonHeight);
        leftY += buttonHeight + gap;
    }

    move(hSectionLabels[0], panelLeft, leftY, columnWidth, labelHeight);
    leftY += labelHeight + gap;

    for (int i = 5; i < 12; ++i)
    {
        move(hInfoButtons[i], panelLeft, leftY, columnWidth, buttonHeight);
        leftY += buttonHeight + gap;
    }

    move(hSectionLabels[1], middleColumnLeft, middleY, columnWidth, labelHeight);
    middleY += labelHeight + gap;

    for (int i = 12; i < 18; ++i)
    {
        move(hInfoButtons[i], middleColumnLeft, middleY, columnWidth, buttonHeight);
        middleY += buttonHeight + gap;
    }

    move(hSectionLabels[2], middleColumnLeft, middleY, columnWidth, labelHeight);
    middleY += labelHeight + gap;

    for (int i = 18; i < 25; ++i)
    {
        move(hInfoButtons[i], middleColumnLeft, middleY, columnWidth, buttonHeight);
        middleY += buttonHeight + gap;
    }

    move(hSectionLabels[3], rightColumnLeft, rightY, columnWidth, labelHeight);
    rightY += labelHeight + gap;

    for (int i = 25; i < COMMAND_BUTTON_COUNT; ++i)
    {
        move(hInfoButtons[i], rightColumnLeft, rightY, columnWidth, buttonHeight);
        rightY += buttonHeight + gap;
    }

    if (hdwp) EndDeferWindowPos(hdwp);

    // The first pass reveals every child via SWP_SHOWWINDOW; re-hide whichever
    // output surface isn't active for the current accessibility mode.
    ApplyAccessibleVisibility();

    s_firstLayoutDone = true;
}

// Show the readable EDIT mirror (and hide the custom OutputControl) when
// accessible mode is on, or vice versa. The progress strip and command
// buttons are unaffected and remain visible in both modes.
void ApplyAccessibleVisibility()
{
    if (hInfoEdit)
        ShowWindow(hInfoEdit, g_accessibleMode ? SW_HIDE : SW_SHOWNOACTIVATE);
    if (hAccessibleEdit)
        ShowWindow(hAccessibleEdit, g_accessibleMode ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (hAccessibleLabel)
        ShowWindow(hAccessibleLabel, g_accessibleMode ? SW_SHOWNOACTIVATE : SW_HIDE);
}

// Switch accessibility mode on/off: sync the View menu check, swap which
// output surface is visible, repaint, and (optionally) move keyboard focus to
// the readable EDIT so the screen reader starts reading there.
void ApplyAccessibleMode(HWND hWnd, bool enabled, bool focusEdit)
{
    const bool modeChanged = (g_accessibleMode != enabled);

    g_accessibleMode = enabled;
    Accessibility::SetEnabled(enabled);  // visible to scan worker threads

    if (HMENU menu = GetMenu(hWnd))
    {
        CheckMenuItem(menu, IDM_TOGGLE_ACCESSIBLE,
                      MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
    }

    ApplyAccessibleVisibility();
    if (modeChanged && IsWindowVisible(hWnd))
    {
        InvalidateRect(hWnd, nullptr, TRUE);
    }

    if (enabled && focusEdit && hAccessibleEdit)
    {
        SetFocus(hAccessibleEdit);
    }
}

