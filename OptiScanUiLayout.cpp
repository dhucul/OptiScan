// OptiScanUiLayout.cpp - control creation, layout, fonts, and accessibility visibility.

#include "framework.h"
#include "OptiScanUiInternal.h"

#include "Accessibility.h"
#include "FontLoader.h"
#include "OutputControl.h"
#include "Theme.h"
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
    if (hOutputFont) DeleteObject(hOutputFont);

    // Inter SemiBold (a bundled San-Francisco-style face) for the command
    // buttons. UiSemiBoldFamily() names the exact semibold family so GDI picks
    // the true 600-weight face rather than faux-bolding Regular; it falls back
    // to "Segoe UI Semibold" if the embedded font ever fails to load.
    hCommandFont = CreateFontW(-ScalePx(24), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_SWISS, UiSemiBoldFamily());
    // JetBrains Mono Medium (bundled, SF-Mono-style) for the output panel. It
    // has broad Unicode coverage (General Punctuation, box drawing, block
    // elements, geometric shapes, arrows, symbols) so most of what the
    // workflows draw renders without needing the fallback chain. Medium ships
    // as its own family, so MonoMediumFamily() names it exactly; it falls back
    // to Cascadia Mono if the embedded font ever fails to load.
    hOutputFont = CreateFontW(-ScalePx(24), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        FIXED_PITCH | FF_MODERN, MonoMediumFamily());
}

void ApplyUiFonts()
{
    if (hInfoEdit) OutputControl::SetFont(hInfoEdit, hOutputFont);
    if (hProgressText) SendMessageW(hProgressText, WM_SETFONT, (WPARAM)hOutputFont, TRUE);
    if (hAccessibleEdit) SendMessageW(hAccessibleEdit, WM_SETFONT, (WPARAM)hOutputFont, TRUE);
    if (hAccessibleLabel) SendMessageW(hAccessibleLabel, WM_SETFONT, (WPARAM)hOutputFont, TRUE);

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
    // children: 1 output + 2 progress + 2 accessible + 35 buttons = 40.
    //
    // On the first pass, children were created hidden — fold SWP_SHOWWINDOW
    // into the batch so positioning and reveal happen as one atomic update.
    const UINT swpFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW;
    bool buttonVisible[COMMAND_BUTTON_COUNT]{};
    HWND priorFocus = GetFocus();

    HDWP hdwp = BeginDeferWindowPos(41);
    auto move = [&](HWND h, int x, int y, int w, int hgt) {
        if (!h) return;
        for (int i = 0; i < COMMAND_BUTTON_COUNT; ++i) {
            if (hInfoButtons[i] == h) { buttonVisible[i] = true; break; }
        }
        if (hdwp) {
            hdwp = DeferWindowPos(hdwp, h, nullptr, x, y, w, hgt, swpFlags);
        } else {
            // BeginDeferWindowPos failed or a prior DeferWindowPos failed
            // (which frees the HDWP). Fall back to per-window move + show.
            MoveWindow(h, x, y, w, hgt, TRUE);
            ShowWindow(h, SW_SHOWNOACTIVATE);
        }
    };

    // One layout for every theme: a navigation rail, six pages, three primary
    // actions on the overview, compact grouped commands, and a full-width
    // console. The rail geometry comes from SidebarWidth()/NavItemTop() so the
    // painter and the hit-test cannot drift from it.
    {
        const int sidebarWidth = SidebarWidth();
        const int contentLeft = sidebarWidth + ScalePx(60);
        const int contentRight = rc.right - ScalePx(40);
        const int contentWidth = max(ScalePx(900), contentRight - contentLeft);
        const int navIndex = GetNavIndex();
        int outputTop = rc.bottom - ScalePx(650);
        if (navIndex == 0)
        {
            const int columnGap = ScalePx(18);
            const int overviewColumns = 6;
            const int columnWidth = max(ScalePx(220),
                (contentWidth - (columnGap * (overviewColumns - 1))) / overviewColumns);
            const int primaryTop = ScalePx(150);
            const int primaryHeight = ScalePx(170);
            const int primaryGap = ScalePx(24);
            const int primaryWidth = max(ScalePx(300), (contentWidth - (primaryGap * 2)) / 3);
            const int primary[] = { 0, 1, 6 };
            for (int i = 0; i < 3; ++i)
            {
                move(hInfoButtons[primary[i]], contentLeft + i * (primaryWidth + primaryGap),
                     primaryTop, primaryWidth, primaryHeight);
            }

            const int buttonHeight = ScalePx(56);
            const int rowGap = ScalePx(10);
            auto placeGroup = [&](const int* indices, int count, int buttonTop) {
                for (int i = 0; i < count; ++i)
                {
                    const int row = i / overviewColumns;
                    const int column = i % overviewColumns;
                    move(hInfoButtons[indices[i]],
                         contentLeft + column * (columnWidth + columnGap),
                         buttonTop + row * (buttonHeight + rowGap),
                         columnWidth, buttonHeight);
                }
                return buttonTop + ((count + overviewColumns - 1) / overviewColumns)
                    * (buttonHeight + rowGap) - rowGap;
            };

            const int ripCopy[] = { 2, 3, 4, 5 };
            const int discQuality[] = { 7, 8, 9, 10, 11, 12 };
            const int analysis[] = { 13, 14, 15, 16, 17, 18 };
            const int driveTools[] = { 19, 20, 21, 22, 23, 24, 25, 29, 30, 31, 32 };
            const int utilities[] = { 26, 27, 28, 33, 34, 35 };

            int groupTitleTop = ScalePx(370);
            int groupBottom = placeGroup(ripCopy, ARRAYSIZE(ripCopy), groupTitleTop + ScalePx(40));
            groupTitleTop = groupBottom + ScalePx(36);
            groupBottom = placeGroup(discQuality, ARRAYSIZE(discQuality), groupTitleTop + ScalePx(40));
            groupTitleTop = groupBottom + ScalePx(36);
            groupBottom = placeGroup(analysis, ARRAYSIZE(analysis), groupTitleTop + ScalePx(40));
            groupTitleTop = groupBottom + ScalePx(36);
            groupBottom = placeGroup(driveTools, ARRAYSIZE(driveTools), groupTitleTop + ScalePx(40));
            groupTitleTop = groupBottom + ScalePx(36);
            groupBottom = placeGroup(utilities, ARRAYSIZE(utilities), groupTitleTop + ScalePx(40));
            outputTop = max(groupBottom + ScalePx(60), outputTop);
        }
        else
        {
            const int ripCopy[] = { 0, 1, 2, 3, 4, 5 };
            const int discQuality[] = { 6, 7, 8, 9, 10, 11, 12 };
            const int analysis[] = { 13, 14, 15, 16, 17, 18 };
            const int driveTools[] = { 19, 20, 21, 22, 23, 24, 25, 29, 30, 31, 32 };
            const int utilities[] = { 26, 27, 28, 33, 34, 35 };
            const int* indices = ripCopy;
            int count = ARRAYSIZE(ripCopy);
            if (navIndex == 2) { indices = discQuality; count = ARRAYSIZE(discQuality); }
            else if (navIndex == 3) { indices = analysis; count = ARRAYSIZE(analysis); }
            else if (navIndex == 4) { indices = driveTools; count = ARRAYSIZE(driveTools); }
            else if (navIndex == 5) { indices = utilities; count = ARRAYSIZE(utilities); }

            const int columns = 3;
            const int gap = ScalePx(18);
            const int buttonWidth = max(ScalePx(300), (contentWidth - gap * (columns - 1)) / columns);
            const int buttonHeight = ScalePx(64);
            const int buttonTop = ScalePx(170);
            for (int i = 0; i < count; ++i)
            {
                move(hInfoButtons[indices[i]],
                     contentLeft + (i % columns) * (buttonWidth + gap),
                     buttonTop + (i / columns) * (buttonHeight + ScalePx(14)),
                     buttonWidth, buttonHeight);
            }
        }

        const int outputBottom = max(ScalePx(160), rc.bottom - ScalePx(40));
        outputTop = min(outputTop, max(ScalePx(100), outputBottom - ScalePx(180)));
        const int outputHeight = max(ScalePx(140), outputBottom - outputTop);
        const int outputInset = ScalePx(22);
        const int progressTop = outputTop + ScalePx(54);
        const int progressHeight = ScalePx(30);
        const int editTop = progressTop + progressHeight + ScalePx(10);
        const int editWidth = max(ScalePx(300), contentWidth - outputInset * 2);
        const int editHeight = max(ScalePx(140), outputTop + outputHeight - editTop - ScalePx(18));
        const int progressTextWidth = max(ScalePx(220), (editWidth * 62) / 100);

        move(hProgressText, contentLeft + outputInset, progressTop,
             progressTextWidth, progressHeight);
        move(hProgressBar, contentLeft + outputInset + progressTextWidth + ScalePx(16),
             progressTop + ScalePx(5), editWidth - progressTextWidth - ScalePx(16), ScalePx(20));
        UpdateOutputEditBrush(editWidth, editHeight);
        move(hInfoEdit, contentLeft + outputInset, editTop, editWidth, editHeight);
        move(hAccessibleLabel, contentLeft + outputInset, outputTop + ScalePx(12), editWidth, ScalePx(32));
        move(hAccessibleEdit, contentLeft + outputInset, editTop, editWidth, editHeight);

        if (hdwp) {
            for (int i = 0; i < COMMAND_BUTTON_COUNT; ++i) {
                if (!buttonVisible[i] && hInfoButtons[i]) {
                    hdwp = DeferWindowPos(hdwp, hInfoButtons[i], nullptr, 0, 0, 0, 0,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
                    if (!hdwp) break;
                }
            }
        }
        const bool committed = hdwp && EndDeferWindowPos(hdwp);
        if (!committed) {
            for (int i = 0; i < COMMAND_BUTTON_COUNT; ++i) {
                if (hInfoButtons[i]) ShowWindow(hInfoButtons[i],
                    buttonVisible[i] ? SW_SHOWNOACTIVATE : SW_HIDE);
            }
        }
        for (int i = 0; i < COMMAND_BUTTON_COUNT; ++i) {
            if (hInfoButtons[i] == priorFocus && buttonVisible[i]) {
                SetFocus(priorFocus);
                break;
            }
        }
        ApplyAccessibleVisibility();
    }
}

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

