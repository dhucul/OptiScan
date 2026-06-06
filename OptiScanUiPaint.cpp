// OptiScanUiPaint.cpp - UI images, brushes, and custom painting.

#include "framework.h"
#include "OptiScanUiInternal.h"

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

    Gdiplus::GdiplusShutdown(gGdiPlusToken);
}

HBRUSH CreateOutputEditBrush(int width, int height)
{
    if (!gOutputBackgroundImage)
    {
        return CreateSolidBrush(OutputDark);
    }

    const int tileWidth = max(1, width);
    const int tileHeight = max(1, height);
    Gdiplus::Bitmap tile(tileWidth, tileHeight, PixelFormat32bppPARGB);
    Gdiplus::Graphics tileGraphics(&tile);
    tileGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    tileGraphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    Gdiplus::SolidBrush baseBrush(Gdiplus::Color(255, GetRValue(OutputDark), GetGValue(OutputDark), GetBValue(OutputDark)));
    tileGraphics.FillRectangle(&baseBrush, 0, 0, tileWidth, tileHeight);

    const int imageWidth = (int)gOutputBackgroundImage->GetWidth();
    const int imageHeight = (int)gOutputBackgroundImage->GetHeight();
    Gdiplus::Rect destination(0, 0, tileWidth, tileHeight);
    tileGraphics.DrawImage(
        gOutputBackgroundImage,
        destination,
        0,
        0,
        imageWidth,
        imageHeight,
        Gdiplus::UnitPixel);

    Gdiplus::SolidBrush toneBrush(Gdiplus::Color(BackgroundAlpha(84), GetRValue(OutputDark), GetGValue(OutputDark), GetBValue(OutputDark)));
    tileGraphics.FillRectangle(&toneBrush, 0, 0, tileWidth, tileHeight);

    Gdiplus::Pen faintLine(Gdiplus::Color(BackgroundAlpha(22), 150, 160, 166), max(1.0f, ScaleReal(1)));
    for (int y = ScalePx(28); y < tileHeight; y += ScalePx(28))
    {
        tileGraphics.DrawLine(&faintLine, 0, y, tileWidth, y);
    }

    HBITMAP bitmap = nullptr;
    if (tile.GetHBITMAP(Gdiplus::Color(255, GetRValue(OutputDark), GetGValue(OutputDark), GetBValue(OutputDark)), &bitmap) != Gdiplus::Ok || !bitmap)
    {
        return CreateSolidBrush(OutputDark);
    }

    HBRUSH brush = CreatePatternBrush(bitmap);
    if (!brush)
    {
        DeleteObject(bitmap);
        return CreateSolidBrush(OutputDark);
    }

    hOutputBrushBitmap = bitmap;
    return brush;
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

static void DrawBackgroundSurface(Gdiplus::Graphics& graphics, const RECT& viewport, int offsetX, int offsetY, bool showFailureText)
{
    const int width = max(1, viewport.right - viewport.left);
    const int height = max(1, viewport.bottom - viewport.top);

    Gdiplus::SolidBrush baseBrush(Gdiplus::Color(255, 3, 4, 5));
    graphics.FillRectangle(&baseBrush, -offsetX, -offsetY, width, height);

    if (gBackgroundImage)
    {
        const int imageWidth = (int)gBackgroundImage->GetWidth();
        const int imageHeight = (int)gBackgroundImage->GetHeight();
        const int drawHeight = (imageWidth > 0)
            ? max(1, (int)((long long)width * imageHeight / imageWidth))
            : height;
        Gdiplus::Rect destination(-offsetX, -offsetY, width, drawHeight);
        graphics.DrawImage(
            gBackgroundImage,
            destination,
            0,
            0,
            imageWidth,
            imageHeight,
            Gdiplus::UnitPixel);
    }
    else if (showFailureText)
    {
        Gdiplus::FontFamily statusFamily(L"Segoe UI");
        Gdiplus::Font statusFont(&statusFamily, ScaleReal(32), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush statusBrush(Gdiplus::Color(255, 154, 164, 176));
        graphics.DrawString(L"Background image did not load", -1, &statusFont, Gdiplus::PointF(ScaleReal(70), ScaleReal(82)), &statusBrush);
    }

    Gdiplus::SolidBrush veil(Gdiplus::Color(BackgroundAlpha(12), 0, 0, 0));
    graphics.FillRectangle(&veil, -offsetX, -offsetY, width, height);
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

    Gdiplus::Pen borderPen(Gdiplus::Color(150, 126, 178, 212), max(1.0f, ScaleReal(1)));
    Gdiplus::Pen softPen(Gdiplus::Color(70, 255, 255, 255), max(1.0f, ScaleReal(1)));
    Gdiplus::SolidBrush panelBrush(Gdiplus::Color(BackgroundAlpha(PanelSurfaceAlpha), 6, 10, 14));
    Gdiplus::SolidBrush panelShade(Gdiplus::Color(110, 126, 178, 212));
    Gdiplus::SolidBrush titleOrange(Gdiplus::Color(255, 198, 178, 150));
    Gdiplus::SolidBrush titleBlue(Gdiplus::Color(255, 156, 168, 180));
    Gdiplus::SolidBrush titleWhite(Gdiplus::Color(255, 216, 222, 226));
    Gdiplus::SolidBrush muted(Gdiplus::Color(255, 136, 148, 160));

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
    if (gOutputBackgroundImage)
    {
        graphics.SetClip(&outputPanel);

        const int imageWidth = (int)gOutputBackgroundImage->GetWidth();
        const int imageHeight = (int)gOutputBackgroundImage->GetHeight();
        Gdiplus::Rect outputPanelRect(layoutMargin, outputTop - ScalePx(42), panelWidth, outputHeight + ScalePx(42));
        graphics.DrawImage(
            gOutputBackgroundImage,
            outputPanelRect,
            0,
            0,
            imageWidth,
            imageHeight,
            Gdiplus::UnitPixel);

        graphics.FillPath(&panelBrush, &outputPanel);
        graphics.ResetClip();
    }
    else
    {
        graphics.FillPath(&panelBrush, &outputPanel);
    }
    graphics.DrawPath(&borderPen, &outputPanel);

    const int consoleInset = ScalePx(22);
    const int consoleHeaderHeight = ScalePx(40);
    const int consoleLeft = layoutMargin + consoleInset;
    const int consoleTop = outputTop + ScalePx(5);
    const int consoleWidth = max(ScalePx(200), panelWidth - (consoleInset * 2));
    const int consoleHeight = max(ScalePx(140), outputHeight - ScalePx(10));

    Gdiplus::GraphicsPath consoleFrame;
    AddRoundedRectangle(consoleFrame, consoleLeft - ScalePx(2), consoleTop - ScalePx(2), consoleWidth + ScalePx(4), consoleHeight + ScalePx(4), ScalePx(12));
    Gdiplus::SolidBrush consoleFrameBrush(Gdiplus::Color(BackgroundAlpha(64), 4, 5, 6));
    Gdiplus::Pen consoleBorder(Gdiplus::Color(88, 126, 178, 212), 1.0f);

    if (gOutputBackgroundImage)
    {
        graphics.SetClip(&consoleFrame);

        const int imageWidth = (int)gOutputBackgroundImage->GetWidth();
        const int imageHeight = (int)gOutputBackgroundImage->GetHeight();

        Gdiplus::Rect outputTextureRect(consoleLeft - ScalePx(2), consoleTop - ScalePx(2), consoleWidth + ScalePx(4), consoleHeight + ScalePx(4));
        graphics.DrawImage(
            gOutputBackgroundImage,
            outputTextureRect,
            0,
            0,
            imageWidth,
            imageHeight,
            Gdiplus::UnitPixel);

        Gdiplus::SolidBrush outputTone(Gdiplus::Color(BackgroundAlpha(112), 4, 5, 6));
        graphics.FillPath(&outputTone, &consoleFrame);
        graphics.ResetClip();
    }
    else
    {
        graphics.FillPath(&consoleFrameBrush, &consoleFrame);
    }

    graphics.DrawPath(&consoleBorder, &consoleFrame);

    Gdiplus::Rect consoleHeaderRect(consoleLeft, consoleTop, consoleWidth, consoleHeaderHeight);
    Gdiplus::LinearGradientBrush consoleHeaderBrush(
        consoleHeaderRect,
        Gdiplus::Color(BackgroundAlpha(50), 12, 16, 18),
        Gdiplus::Color(BackgroundAlpha(34), 5, 7, 8),
        Gdiplus::LinearGradientModeVertical);
    graphics.FillRectangle(&consoleHeaderBrush, consoleHeaderRect);

    Gdiplus::Pen consoleRule(Gdiplus::Color(68, 126, 178, 212), 1.0f);
    graphics.DrawLine(&consoleRule, consoleLeft + ScalePx(12), consoleTop + consoleHeaderHeight, consoleLeft + consoleWidth - ScalePx(12), consoleTop + consoleHeaderHeight);

    Gdiplus::SolidBrush consoleDot(Gdiplus::Color(135, 126, 178, 212));
    for (int i = 0; i < 3; ++i)
    {
        graphics.FillEllipse(&consoleDot, consoleLeft + ScalePx(14) + (i * ScalePx(16)), consoleTop + ScalePx(15), ScalePx(7), ScalePx(7));
    }

    Gdiplus::FontFamily consoleFamily(L"Segoe UI");
    Gdiplus::Font consoleMini(&consoleFamily, ScaleReal(15), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush consoleText(Gdiplus::Color(185, 178, 188, 196));
    graphics.DrawString(L"READY", -1, &consoleMini, Gdiplus::PointF((Gdiplus::REAL)(consoleLeft + ScalePx(72)), (Gdiplus::REAL)(consoleTop + ScalePx(11))), &consoleText);

    Gdiplus::SolidBrush consoleBar(Gdiplus::Color(92, 126, 178, 212));
    for (int i = 0; i < 18; ++i)
    {
        const int barHeight = ScalePx(7 + ((i % 4) * 3));
        const int barX = consoleLeft + consoleWidth - ScalePx(190) + (i * ScalePx(8));
        graphics.FillRectangle(&consoleBar, barX, consoleTop + ScalePx(22) - barHeight, ScalePx(4), barHeight);
    }

    graphics.FillRectangle(&panelShade, layoutMargin + ScalePx(1), top - ScalePx(69), ScalePx(7), commandPanelHeight + ScalePx(68));
    graphics.FillRectangle(&panelShade, layoutMargin + ScalePx(1), outputTop - ScalePx(41), ScalePx(7), outputHeight + ScalePx(40));
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
    graphics.DrawString(L"Opti", -1, &brandFont, Gdiplus::PointF(brandX, ScaleReal(82)), &brandFormat, &titleOrange);
    graphics.DrawString(L"Scan", -1, &brandFont, Gdiplus::PointF(brandX + optiBounds.Width, ScaleReal(82)), &brandFormat, &titleOrange);
    graphics.DrawLine(&borderPen, (rc.right / 2) - ScalePx(210), ScalePx(142), (rc.right / 2) + ScalePx(210), ScalePx(142));
    graphics.DrawString(L"COMMAND MENU", -1, &eyebrowFont, Gdiplus::PointF((Gdiplus::REAL)(layoutMargin + ScalePx(20)), (Gdiplus::REAL)(top - ScalePx(54))), &titleWhite);
    graphics.DrawString(L"OUTPUT LOG", -1, &eyebrowFont, Gdiplus::PointF((Gdiplus::REAL)(layoutMargin + ScalePx(24)), (Gdiplus::REAL)(outputTop - ScalePx(31))), &titleOrange);
}

void DrawCommandButton(const DRAWITEMSTRUCT* drawItem)
{
    if (!drawItem || drawItem->CtlType != ODT_BUTTON ||
        drawItem->CtlID < IDC_INFO_BUTTON1 || drawItem->CtlID > IDC_INFO_BUTTON34)
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

    // Dimmed-grey text for disabled buttons. Picked so it's visibly dim
    // against the dark button background but still legible.
    const COLORREF DisabledText = RGB(96, 100, 104);

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

    Gdiplus::Color topColor = pressed ? Gdiplus::Color(240, 52, 58, 68) : Gdiplus::Color(220, 38, 44, 54);
    Gdiplus::Color bottomColor = pressed ? Gdiplus::Color(240, 22, 27, 36) : Gdiplus::Color(220, 16, 20, 28);
    Gdiplus::Rect buttonRect(rc.left, rc.top, max(1, rc.right - rc.left), max(1, rc.bottom - rc.top));
    Gdiplus::LinearGradientBrush buttonBrush(buttonRect, topColor, bottomColor, Gdiplus::LinearGradientModeVertical);
    graphics.FillPath(&buttonBrush, &buttonPath);

    Gdiplus::Pen borderPen(
        focused ? Gdiplus::Color(235, 218, 224, 230) : Gdiplus::Color(130, 158, 168, 178),
        focused ? max(1.0f, ScaleReal(2)) : max(1.0f, ScaleReal(1)));
    graphics.DrawPath(&borderPen, &buttonPath);

    Gdiplus::GraphicsPath accentPath;
    AddRoundedRectangle(accentPath, rc.left + ScalePx(1), rc.top + ScalePx(1), ScalePx(5), rc.bottom - rc.top - ScalePx(2), ScalePx(4));
    Gdiplus::SolidBrush accentBrush(exitCommand ? Gdiplus::Color(230, 190, 118, 118) : Gdiplus::Color(120, 166, 176, 188));
    graphics.FillPath(&accentBrush, &accentPath);

    Gdiplus::Pen glowPen(Gdiplus::Color(clearCommand ? 120 : 82, 166, 176, 188), max(1.0f, ScaleReal(1)));
    graphics.DrawLine(&glowPen, (INT)(rc.left + ScalePx(18)), (INT)(rc.bottom - ScalePx(2)), (INT)(rc.right - ScalePx(12)), (INT)(rc.bottom - ScalePx(2)));

    WCHAR number[8];
    LPCWSTR label = CommandLabels[commandIndex];
    SplitCommandLabel(CommandLabels[commandIndex], number, ARRAYSIZE(number), &label);

    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, hHeaderFont);
    SetTextColor(hdc, disabled ? DisabledText
                                : (exitCommand ? RGB(202, 132, 130) : MenuNumberGrey));

    RECT numberRect = { rc.left + ScalePx(18), rc.top, rc.left + ScalePx(72), rc.bottom };
    DrawTextW(hdc, number, -1, &numberRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);

    SelectObject(hdc, hCommandFont);
    SetTextColor(hdc, disabled ? DisabledText
                                : (exitCommand ? RGB(214, 142, 138) : MenuTextGrey));

    RECT textRect = { rc.left + ScalePx(78), rc.top, rc.right - ScalePx(14), rc.bottom };
    DrawTextW(hdc, label, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
}

static void DrawTechAccents(Gdiplus::Graphics& graphics, const RECT& rc)
{
    Gdiplus::Pen faintLine(Gdiplus::Color(70, 220, 232, 238), max(1.0f, ScaleReal(1)));
    Gdiplus::Pen orangeLine(Gdiplus::Color(150, 126, 178, 212), max(1.0f, ScaleReal(2)));
    Gdiplus::Pen dimOrange(Gdiplus::Color(88, 126, 178, 212), max(1.0f, ScaleReal(1)));
    Gdiplus::SolidBrush orangeDot(Gdiplus::Color(180, 126, 178, 212));
    Gdiplus::SolidBrush darkGlow(Gdiplus::Color(86, 126, 178, 212));
    Gdiplus::SolidBrush softPanelGlow(Gdiplus::Color(22, 126, 178, 212));

    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int left = ScalePx(88);
    const int right = max(left + ScalePx(400), width - ScalePx(820));
    const int lower = max(ScalePx(760), height - ScalePx(420));

    graphics.FillRectangle(&softPanelGlow, ScalePx(42), ScalePx(78), max(1, width - ScalePx(84)), ScalePx(62));
    graphics.FillRectangle(&softPanelGlow, ScalePx(42), height - ScalePx(420), max(1, width - ScalePx(84)), ScalePx(48));

    for (int i = 0; i < 9; ++i)
    {
        const int y = ScalePx(220) + (i * ScalePx(58));
        const int x1 = left + (i % 3) * ScalePx(54);
        const int x2 = min(width - ScalePx(120), x1 + ScalePx(480) + (i * ScalePx(38)));
        graphics.DrawLine(&faintLine, x1, y, x2, y);
        graphics.DrawLine(&dimOrange, x2, y, x2 + ScalePx(56), y + ScalePx(22));
        graphics.FillEllipse(&orangeDot, x2 - ScalePx(5), y - ScalePx(5), ScalePx(10), ScalePx(10));
    }

    for (int i = 0; i < 7; ++i)
    {
        const int y = ScalePx(185) + (i * ScalePx(76));
        const int x = right + (i * ScalePx(34));
        graphics.DrawLine(&orangeLine, x, y, min(width - ScalePx(95), x + ScalePx(260)), y);
        graphics.DrawLine(&faintLine, min(width - ScalePx(95), x + ScalePx(260)), y, min(width - ScalePx(65), x + ScalePx(310)), y + ScalePx(34));
        graphics.FillEllipse(&orangeDot, x - ScalePx(5), y - ScalePx(5), ScalePx(10), ScalePx(10));
    }

    for (int i = 0; i < 8; ++i)
    {
        const int cx = right - ScalePx(40) + (i * ScalePx(88));
        const int cy = lower + ((i % 2) * ScalePx(54));
        const int radius = ScalePx(34);
        Gdiplus::Point points[6] =
        {
            Gdiplus::Point(cx + radius, cy),
            Gdiplus::Point(cx + radius / 2, cy + ScalePx(29)),
            Gdiplus::Point(cx - radius / 2, cy + ScalePx(29)),
            Gdiplus::Point(cx - radius, cy),
            Gdiplus::Point(cx - radius / 2, cy - ScalePx(29)),
            Gdiplus::Point(cx + radius / 2, cy - ScalePx(29))
        };
        graphics.DrawPolygon(i == 1 ? &orangeLine : &dimOrange, points, 6);
    }

    for (int i = 0; i < 38; ++i)
    {
        const int x = ScalePx(120) + (i * ScalePx(87)) % max(1, width - ScalePx(240));
        const int y = ScalePx(160) + (i * ScalePx(131)) % max(1, height - ScalePx(360));
        const int dotSize = ScalePx(4 + (i % 3) * 2);
        graphics.FillEllipse(&darkGlow, x, y, dotSize, dotSize);
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

