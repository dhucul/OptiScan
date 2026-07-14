// OptiScanUiPaint.cpp - UI images, brushes, and custom painting.

#include "framework.h"
#include "OptiScanUiInternal.h"
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
static void DrawTechAccents(Gdiplus::Graphics& graphics, const RECT& rc);

// Build a GDI+ ARGB colour from a theme COLORREF (0x00BBGGRR) plus an alpha.
// The chrome is drawn semi-transparent over the artwork, so alpha stays at the
// paint site while the RGB comes from the active palette.
static inline Gdiplus::Color ThemeArgb(BYTE a, COLORREF c)
{
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

// Nudge a colour lighter (for pressed-button gradient stops).
static inline COLORREF Lighten(COLORREF c, int amt)
{
    const int r = min(255, GetRValue(c) + amt);
    const int g = min(255, GetGValue(c) + amt);
    const int b = min(255, GetBValue(c) + amt);
    return RGB(r, g, b);
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

// Paint the whole themed backdrop procedurally from the active palette into a
// width x height area: diagonal gradient base, radial accent glows, turntable
// concentric rings + glowing hub, EQ bars with warm accent caps, flowing wave
// lines, faint tech dots, and an edge vignette. Replaces the fixed navy/gold
// artwork so every theme looks native. Rendered once into a cached bitmap
// (EnsureBackdropBitmap) since it's blitted many times per paint.
static void PaintProceduralBackdrop(Gdiplus::Graphics& g, int width, int height)
{
    const Palette& p = ActiveTheme();
    const float W = (float)width, H = (float)height;
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    // 1. diagonal base gradient (slight lift -> base)
    Gdiplus::Rect full(0, 0, width, height);
    Gdiplus::LinearGradientBrush baseGrad(full,
        ThemeArgb(255, Lighten(p.windowBase, 10)), ThemeArgb(255, p.windowBase), 45.0f);
    g.FillRectangle(&baseGrad, full);

    // (The disc-arc and flowing-wave line motifs that used to live here were
    //  removed — pale lines on the light base were undetectable. The aurora
    //  colour field above is the design now.)

    // 7. edge vignette. A black surround reads as a heavy grey smear on light
    // themes, so scale the darkening by the base luminance: full on dark bases,
    // a soft shadow on light ones.
    const int baseLuma = (GetRValue(p.windowBase) * 299 +
                          GetGValue(p.windowBase) * 587 +
                          GetBValue(p.windowBase) * 114) / 1000;
    const BYTE vignetteAlpha = baseLuma > 150 ? (BYTE)0 : (BYTE)120;
    Gdiplus::GraphicsPath vp;
    vp.AddEllipse(-W * 0.2f, -H * 0.2f, W * 1.4f, H * 1.4f);
    Gdiplus::PathGradientBrush vg(&vp);
    vg.SetCenterColor(ThemeArgb(0, p.windowBase));
    Gdiplus::Color vSurround[] = { Gdiplus::Color(vignetteAlpha, 0, 0, 0) };
    int vcnt = 1;
    vg.SetSurroundColors(vSurround, &vcnt);
    g.FillRectangle(&vg, full);
}

// Cached backdrop bitmap. Regenerated when the size or theme changes; blitted
// (with per-button offset) by DrawBackgroundSurface so the procedural art is
// painted once per frame instead of once per owner-drawn button.
static Gdiplus::Bitmap* gBackdropBitmap = nullptr;
static int gBackdropW = 0;
static int gBackdropH = 0;
static int gBackdropThemeId = -1;

static void EnsureBackdropBitmap(int width, int height)
{
    width = max(1, width);
    height = max(1, height);
    const int tid = (int)CurrentThemeId();
    if (gBackdropBitmap && gBackdropW == width && gBackdropH == height && gBackdropThemeId == tid)
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
}


static Gdiplus::Image* LoadPngFromResource(WORD resourceId)
{
    HMODULE hModule = GetModuleHandleW(nullptr);
    HRSRC hResInfo = FindResourceW(hModule, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hResInfo) return nullptr;

    DWORD size = SizeofResource(hModule, hResInfo);
    HGLOBAL hResData = LoadResource(hModule, hResInfo);
    if (!hResData || size == 0) return nullptr;

    const void* pData = LockResource(hResData);
    if (!pData) return nullptr;

    HGLOBAL hBuffer = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hBuffer) return nullptr;

    void* pBuffer = GlobalLock(hBuffer);
    if (!pBuffer)
    {
        GlobalFree(hBuffer);
        return nullptr;
    }
    memcpy(pBuffer, pData, size);
    GlobalUnlock(hBuffer);

    IStream* pStream = nullptr;
    if (CreateStreamOnHGlobal(hBuffer, TRUE, &pStream) != S_OK || !pStream)
    {
        GlobalFree(hBuffer);
        return nullptr;
    }

    Gdiplus::Image* image = Gdiplus::Image::FromStream(pStream);
    pStream->Release();

    if (!image || image->GetLastStatus() != Gdiplus::Ok)
    {
        delete image;
        return nullptr;
    }
    return image;
}

static void LoadBackgroundImage()
{
    gBackgroundImage = LoadPngFromResource(IDR_TOP_PNG);
    gOutputBackgroundImage = LoadPngFromResource(IDR_OUTPUT_PNG);
}

void InitializeUiResources()
{
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gGdiPlusToken, &gdiplusStartupInput, nullptr);
    LoadBackgroundImage();
}

void DestroyUiResources()
{
    delete gBackgroundImage;
    gBackgroundImage = nullptr;

    delete gOutputBackgroundImage;
    gOutputBackgroundImage = nullptr;

    if (hCommandFont)
    {
        DeleteObject(hCommandFont);
        hCommandFont = nullptr;
    }

    if (hHeaderFont)
    {
        DeleteObject(hHeaderFont);
        hHeaderFont = nullptr;
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
        Gdiplus::SolidBrush baseBrush(ThemeArgb(255, ActiveTheme().windowBase));
        graphics.FillRectangle(&baseBrush, -offsetX, -offsetY, width, height);
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

    DrawBackgroundSurface(graphics, rc, 0, 0, true);

    Gdiplus::Pen borderPen(ThemeArgb(150, ActiveTheme().chromeAccent), max(1.0f, ScaleReal(1)));
    Gdiplus::Pen softPen(Gdiplus::Color(70, 255, 255, 255), max(1.0f, ScaleReal(1)));
    // Coloured panels: light themes fill the command/output panels with a solid
    // light soft-blue so there is clear colour directly behind the white button
    // cards. Dark themes keep the translucent panel veil.
    const int pnlLum = (GetRValue(ActiveTheme().windowBase) * 299 + GetGValue(ActiveTheme().windowBase) * 587 +
                        GetBValue(ActiveTheme().windowBase) * 114) / 1000;
    Gdiplus::SolidBrush panelBrush(
        pnlLum > 150 ? Gdiplus::Color(238, 205, 222, 245)   // #CDDEF5 light soft-blue
                     : ThemeArgb(BackgroundAlpha(PanelSurfaceAlpha), ActiveTheme().panelSurface));
    Gdiplus::SolidBrush titleOrange(ThemeArgb(255, ActiveTheme().accentWarm));
    Gdiplus::SolidBrush titleBlue(ThemeArgb(255, ActiveTheme().chromeAccent));
    Gdiplus::SolidBrush titleWhite(ThemeArgb(255, ActiveTheme().bright));
    Gdiplus::SolidBrush muted(ThemeArgb(255, ActiveTheme().dim));

    const int margin = ScalePx(18);
    graphics.DrawRectangle(&softPen, margin, margin, max(0, rc.right - (margin * 2)), max(0, rc.bottom - (margin * 2)));

    const int top = ScalePx(145);
    const int layoutMargin = ScalePx(40);
    const int gap = ScalePx(8);
    const int buttonHeight = ScalePx(46);
    const int labelHeight = ScalePx(30);
    const int middleColumnHeight =
        (labelHeight + gap) +
        (6 * (buttonHeight + gap)) +
        (labelHeight + gap) +
        (7 * (buttonHeight + gap));
    const int minimumOutputTop = top + middleColumnHeight + ScalePx(32);
    const int requestedOutputHeight = max(ScalePx(420), min(ScalePx(680), ((rc.bottom - rc.top) * 38) / 100));
    const int outputTop = max(minimumOutputTop, rc.bottom - layoutMargin - requestedOutputHeight);
    const int outputHeight = max(ScalePx(300), rc.bottom - layoutMargin - outputTop);
    const int panelWidth = max(ScalePx(900), rc.right - rc.left - (layoutMargin * 2));
    const int commandPanelHeight = max(ScalePx(220), outputTop - top - ScalePx(26));

    Gdiplus::GraphicsPath commandPanel;
    AddRoundedRectangle(commandPanel, layoutMargin, top - ScalePx(70), panelWidth, commandPanelHeight + ScalePx(70), ScalePx(18));
    graphics.FillPath(&panelBrush, &commandPanel);
    graphics.DrawPath(&borderPen, &commandPanel);

    Gdiplus::GraphicsPath outputPanel;
    AddRoundedRectangle(outputPanel, layoutMargin, outputTop - ScalePx(42), panelWidth, outputHeight + ScalePx(42), ScalePx(18));
    // Translucent panel fill over the procedural backdrop painted above.
    graphics.FillPath(&panelBrush, &outputPanel);
    graphics.DrawPath(&borderPen, &outputPanel);

    const int consoleInset = ScalePx(22);
    const int consoleHeaderHeight = ScalePx(40);
    const int consoleLeft = layoutMargin + consoleInset;
    const int consoleTop = outputTop + ScalePx(5);
    const int consoleWidth = max(ScalePx(200), panelWidth - (consoleInset * 2));
    const int consoleHeight = max(ScalePx(140), outputHeight - ScalePx(10));

    Gdiplus::GraphicsPath consoleFrame;
    AddRoundedRectangle(consoleFrame, consoleLeft - ScalePx(2), consoleTop - ScalePx(2), consoleWidth + ScalePx(4), consoleHeight + ScalePx(4), ScalePx(12));
    Gdiplus::SolidBrush consoleFrameBrush(ThemeArgb(BackgroundAlpha(64), ActiveTheme().consoleBase));
    Gdiplus::Pen consoleBorder(ThemeArgb(88, ActiveTheme().chromeAccent), 1.0f);

    // Darken the console frame slightly over the procedural backdrop so the
    // log text panel reads as a recessed surface.
    graphics.FillPath(&consoleFrameBrush, &consoleFrame);

    graphics.DrawPath(&consoleBorder, &consoleFrame);

    Gdiplus::Rect consoleHeaderRect(consoleLeft, consoleTop, consoleWidth, consoleHeaderHeight);
    Gdiplus::LinearGradientBrush consoleHeaderBrush(
        consoleHeaderRect,
        ThemeArgb(BackgroundAlpha(50), ActiveTheme().outputBg),
        ThemeArgb(BackgroundAlpha(34), ActiveTheme().consoleBase),
        Gdiplus::LinearGradientModeVertical);
    graphics.FillRectangle(&consoleHeaderBrush, consoleHeaderRect);

    Gdiplus::Pen consoleRule(ThemeArgb(68, ActiveTheme().chromeAccent), 1.0f);
    graphics.DrawLine(&consoleRule, consoleLeft + ScalePx(12), consoleTop + consoleHeaderHeight, consoleLeft + consoleWidth - ScalePx(12), consoleTop + consoleHeaderHeight);

    Gdiplus::SolidBrush consoleDot(ThemeArgb(135, ActiveTheme().chromeAccent));
    for (int i = 0; i < 3; ++i)
    {
        graphics.FillEllipse(&consoleDot, consoleLeft + ScalePx(14) + (i * ScalePx(16)), consoleTop + ScalePx(15), ScalePx(7), ScalePx(7));
    }

    Gdiplus::FontFamily consoleFamily(L"Segoe UI");
    Gdiplus::Font consoleMini(&consoleFamily, ScaleReal(15), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush consoleText(ThemeArgb(185, ActiveTheme().chromeText));
    graphics.DrawString(L"READY", -1, &consoleMini, Gdiplus::PointF((Gdiplus::REAL)(consoleLeft + ScalePx(72)), (Gdiplus::REAL)(consoleTop + ScalePx(11))), &consoleText);

    Gdiplus::SolidBrush consoleBar(ThemeArgb(92, ActiveTheme().chromeAccent));
    for (int i = 0; i < 18; ++i)
    {
        const int barHeight = ScalePx(7 + ((i % 4) * 3));
        const int barX = consoleLeft + consoleWidth - ScalePx(190) + (i * ScalePx(8));
        graphics.FillRectangle(&consoleBar, barX, consoleTop + ScalePx(22) - barHeight, ScalePx(4), barHeight);
    }

    // (Removed the thick blue accent bar down the left edge of each panel — its
    //  colour collided with the left-edge accent stripe on the first column of
    //  buttons. The rounded panel border alone defines the edge now.)
    DrawTechAccents(graphics, rc);

    Gdiplus::FontFamily titleFamily(L"Segoe UI");
    Gdiplus::FontFamily brandFamily(L"Bahnschrift");
    Gdiplus::Font brandFont(&brandFamily, ScaleReal(56), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font smallFont(&titleFamily, ScaleReal(22), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font eyebrowFont(&titleFamily, ScaleReal(20), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

    Gdiplus::StringFormat brandFormat(Gdiplus::StringFormat::GenericTypographic());
    Gdiplus::RectF optiBounds;
    Gdiplus::RectF scanBounds;
    graphics.MeasureString(L"Opti", -1, &brandFont, Gdiplus::PointF(0.0f, 0.0f), &brandFormat, &optiBounds);
    graphics.MeasureString(L"Scan", -1, &brandFont, Gdiplus::PointF(0.0f, 0.0f), &brandFormat, &scanBounds);

    const Gdiplus::REAL brandX = ((Gdiplus::REAL)rc.right - optiBounds.Width - scanBounds.Width) / 2.0f;

    // Soft halo behind the wordmark so the title stays crisp over the colourful
    // aurora backdrop. (The old header waveform lines were removed — too faint.)
    {
        const Gdiplus::REAL waveY = ScaleReal(112);
        const Gdiplus::REAL wcx = brandX + (optiBounds.Width + scanBounds.Width) / 2.0f;
        const Gdiplus::REAL hw  = (optiBounds.Width + scanBounds.Width) * 0.66f + ScaleReal(30);
        const Gdiplus::REAL hh  = ScaleReal(54);
        Gdiplus::GraphicsPath hp;
        hp.AddEllipse(wcx - hw, waveY - hh, hw * 2, hh * 2);
        Gdiplus::PathGradientBrush hb(&hp);
        hb.SetCenterColor(ThemeArgb(220, ActiveTheme().panelSurface));
        Gdiplus::Color haloEdge[] = { ThemeArgb(0, ActiveTheme().panelSurface) };
        int hcnt = 1;
        hb.SetSurroundColors(haloEdge, &hcnt);
        graphics.FillPath(&hb, &hp);
    }

    graphics.DrawString(L"Opti", -1, &brandFont, Gdiplus::PointF(brandX, ScaleReal(82)), &brandFormat, &titleOrange);
    graphics.DrawString(L"Scan", -1, &brandFont, Gdiplus::PointF(brandX + optiBounds.Width, ScaleReal(82)), &brandFormat, &titleWhite);

    // Divider: a warm-accent hairline split around a small centred diamond node,
    // replacing the old flat line for a more refined header.
    {
        Gdiplus::Pen hairPen(ThemeArgb(150, ActiveTheme().accentWarm), max(1.0f, ScaleReal(1)));
        const int cxMid = rc.right / 2;
        const int hy = ScalePx(142);
        graphics.DrawLine(&hairPen, cxMid - ScalePx(210), hy, cxMid - ScalePx(12), hy);
        graphics.DrawLine(&hairPen, cxMid + ScalePx(12), hy, cxMid + ScalePx(210), hy);

        Gdiplus::SolidBrush node(ThemeArgb(220, ActiveTheme().btnStripe));
        Gdiplus::GraphicsState st = graphics.Save();
        graphics.TranslateTransform((Gdiplus::REAL)cxMid, (Gdiplus::REAL)hy);
        graphics.RotateTransform(45.0f);
        const Gdiplus::REAL d = ScaleReal(5);
        graphics.FillRectangle(&node, -d / 2.0f, -d / 2.0f, d, d);
        graphics.Restore(st);
    }
    graphics.DrawString(L"COMMAND MENU", -1, &eyebrowFont, Gdiplus::PointF((Gdiplus::REAL)(layoutMargin + ScalePx(20)), (Gdiplus::REAL)(top - ScalePx(54))), &titleWhite);
    graphics.DrawString(L"OUTPUT LOG", -1, &eyebrowFont, Gdiplus::PointF((Gdiplus::REAL)(layoutMargin + ScalePx(24)), (Gdiplus::REAL)(outputTop - ScalePx(31))), &titleOrange);
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
    const bool clearCommand = commandIndex == kClearButtonIndex;
    const bool exitCommand = commandIndex == kExitButtonIndex;

    // Dimmed text for disabled buttons; from the active theme.
    const COLORREF DisabledText = ActiveTheme().disabledText;

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

    Gdiplus::GraphicsPath buttonPath;
    AddRoundedRectangle(buttonPath, rc.left, rc.top, rc.right - rc.left - ScalePx(1), rc.bottom - rc.top - ScalePx(1), ScalePx(7));

    // Soft drop shadow so the (near-opaque) button lifts off the textured,
    // deeper backdrop. Two low-alpha offset layers approximate a blur.
    if (!pressed)
    {
        Gdiplus::GraphicsPath sp1, sp2;
        AddRoundedRectangle(sp1, rc.left + ScalePx(1), rc.top + ScalePx(3), rc.right - rc.left - ScalePx(2), rc.bottom - rc.top - ScalePx(1), ScalePx(8));
        AddRoundedRectangle(sp2, rc.left + ScalePx(2), rc.top + ScalePx(2), rc.right - rc.left - ScalePx(4), rc.bottom - rc.top - ScalePx(1), ScalePx(8));
        Gdiplus::SolidBrush sb1(Gdiplus::Color(16, 26, 38, 62));
        Gdiplus::SolidBrush sb2(Gdiplus::Color(24, 26, 38, 62));
        graphics.FillPath(&sb1, &sp1);
        graphics.FillPath(&sb2, &sp2);
    }

    // Frosted-glass fill: semi-translucent so the themed backdrop (disc arcs +
    // waves) reads softly through the button faces, not just in the gaps. Border
    // + drop shadow keep the cards defined so they still stand out.
    Gdiplus::Color topColor = pressed ? ThemeArgb(240, Lighten(ActiveTheme().btnTop, 14)) : ThemeArgb(242, ActiveTheme().btnTop);
    Gdiplus::Color bottomColor = pressed ? ThemeArgb(240, Lighten(ActiveTheme().btnBottom, 8)) : ThemeArgb(242, ActiveTheme().btnBottom);
    Gdiplus::Rect buttonRect(rc.left, rc.top, max(1, rc.right - rc.left), max(1, rc.bottom - rc.top));
    Gdiplus::LinearGradientBrush buttonBrush(buttonRect, topColor, bottomColor, Gdiplus::LinearGradientModeVertical);
    graphics.FillPath(&buttonBrush, &buttonPath);

    Gdiplus::Pen borderPen(
        focused ? ThemeArgb(235, ActiveTheme().btnBorderFocus) : ThemeArgb(165, ActiveTheme().btnBorder),
        focused ? max(1.0f, ScaleReal(2)) : max(1.0f, ScaleReal(1)));
    graphics.DrawPath(&borderPen, &buttonPath);

    Gdiplus::GraphicsPath accentPath;
    AddRoundedRectangle(accentPath, rc.left + ScalePx(1), rc.top + ScalePx(1), ScalePx(5), rc.bottom - rc.top - ScalePx(2), ScalePx(4));
    Gdiplus::SolidBrush accentBrush(exitCommand ? ThemeArgb(230, ActiveTheme().error) : ThemeArgb(200, ActiveTheme().btnStripe));
    graphics.FillPath(&accentBrush, &accentPath);

    Gdiplus::Pen glowPen(ThemeArgb(clearCommand ? 120 : 82, ActiveTheme().btnStripe), max(1.0f, ScaleReal(1)));
    graphics.DrawLine(&glowPen, (INT)(rc.left + ScalePx(18)), (INT)(rc.bottom - ScalePx(2)), (INT)(rc.right - ScalePx(12)), (INT)(rc.bottom - ScalePx(2)));

    WCHAR number[8];
    LPCWSTR label = CommandLabels[commandIndex];
    SplitCommandLabel(CommandLabels[commandIndex], number, ARRAYSIZE(number), &label);

    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, hHeaderFont);
    SetTextColor(hdc, disabled ? DisabledText
                                : (exitCommand ? ActiveTheme().error : MenuNumberGrey));

    RECT numberRect = { rc.left + ScalePx(18), rc.top, rc.left + ScalePx(72), rc.bottom };
    DrawTextW(hdc, number, -1, &numberRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);

    SelectObject(hdc, hCommandFont);
    SetTextColor(hdc, disabled ? DisabledText
                                : (exitCommand ? Lighten(ActiveTheme().error, 12) : MenuTextGrey));

    RECT textRect = { rc.left + ScalePx(78), rc.top, rc.right - ScalePx(14), rc.bottom };
    DrawTextW(hdc, label, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
}

static void DrawTechAccents(Gdiplus::Graphics& graphics, const RECT& rc)
{
    // The old "circuit" clutter (horizontal lines, hexagons, and 38 scattered
    // dots — several of which used the cool cyan role) has been retired. Depth
    // now comes from the cleaner procedural backdrop (corner blooms + top-right
    // disc emboss) painted behind the panels. All that remains here is a faint
    // accent-tinted band behind each panel's eyebrow label.
    Gdiplus::SolidBrush softPanelGlow(ThemeArgb(22, ActiveTheme().chromeAccent));
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    graphics.FillRectangle(&softPanelGlow, ScalePx(42), ScalePx(78), max(1, width - ScalePx(84)), ScalePx(62));
    graphics.FillRectangle(&softPanelGlow, ScalePx(42), height - ScalePx(420), max(1, width - ScalePx(84)), ScalePx(48));
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

