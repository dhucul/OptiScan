// OptiScanUiPaint.cpp - UI images, brushes, and custom painting.

#include "framework.h"
#include "OptiScanUiInternal.h"
#include "FontLoader.h"
#include "Theme.h"

#include <cmath>
#include <cstring>
#include <cwchar>

// Progress.h (pulled in elsewhere) does `#undef min` / `#undef max`, which
// would otherwise break the legacy `min(a, b)` / `max(a, b)` calls in the GUI
// layout / paint code. Restore the Windows-style macros for this module too.
#undef min
#undef max
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) < (b)) ? (b) : (a))

static void AddRoundedRectangle(Gdiplus::GraphicsPath& path, int x, int y, int width, int height, int radius);
static void SplitCommandLabel(LPCWSTR source, WCHAR* number, int numberLength, LPCWSTR* label);
static void DrawUnifiedBackground(Gdiplus::Graphics& graphics, const RECT& rc);
static void DrawOpticalRingMark(Gdiplus::Graphics& graphics, Gdiplus::REAL cx, Gdiplus::REAL cy,
                                Gdiplus::REAL radius, BYTE alpha, bool includeBlueArc);

// Build a GDI+ ARGB colour from a theme COLORREF (0x00BBGGRR) plus an alpha.
// The chrome is drawn semi-transparent over the artwork, so alpha stays at the
// paint site while the RGB comes from the active palette.
static inline Gdiplus::Color ThemeArgb(BYTE a, COLORREF c)
{
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

// Soft radial glow via a path gradient (accent colour fading to transparent).
static void PaintGlow(Gdiplus::Graphics& g, float cx, float cy, float r, BYTE alpha, COLORREF col)
{
    if (r < 1.0f) return;
    Gdiplus::GraphicsPath path;
    path.AddEllipse(cx - r, cy - r, r * 2, r * 2);
    Gdiplus::PathGradientBrush pgb(&path);
    pgb.SetCenterColor(ThemeArgb(alpha, col));
    Gdiplus::Color surround[] = { ThemeArgb(0, col) };
    int cnt = 1;
    pgb.SetSurroundColors(surround, &cnt);
    g.FillEllipse(&pgb, cx - r, cy - r, r * 2, r * 2);
}

// Paint the instrumentation canvas procedurally from the active palette into a
// width x height area: a diagonal gradient base, a vertical depth pass, a radial
// scan glow with rays, a sparse measurement dot matrix, concentric scan arcs,
// and a horizon line. Every colour is a backdrop* palette role, so each theme
// renders the same composition in its own family.
//
// Rendered once into a cached bitmap (EnsureBackdropBitmap) and blitted with a
// per-button offset by DrawBackgroundSurface, so the slice under each
// owner-drawn rounded button lines up with the parent window's copy.
static void PaintProceduralBackdrop(Gdiplus::Graphics& g, int width, int height)
{
    const Palette& p = ActiveTheme();
    const float W = (float)width, H = (float)height;
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    Gdiplus::Rect full(0, 0, width, height);
    {
        Gdiplus::LinearGradientBrush canvas(full,
            ThemeArgb(255, p.backdropTop), ThemeArgb(255, p.backdropBottom), 12.0f);
        g.FillRectangle(&canvas, full);

        // Deepen the lower canvas without flattening the tint in the upper-left.
        Gdiplus::LinearGradientBrush depth(full,
            ThemeArgb(0, p.backdropDepth), ThemeArgb(58, p.backdropDepth),
            Gdiplus::LinearGradientModeVertical);
        g.FillRectangle(&depth, full);

        const float scanX = W * 1.015f;
        const float scanY = H * 0.40f;
        PaintGlow(g, scanX, scanY, min(W, H) * 0.34f, 92, p.backdropGlow);

        // Sparse measurement matrix, concentrated on the right half.
        Gdiplus::SolidBrush dot(ThemeArgb(30, p.backdropInstrument));
        const int dotStep = max(ScalePx(24), 12);
        for (int y = ScalePx(18); y < (int)(H * 0.82f); y += dotStep)
            for (int x = (int)(W * 0.40f); x < width; x += dotStep)
                g.FillEllipse(&dot, (Gdiplus::REAL)x, (Gdiplus::REAL)y, ScaleReal(2), ScaleReal(2));

        // Radial scan geometry anchored just beyond the right edge.
        const float maxRadius = min(W, H) * 0.62f;
        for (int i = 1; i <= 11; ++i)
        {
            const float r = maxRadius * ((float)i / 11.0f);
            Gdiplus::Pen arc(ThemeArgb((BYTE)(30 + (i % 3) * 12), p.backdropInstrument),
                              max(1.0f, ScaleReal(i % 4 == 0 ? 2 : 1)));
            g.DrawEllipse(&arc, scanX - r, scanY - r, r * 2.0f, r * 2.0f);
        }
        Gdiplus::Pen ray(ThemeArgb(38, p.backdropGlow), max(1.0f, ScaleReal(1)));
        for (int degrees = 150; degrees <= 210; degrees += 5)
        {
            const float radians = degrees * 3.14159265f / 180.0f;
            g.DrawLine(&ray, scanX, scanY,
                       scanX + std::cos(radians) * maxRadius,
                       scanY + std::sin(radians) * maxRadius);
        }

        // Horizon glow below the primary action cards.
        Gdiplus::Pen horizon(ThemeArgb(105, p.backdropInstrument), max(1.0f, ScaleReal(2)));
        g.DrawLine(&horizon, W * 0.10f, H * 0.185f, W * 0.88f, H * 0.185f);
    }
}

// Cached backdrop bitmap. Regenerated when the size or theme changes; blitted
// (with per-button offset) by DrawBackgroundSurface so the procedural art is
// painted once per frame instead of once per owner-drawn button.
static Gdiplus::Bitmap* gBackdropBitmap = nullptr;
static int gBackdropW = 0;
static int gBackdropH = 0;
static int gBackdropThemeId = -1;
static double gBackdropScale = 0.0;

static void EnsureBackdropBitmap(int width, int height)
{
    width = max(1, width);
    height = max(1, height);
    const int tid = (int)CurrentThemeId();
    // Keyed on gUiScale as well as size: the dot spacing and stroke widths come
    // from ScalePx/ScaleReal, so a DPI change that happens to leave the client
    // size unchanged (same-resolution monitor, different DPI) would otherwise
    // reuse a bitmap drawn at the old scale.
    if (gBackdropBitmap && gBackdropW == width && gBackdropH == height
        && gBackdropThemeId == tid && gBackdropScale == gUiScale)
    {
        return;
    }
    delete gBackdropBitmap;
    gBackdropBitmap = new Gdiplus::Bitmap(width, height, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(gBackdropBitmap);
    PaintProceduralBackdrop(g, width, height);
    gBackdropW = width;
    gBackdropH = height;
    gBackdropThemeId = tid;
    gBackdropScale = gUiScale;
}


void InitializeUiResources()
{
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gGdiPlusToken, &gdiplusStartupInput, nullptr);
}

void DestroyUiResources()
{
    if (hCommandFont)
    {
        DeleteObject(hCommandFont);
        hCommandFont = nullptr;
    }

    if (hOutputFont)
    {
        DeleteObject(hOutputFont);
        hOutputFont = nullptr;
    }

    if (hDarkEditBrush)
    {
        DeleteObject(hDarkEditBrush);
        hDarkEditBrush = nullptr;
    }

    if (hOutputSolidBrush)
    {
        DeleteObject(hOutputSolidBrush);
        hOutputSolidBrush = nullptr;
    }

    if (hOutputBrushBitmap)
    {
        DeleteObject(hOutputBrushBitmap);
        hOutputBrushBitmap = nullptr;
    }

    delete gBackdropBitmap;
    gBackdropBitmap = nullptr;

    // Release the bundled private fonts (GDI mem-font handles + the GDI+
    // PrivateFontCollection) before GDI+ itself shuts down.
    UnloadBundledFonts();

    Gdiplus::GdiplusShutdown(gGdiPlusToken);
}

HBRUSH CreateOutputEditBrush(int /*width*/, int /*height*/)
{
    // The accessible mirror EDIT sits on a plain themed dark surface (its
    // content is text; the procedural artwork lives on the main window).
    return CreateSolidBrush(OutputDark);
}

void UpdateOutputEditBrush(int width, int height)
{
    width = max(1, width);
    height = max(1, height);

    if (hDarkEditBrush && gOutputBrushWidth == width && gOutputBrushHeight == height)
    {
        return;
    }

    if (hDarkEditBrush)
    {
        DeleteObject(hDarkEditBrush);
        hDarkEditBrush = nullptr;
    }

    if (hOutputBrushBitmap)
    {
        DeleteObject(hOutputBrushBitmap);
        hOutputBrushBitmap = nullptr;
    }

    hDarkEditBrush = CreateOutputEditBrush(width, height);
    gOutputBrushWidth = width;
    gOutputBrushHeight = height;

    if (hInfoEdit)
    {
        InvalidateRect(hInfoEdit, nullptr, TRUE);
    }
}

// PaintOutputImageBackground, the cached-paint state, PaintOutputEdit, and
// OutputEditProc previously lived here. The output box is now an
// OutputControl that paints itself, so all of that custom paint plumbing is
// gone.

static void DrawBackgroundSurface(Gdiplus::Graphics& graphics, const RECT& viewport, int offsetX, int offsetY, bool /*showFailureText*/)
{
    const int width = max(1, viewport.right - viewport.left);
    const int height = max(1, viewport.bottom - viewport.top);

    EnsureBackdropBitmap(width, height);
    if (gBackdropBitmap)
    {
        // Blit the full backdrop at the (negative) offset so owner-drawn
        // buttons get the slice that lines up under them.
        graphics.DrawImage(gBackdropBitmap, -offsetX, -offsetY,
                           0, 0, width, height, Gdiplus::UnitPixel);
    }
    else
    {
        Gdiplus::SolidBrush baseBrush(ThemeArgb(255, ActiveTheme().backdropTop));
        graphics.FillRectangle(&baseBrush, -offsetX, -offsetY, width, height);
    }
}

static void DrawUnifiedBackground(Gdiplus::Graphics& graphics, const RECT& rc)
{
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int sidebarWidth = SidebarWidth();
    const int contentLeft = sidebarWidth + ScalePx(60);
    const int contentRight = rc.right - ScalePx(40);
    const int contentWidth = max(ScalePx(900), contentRight - contentLeft);
    const int selectedNav = GetNavIndex();

    DrawBackgroundSurface(graphics, rc, 0, 0, true);

    const Palette& p = ActiveTheme();

    // The rail sits directly on the instrumentation canvas, so it uses the
    // raised surface rather than a stark paper-white.
    Gdiplus::SolidBrush sidebar(ThemeArgb(255, p.surfaceRaised));
    graphics.FillRectangle(&sidebar, 0, 0, sidebarWidth, height);
    Gdiplus::Pen divider(ThemeArgb(255, p.hairline), max(1.0f, ScaleReal(1)));
    graphics.DrawLine(&divider, sidebarWidth, 0, sidebarWidth, height);

    // Small, fixed brand lockup in the navigation rail.
    DrawOpticalRingMark(graphics, ScaleReal(55), ScaleReal(62), ScaleReal(22), 220, true);
    Gdiplus::FontFamily uiFamily(UiFamily(), UiFontCollection());
    Gdiplus::FontFamily navFamily(UiFamily(), UiFontCollection());
    Gdiplus::Font brand(&uiFamily, ScaleReal(30), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font navFont(&navFamily, ScaleReal(23), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font navSelected(&navFamily, ScaleReal(23), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font pageTitle(&uiFamily, ScaleReal(38), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font subtitle(&uiFamily, ScaleReal(21), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::FontFamily sidebarDetailFamily(UiFamily(), UiFontCollection());
    Gdiplus::Font sidebarDetail(&sidebarDetailFamily, ScaleReal(20), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font sectionFont(&uiFamily, ScaleReal(24), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush ink(ThemeArgb(255, p.cardInk));
    // Secondary ink keeps unselected navigation and drive-status text readable
    // on the rail without competing with selected items.
    Gdiplus::SolidBrush muted(ThemeArgb(255, p.cardInkMuted));
    Gdiplus::SolidBrush mainInk(ThemeArgb(255, p.canvasInk));
    Gdiplus::SolidBrush mainMuted(ThemeArgb(255, p.canvasInkMuted));
    Gdiplus::SolidBrush blue(ThemeArgb(255, p.accentPrimary));
    graphics.DrawString(L"OptiScan", -1, &brand, Gdiplus::PointF(ScaleReal(90), ScaleReal(42)), &ink);

    const wchar_t* navLabels[] = { L"Overview", L"Rip & Copy", L"Disc Quality", L"Analysis", L"Drive Tools", L"Utilities" };
    static_assert(ARRAYSIZE(navLabels) == kNavItemCount, "nav labels must match the hit-test's item count");
    for (int i = 0; i < kNavItemCount; ++i)
    {
        const int y = NavItemTop(i);
        if (i == selectedNav)
        {
            Gdiplus::GraphicsPath selectedPath;
            AddRoundedRectangle(selectedPath, ScalePx(18), y, sidebarWidth - ScalePx(36), ScalePx(kNavItemHeight), ScalePx(10));
            Gdiplus::SolidBrush selectedBg(ThemeArgb(255, p.surfaceSunken));
            graphics.FillPath(&selectedBg, &selectedPath);
            graphics.FillRectangle(&blue, ScalePx(18), y + ScalePx(10), ScalePx(5), ScalePx(38));
        }
        Gdiplus::Pen iconPen(i == selectedNav ? ThemeArgb(255, p.accentPrimary)
                                    : ThemeArgb(255, p.cardInkMuted), max(1.0f, ScaleReal(2)));
        graphics.DrawEllipse(&iconPen, ScaleReal(43), (Gdiplus::REAL)y + ScaleReal(17), ScaleReal(24), ScaleReal(24));
        graphics.DrawString(navLabels[i], -1, i == selectedNav ? &navSelected : &navFont,
                            Gdiplus::PointF(ScaleReal(88), (Gdiplus::REAL)y + ScaleReal(14)),
                            i == selectedNav ? &blue : &muted);
    }

    // Drive status remains visible without competing with the command area.
    Gdiplus::Pen sidebarRule(ThemeArgb(255, p.hairline), 1.0f);
    graphics.DrawLine(&sidebarRule, ScalePx(28), height - ScalePx(215), sidebarWidth - ScalePx(28), height - ScalePx(215));
    Gdiplus::SolidBrush ready(ThemeArgb(255, p.ok));
    graphics.FillEllipse(&ready, ScaleReal(34), (Gdiplus::REAL)(height - ScalePx(172)), ScaleReal(10), ScaleReal(10));
    graphics.DrawString(L"Drive ready", -1, &navSelected,
                        Gdiplus::PointF(ScaleReal(58), (Gdiplus::REAL)(height - ScalePx(187))), &ink);
    graphics.DrawString(L"Optical drive connected", -1, &sidebarDetail,
                        Gdiplus::PointF(ScaleReal(34), (Gdiplus::REAL)(height - ScalePx(142))), &muted);

    const wchar_t* pageTitles[] = { L"Overview", L"Rip & Copy", L"Disc Quality", L"Analysis", L"Drive Tools", L"Utilities" };
    const wchar_t* pageSubtitles[] = {
        L"Optical media toolkit for copying, ripping, scanning, and analysis.",
        L"Create, extract, write, and recover optical media.",
        L"Measure disc readability, errors, degradation, and surface quality.",
        L"Inspect audio content, fingerprints, lead areas, and subchannels.",
        L"Identify, calibrate, validate, and benchmark optical drives.",
        L"Rescan media, run batches, get help, and manage OptiScan."
    };
    graphics.DrawString(pageTitles[selectedNav], -1, &pageTitle,
                        Gdiplus::PointF((Gdiplus::REAL)contentLeft, ScaleReal(48)), &mainInk);
    graphics.DrawString(pageSubtitles[selectedNav], -1, &subtitle,
                        Gdiplus::PointF((Gdiplus::REAL)contentLeft, ScaleReal(98)), &mainMuted);

    int groupBottom = 0;
    if (selectedNav == 0)
    {
        const wchar_t* groupNames[] = { L"Rip & Copy", L"Disc Quality", L"Analysis", L"Drive Tools", L"Utilities" };
        const int groupCounts[] = { 3, 6, 6, 11, 6 };
        const int overviewColumns = 6;
        int groupTitleTop = ScalePx(370);
        const int buttonHeight = ScalePx(56);
        const int rowGap = ScalePx(10);
        for (int i = 0; i < ARRAYSIZE(groupNames); ++i)
        {
            graphics.DrawString(groupNames[i], -1, &sectionFont,
                                Gdiplus::PointF((Gdiplus::REAL)contentLeft, (Gdiplus::REAL)groupTitleTop), &mainInk);
            const int buttonTop = groupTitleTop + ScalePx(40);
            groupBottom = buttonTop + ((groupCounts[i] + overviewColumns - 1) / overviewColumns)
                * (buttonHeight + rowGap) - rowGap;
            groupTitleTop = groupBottom + ScalePx(36);
        }
    }

    const int outputTop = selectedNav == 0
        ? max(groupBottom + ScalePx(60), height - ScalePx(650))
        : height - ScalePx(650);
    const int outputBottom = height - ScalePx(40);
    Gdiplus::GraphicsPath outputCard;
    AddRoundedRectangle(outputCard, contentLeft, outputTop, contentWidth,
                        max(ScalePx(260), outputBottom - outputTop), ScalePx(16));
    Gdiplus::SolidBrush outputBg(ThemeArgb(255, p.outputBg));
    graphics.FillPath(&outputBg, &outputCard);
    Gdiplus::Font outputTitle(&uiFamily, ScaleReal(22), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    // Accessible mode puts a real STATIC ("Scan output (read-only)") in this
    // spot to name the EDIT for screen readers, so the decorative title would
    // just overprint it.
    if (!IsAccessibleMode())
    {
        Gdiplus::SolidBrush outputInk(ThemeArgb(255, p.bright));
        graphics.DrawString(L"Output", -1, &outputTitle,
                            Gdiplus::PointF((Gdiplus::REAL)(contentLeft + ScalePx(22)), (Gdiplus::REAL)(outputTop + ScalePx(16))),
                            &outputInk);
    }
}

void DrawMainBackground(HWND hWnd, HDC hdc)
{
    RECT rc;
    GetClientRect(hWnd, &rc);

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    DrawUnifiedBackground(graphics, rc);
}

void DrawCommandButton(const DRAWITEMSTRUCT* drawItem)
{
    if (!drawItem || drawItem->CtlType != ODT_BUTTON ||
        drawItem->CtlID < IDC_INFO_BUTTON1 ||
        drawItem->CtlID > IDC_INFO_BUTTON1 + COMMAND_BUTTON_COUNT - 1)
    {
        return;
    }

    HDC hdc = drawItem->hDC;
    RECT rc = drawItem->rcItem;
    const bool pressed = (drawItem->itemState & ODS_SELECTED) != 0;
    const bool focused = (drawItem->itemState & ODS_FOCUS) != 0;
    const bool disabled = (drawItem->itemState & ODS_DISABLED) != 0;
    const int commandIndex = drawItem->CtlID - IDC_INFO_BUTTON1;
    const bool exitCommand = commandIndex == kExitButtonIndex;

    const Palette& p = ActiveTheme();

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    HWND parent = GetParent(drawItem->hwndItem);
    if (parent)
    {
        RECT parentClient;
        RECT buttonOnParent;
        GetClientRect(parent, &parentClient);
        GetWindowRect(drawItem->hwndItem, &buttonOnParent);
        MapWindowPoints(nullptr, parent, (LPPOINT)&buttonOnParent, 2);
        DrawBackgroundSurface(graphics, parentClient, buttonOnParent.left, buttonOnParent.top, false);
    }

        const bool primary = GetNavIndex() == 0 &&
                             (commandIndex == 0 || commandIndex == 1 || commandIndex == 5);
        WCHAR number[8];
        LPCWSTR rawLabel = CommandLabels[commandIndex];
        SplitCommandLabel(CommandLabels[commandIndex], number, ARRAYSIZE(number), &rawLabel);
        WCHAR label[160];
        wcsncpy_s(label, rawLabel, _TRUNCATE);
        size_t labelLength = wcslen(label);
        while (labelLength > 0 && (label[labelLength - 1] == L'*' || label[labelLength - 1] == L' '))
            label[--labelLength] = L'\0';
        LPCWSTR displayLabel = label;
        // Keep the complete wording in CommandLabels/Operations menus, but use
        // concise card captions where the full diagnostic name is too wide.
        switch (commandIndex)
        {
        case 2:  displayLabel = L"Write disc image files"; break;
        case 3:  displayLabel = L"Write tracks with current pregaps"; break;
        case 4:  displayLabel = L"Recovery rip"; break;
	case 5:  displayLabel = L"Quality scan (hardware errors)"; break;
        case 11: displayLabel = L"Compare original and copy CRCs"; break;
        case 13: displayLabel = L"Disc fingerprint IDs"; break;
        case 16: displayLabel = L"Verify subchannel burn"; break;
        case 28: displayLabel = L"Pioneer audio quality check"; break;
        case 31: displayLabel = L"FE/TE servo scan (LiteOn)"; break;
        case 32: displayLabel = L"Batch run"; break;
        case 33: displayLabel = L"Clear output"; break;
        default: break;
        }

        Gdiplus::GraphicsPath card;
        AddRoundedRectangle(card, rc.left + ScalePx(1), rc.top + ScalePx(1),
                            rc.right - rc.left - ScalePx(2), rc.bottom - rc.top - ScalePx(2),
                            primary ? ScalePx(14) : ScalePx(8));
        if (!pressed)
        {
            Gdiplus::GraphicsPath shadow;
            AddRoundedRectangle(shadow, rc.left + ScalePx(2), rc.top + ScalePx(4),
                                rc.right - rc.left - ScalePx(4), rc.bottom - rc.top - ScalePx(3),
                                primary ? ScalePx(14) : ScalePx(8));
            Gdiplus::SolidBrush shadowBrush(ThemeArgb(primary ? 18 : 10, p.shadowInk));
            graphics.FillPath(&shadowBrush, &shadow);
        }

        // The card face sits on the instrumentation canvas; the exit card takes
        // the danger surface so it reads as destructive at a glance.
        const Gdiplus::Color face = exitCommand
            ? ThemeArgb(255, p.dangerSurface)
            : (pressed ? ThemeArgb(255, p.surfaceSunken) : ThemeArgb(255, p.surfaceRaised));
        Gdiplus::SolidBrush faceBrush(face);
        graphics.FillPath(&faceBrush, &card);
        Gdiplus::Pen cardBorder(
            focused ? ThemeArgb(255, p.accentPrimary)
                    : (exitCommand ? ThemeArgb(255, p.dangerBorder)
                                   : ThemeArgb(255, p.hairline)),
            focused ? max(1.0f, ScaleReal(2)) : max(1.0f, ScaleReal(1)));
        graphics.DrawPath(&cardBorder, &card);

        Gdiplus::FontFamily uiFamily(UiFamily(), UiFontCollection());
        Gdiplus::SolidBrush ink(disabled ? ThemeArgb(255, p.disabledText)
                                         : (exitCommand ? ThemeArgb(255, p.dangerInk)
                                                        : ThemeArgb(255, p.cardInk)));
        Gdiplus::SolidBrush blue(ThemeArgb(255, p.accentPrimary));

        if (primary)
        {
            Gdiplus::GraphicsPath iconTile;
            AddRoundedRectangle(iconTile, rc.left + ScalePx(28), rc.top + ScalePx(28),
                                ScalePx(76), ScalePx(76), ScalePx(14));
            Gdiplus::SolidBrush iconBg(ThemeArgb(255, p.accentSurface));
            graphics.FillPath(&iconBg, &iconTile);
            DrawOpticalRingMark(graphics, rc.left + ScaleReal(66), rc.top + ScaleReal(66), ScaleReal(24), 210, true);

            Gdiplus::Font titleFont(&uiFamily, ScaleReal(27), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font badgeFont(&uiFamily, ScaleReal(18), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::FontFamily descriptionFamily(UiFamily(), UiFontCollection());
            Gdiplus::Font descriptionFont(&descriptionFamily, ScaleReal(20), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::Font actionFont(&uiFamily, ScaleReal(18), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            const wchar_t* description = commandIndex == 0
                ? L"Create an exact copy of the current disc."
                : (commandIndex == 1 ? L"Extract audio tracks to WAV or FLAC."
                                     : L"Scan the disc surface and read quality.");
            const wchar_t* primaryTitle = commandIndex == 0 ? L"Copy disc"
                : (commandIndex == 1 ? L"Rip tracks" : L"Quality scan");
            const wchar_t* menuNumber = commandIndex == 0 ? L"1" : (commandIndex == 1 ? L"2" : L"6");
            Gdiplus::GraphicsPath numberBadge;
            AddRoundedRectangle(numberBadge, rc.left + ScalePx(128), rc.top + ScalePx(28),
                                ScalePx(34), ScalePx(30), ScalePx(7));
            Gdiplus::SolidBrush badgeBg(ThemeArgb(255, p.accentSurface));
            graphics.FillPath(&badgeBg, &numberBadge);
            graphics.DrawString(menuNumber, -1, &badgeFont,
                                Gdiplus::PointF(rc.left + ScaleReal(139), rc.top + ScaleReal(33)), &blue);
            graphics.DrawString(primaryTitle, -1, &titleFont,
                                Gdiplus::PointF(rc.left + ScaleReal(176), rc.top + ScaleReal(27)), &ink);
            // Same brush as the title, so a disabled hero card dims both. The
            // old dedicated "secondary" brush ignored `disabled` and left the
            // description at full strength under a greyed-out title.
            graphics.DrawString(description, -1, &descriptionFont,
                                Gdiplus::PointF(rc.left + ScaleReal(128), rc.top + ScaleReal(66)), &ink);

            Gdiplus::GraphicsPath actionPill;
            AddRoundedRectangle(actionPill, rc.left + ScalePx(128), rc.top + ScalePx(112),
                                ScalePx(142), ScalePx(38), ScalePx(7));
            graphics.FillPath(&blue, &actionPill);
            // Ink ON the accent, not white: white fails contrast on the lighter
            // accents (2.4:1 on Nord's frost blue).
            Gdiplus::SolidBrush pillInk(ThemeArgb(255, p.onAccentInk));
            graphics.DrawString(commandIndex == 0 ? L"Copy disc" : (commandIndex == 1 ? L"Rip tracks" : L"Quality scan"),
                                -1, &actionFont,
                                Gdiplus::PointF(rc.left + ScaleReal(145), rc.top + ScaleReal(119)), &pillInk);
        }
        else
        {
            Gdiplus::Font numberFont(&uiFamily, ScaleReal(21), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font labelFont(&uiFamily, ScaleReal(23), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            graphics.DrawString(number, -1, &numberFont,
                                Gdiplus::PointF(rc.left + ScaleReal(14), rc.top + ScaleReal(15)),
                                exitCommand ? &ink : &blue);
            Gdiplus::StringFormat labelFormat;
            labelFormat.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            labelFormat.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            labelFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::RectF labelRect(rc.left + ScaleReal(54), (Gdiplus::REAL)rc.top,
                                    max(ScaleReal(20), (Gdiplus::REAL)(rc.right - rc.left) - ScaleReal(68)),
                                    (Gdiplus::REAL)(rc.bottom - rc.top));
            graphics.DrawString(displayLabel, -1, &labelFont, labelRect, &labelFormat, &ink);
        }
}

static void DrawOpticalRingMark(Gdiplus::Graphics& graphics, Gdiplus::REAL cx, Gdiplus::REAL cy,
                                Gdiplus::REAL radius, BYTE alpha, bool includeBlueArc)
{
    const COLORREF silver = ActiveTheme().chromeText;
    const Gdiplus::REAL stroke = max(1.0f, radius * 0.055f);
    Gdiplus::Pen ringPen(ThemeArgb(alpha, silver), stroke);

    const Gdiplus::REAL scales[] = { 1.0f, 0.72f, 0.42f };
    for (Gdiplus::REAL scale : scales)
    {
        const Gdiplus::REAL r = radius * scale;
        graphics.DrawEllipse(&ringPen, cx - r, cy - r, r * 2.0f, r * 2.0f);
    }

    Gdiplus::SolidBrush hub(ThemeArgb((BYTE)min(255, alpha + 18), ActiveTheme().chromeAccent));
    const Gdiplus::REAL hubRadius = max(1.5f, radius * 0.075f);
    graphics.FillEllipse(&hub, cx - hubRadius, cy - hubRadius, hubRadius * 2.0f, hubRadius * 2.0f);

    if (includeBlueArc)
    {
        Gdiplus::Pen blueArc(ThemeArgb(alpha, ActiveTheme().chromeAccent), max(2.0f, radius * 0.11f));
        blueArc.SetStartCap(Gdiplus::LineCapRound);
        blueArc.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawArc(&blueArc, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 218.0f, 184.0f);
    }
}

static void AddRoundedRectangle(Gdiplus::GraphicsPath& path, int x, int y, int width, int height, int radius)
{
    const int diameter = radius * 2;
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
}

static void SplitCommandLabel(LPCWSTR source, WCHAR* number, int numberLength, LPCWSTR* label)
{
    number[0] = L'\0';
    *label = source;

    const WCHAR* dot = wcschr(source, L'.');
    if (!dot || dot == source)
    {
        return;
    }

    const int count = min((int)(dot - source), numberLength - 1);
    wcsncpy_s(number, numberLength, source, count);
    *label = dot + 1;

    while (**label == L' ')
    {
        ++(*label);
    }
}

