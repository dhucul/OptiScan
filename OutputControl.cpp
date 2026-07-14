#define NOMINMAX
#include "OutputControl.h"
#include "Theme.h"

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {

    // ------------------------------------------------------------------------
    // Palette. Seeded from the active theme's Palette (see ApplyTheme below).
    // Mutable so the runtime theme switch can re-colour the log. Defaults are
    // the Graphite values; OutputControl::ApplyTheme() overwrites them.
    // ------------------------------------------------------------------------
    COLORREF LogDefault    = RGB(216, 216, 211);
    COLORREF LogDim        = RGB(118, 128, 138);
    COLORREF LogHeading    = RGB(198, 198, 192);
    COLORREF LogCommand    = RGB(198, 178, 150);
    COLORREF LogOk         = RGB(148, 192, 166);
    COLORREF LogWarn       = RGB(210, 174, 112);
    COLORREF LogError      = RGB(222, 126, 122);
    COLORREF LogInfo       = RGB(204, 204, 198);
    COLORREF LogGraphFrame = RGB(150, 160, 170);
    COLORREF LogGraphBar   = RGB(174, 182, 190);
    COLORREF PanelDark     = RGB(14, 17, 22);
    COLORREF SelectionBg   = RGB(58, 66, 78);

    enum : UINT_PTR {
        IDM_COPY       = 1001,
        IDM_COPY_ALL   = 1002,
        IDM_SELECT_ALL = 1003,
    };

    constexpr UINT_PTR kAutoScrollTimer   = 0xA5C1;
    constexpr UINT     kAutoScrollPeriodMs = 50;

    // Per-line colour heuristic for runs that didn't get an explicit ANSI
    // SGR colour. Deliberately narrow: only line-start prefixes count, so a
    // success message that happens to contain the substring "error" or
    // "done" (e.g. "all errors were correctable", "no C2 correction needed")
    // doesn't get miscoloured. Workflows that want a specific colour should
    // either emit an explicit SGR escape via Console::SetColorRGB or prefix
    // the line with one of the recognised tags below.
    COLORREF HeuristicColor(const std::wstring& lineText) {
        size_t first = lineText.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) return LogDefault;
        const std::wstring view = lineText.substr(first);

        if (view.rfind(L">>>", 0) == 0) return LogCommand;
        if (view.rfind(L"===", 0) == 0 || view.rfind(L"---", 0) == 0) return LogHeading;

        // Explicit status tags at the start of a line — these are emitted
        // deliberately by the workflow code and never false-positive.
        if (view.rfind(L"[OK]",    0) == 0) return LogOk;
        if (view.rfind(L"[PASS]",  0) == 0) return LogOk;
        if (view.rfind(L"[WARN]",  0) == 0) return LogWarn;
        if (view.rfind(L"[ERROR]", 0) == 0) return LogError;
        if (view.rfind(L"[FAIL]",  0) == 0) return LogError;
        if (view.rfind(L"[INFO]",  0) == 0) return LogInfo;
        if (view.find(L"\x2588") != std::wstring::npos ||   // █
            view.find(L"\x2587") != std::wstring::npos ||
            view.find(L"\x2586") != std::wstring::npos ||
            view.find(L"\x2585") != std::wstring::npos ||
            view.find(L"\x2584") != std::wstring::npos ||
            view.find(L"\x2583") != std::wstring::npos ||
            view.find(L"\x2582") != std::wstring::npos ||
            view.find(L"\x2581") != std::wstring::npos) {
            return LogGraphBar;
        }
        if (view.find(L"\x2500") != std::wstring::npos ||   // ─
            view.find(L"\x2502") != std::wstring::npos ||
            view.find(L"\x2514") != std::wstring::npos ||
            view.find(L"\x2524") != std::wstring::npos ||
            view.find(L"\x2192") != std::wstring::npos) {
            return LogGraphFrame;
        }
        if (view.rfind(L"  ", 0) == 0) return LogDim;
        return LogDefault;
    }

    // ------------------------------------------------------------------------
    // Per-control state. Stored via GWLP_USERDATA so it's reachable from the
    // wndproc by HWND alone.
    // ------------------------------------------------------------------------
    struct Segment {
        std::wstring text;
        COLORREF color;
        bool isExplicit;
    };
    struct Line {
        std::vector<Segment> segments;
        int charCount = 0;             // total characters across all segments
        int visualLines = 1;           // wrapped rows this line occupies
        COLORREF heuristicColor = LogDefault;
    };

    struct State {
        HWND hwnd = nullptr;
        HFONT font = nullptr;
        // Stack of fallback fonts. Each is tried in order for glyphs the
        // primary (and earlier fallbacks) don't have. The first one Segoe UI
        // covers general punctuation (em dash etc.) and most BMP symbols;
        // Segoe UI Symbol covers ℹ ♫ etc.; Segoe UI Emoji covers astral-plane
        // emoji like 💿.
        static constexpr int kFallbackCount = 3;
        HFONT fallbackFonts[kFallbackCount] = { nullptr, nullptr, nullptr };
        Gdiplus::Image* backgroundImage = nullptr;

        std::vector<Line> closedLines;
        Line currentLine;            // line still being built (no \n yet)

        int lineHeight = 16;
        int charWidth  = 8;          // monospace cell width (computed on font/size)
        int maxCharsPerLine = 1;     // wrap column derived from client width
        int totalVisualLines = 0;    // sum of all logical lines' visualLines
        int topLine = 0;             // index of first visible visual line
        bool followBottom = true;

        // Line-based selection. Indices into closedLines (plus optional
        // currentLine at index closedLines.size()). -1 = no selection.
        int  selectionAnchor = -1;
        int  selectionCaret  = -1;
        bool selecting       = false;  // left-button drag in progress

        // Cached background composition (sized to client area).
        HDC     bgDc        = nullptr;
        HBITMAP bgBitmap    = nullptr;
        HBITMAP bgOldBitmap = nullptr;
        // Back buffer used for atomic paint.
        HDC     backDc        = nullptr;
        HBITMAP backBitmap    = nullptr;
        HBITMAP backOldBitmap = nullptr;
        int cacheWidth  = 0;
        int cacheHeight = 0;
        BYTE backgroundToneAlpha = 84;
        BYTE backgroundGridAlpha = 22;
    };

    State* GetState(HWND h) {
        return reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    void SetState(HWND h, State* s) {
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }

    int LogicalLineCount(const State& s) {
        return (int)s.closedLines.size()
             + (s.currentLine.segments.empty() ? 0 : 1);
    }

    int VisibleLineCount(const State& s) {
        // Total visual (post-wrap) rows.
        return s.totalVisualLines;
    }

    const Line* LineAt(const State& s, int idx) {
        const int closed = (int)s.closedLines.size();
        if (idx < 0) return nullptr;
        if (idx < closed) return &s.closedLines[idx];
        if (idx == closed && !s.currentLine.segments.empty()) return &s.currentLine;
        return nullptr;
    }

    int ComputeVisualLines(int charCount, int maxCharsPerLine) {
        if (maxCharsPerLine <= 0) return 1;
        if (charCount <= 0) return 1;
        return (charCount + maxCharsPerLine - 1) / maxCharsPerLine;
    }

    int LineContribution(const Line& line, bool isCurrent, int maxChars) {
        // A closed empty line shows as one blank row; an empty currentLine
        // contributes nothing (just the trailing caret position).
        if (line.segments.empty()) {
            return isCurrent ? 0 : 1;
        }
        return ComputeVisualLines(line.charCount, maxChars);
    }

    std::wstring JoinLine(const Line& line) {
        std::wstring out;
        out.reserve((size_t)line.charCount);
        for (const auto& seg : line.segments) out.append(seg.text);
        return out;
    }

    // Recompute per-line cached values (visualLines, heuristicColor).
    // The closed-line versions are otherwise frozen at commit time.
    void RecacheLine(Line& line, int maxChars) {
        line.charCount = 0;
        for (auto& seg : line.segments) line.charCount += (int)seg.text.size();
        line.visualLines = ComputeVisualLines(line.charCount, maxChars);
        line.heuristicColor = HeuristicColor(JoinLine(line));
    }

    void RelayoutAllLines(State& s) {
        int total = 0;
        for (auto& line : s.closedLines) {
            line.visualLines = (line.segments.empty())
                ? 1
                : ComputeVisualLines(line.charCount, s.maxCharsPerLine);
            total += line.visualLines;
        }
        if (!s.currentLine.segments.empty()) {
            s.currentLine.visualLines =
                ComputeVisualLines(s.currentLine.charCount, s.maxCharsPerLine);
            total += s.currentLine.visualLines;
        } else {
            s.currentLine.visualLines = 0;
        }
        s.totalVisualLines = total;
    }

    void ComputeMetrics(State& s, HDC referenceDc) {
        if (!referenceDc || !s.font) return;

        HFONT old = (HFONT)SelectObject(referenceDc, s.font);
        SIZE sz{};
        GetTextExtentPoint32W(referenceDc, L"M", 1, &sz);
        TEXTMETRICW tm{};
        GetTextMetricsW(referenceDc, &tm);
        if (old) SelectObject(referenceDc, old);

        s.charWidth = std::max(1, (int)sz.cx);
        s.lineHeight = std::max(1, (int)(tm.tmHeight + tm.tmExternalLeading + 2));

        RECT rc;
        GetClientRect(s.hwnd, &rc);
        const int leftMargin = 18;
        const int rightMargin = 8;
        const int textWidth = std::max(1, (int)(rc.right - rc.left)
                                          - leftMargin - rightMargin);
        s.maxCharsPerLine = std::max(1, textWidth / s.charWidth);
    }

    // Build (or rebuild) the fallback font stack from the primary's LOGFONT.
    // Order matters: each char missing in earlier fonts gets routed to the
    // first later font that has it.
    void EnsureFallbackFonts(State& s) {
        if (!s.font) return;
        const wchar_t* faces[State::kFallbackCount] = {
            L"Segoe UI",          // General Punctuation (em dash etc.), most BMP
            L"Segoe UI Symbol",   // ℹ ♫ ⚠ and assorted symbol-block chars
            L"Segoe UI Emoji"     // astral-plane emoji (💿 …)
        };
        LOGFONTW lf{};
        if (GetObjectW(s.font, sizeof(lf), &lf) == 0) return;
        for (int i = 0; i < State::kFallbackCount; ++i) {
            if (s.fallbackFonts[i]) continue;
            wcscpy_s(lf.lfFaceName, faces[i]);
            s.fallbackFonts[i] = CreateFontIndirectW(&lf);
        }
    }

    void ReleaseFallbackFonts(State& s) {
        for (int i = 0; i < State::kFallbackCount; ++i) {
            if (s.fallbackFonts[i]) {
                DeleteObject(s.fallbackFonts[i]);
                s.fallbackFonts[i] = nullptr;
            }
        }
    }

    // Draw a run of text at the cell grid (x, y). Each character occupies
    // exactly `cellWidth` pixels via ExtTextOut's dx[] array, so monospace
    // alignment survives even when chars are drawn with a fallback font
    // whose natural advances differ from Consolas.
    //
    // For each char we pick the first font in [primary, fallbacks...] that
    // has a glyph for it. Adjacent chars assigned to the same font are
    // batched into one ExtTextOut call.
    //
    // `fontIndex` of -1 means primary; 0..N-1 indexes into `fallbacks`.
    void DrawCellsWithFallback(HDC hdc, int x, int y,
                               const wchar_t* text, int len, int cellWidth,
                               HFONT primary, HFONT* fallbacks, int numFallbacks) {
        if (len <= 0) return;

        std::vector<int> fontIndex((size_t)len, -1);  // start: assume primary

        // First pass — mark which chars are missing from primary.
        std::vector<WORD> idxBuf((size_t)len);
        DWORD got = GetGlyphIndicesW(hdc, text, len, idxBuf.data(),
                                     GGI_MARK_NONEXISTING_GLYPHS);
        if (got != GDI_ERROR) {
            for (int i = 0; i < len; ++i) {
                if (idxBuf[i] == 0xFFFF) fontIndex[i] = -2;  // unresolved
            }
        }

        // Walk fallbacks in order; resolve each unresolved char to the first
        // font that has it.
        for (int f = 0; f < numFallbacks; ++f) {
            if (!fallbacks[f]) continue;

            // Collect unresolved positions for this round.
            std::vector<int>     pos;
            std::vector<wchar_t> chars;
            pos.reserve((size_t)len);
            chars.reserve((size_t)len);
            for (int i = 0; i < len; ++i) {
                if (fontIndex[i] == -2) {
                    pos.push_back(i);
                    chars.push_back(text[i]);
                }
            }
            if (chars.empty()) break;

            HFONT prev = (HFONT)SelectObject(hdc, fallbacks[f]);
            std::vector<WORD> fb(chars.size());
            DWORD r = GetGlyphIndicesW(hdc, chars.data(), (int)chars.size(),
                                       fb.data(), GGI_MARK_NONEXISTING_GLYPHS);
            if (prev) SelectObject(hdc, prev);
            if (r == GDI_ERROR) continue;

            for (size_t i = 0; i < chars.size(); ++i) {
                if (fb[i] != 0xFFFF) fontIndex[pos[i]] = f;
            }
        }

        // Anything still unresolved gets routed to the last fallback so the
        // user sees that font's notdef glyph instead of the primary's. Falls
        // back to primary if there are no fallbacks at all.
        for (int i = 0; i < len; ++i) {
            if (fontIndex[i] == -2) {
                int lastIdx = -1;
                for (int f = numFallbacks - 1; f >= 0; --f) {
                    if (fallbacks[f]) { lastIdx = f; break; }
                }
                fontIndex[i] = lastIdx;
            }
        }

        // Group consecutive same-font chars and draw each run with one
        // ExtTextOut, forcing cellWidth advances per char.
        std::vector<INT> dx((size_t)len, cellWidth);
        int runStart = 0;
        int runFont = fontIndex[0];
        auto flushRun = [&](int end) {
            const int runLen = end - runStart;
            if (runLen <= 0) return;
            HFONT toUse = (runFont == -1) ? primary : fallbacks[runFont];
            if (!toUse) toUse = primary;
            HFONT prev = (HFONT)SelectObject(hdc, toUse);
            ExtTextOutW(hdc, x + runStart * cellWidth, y, 0, nullptr,
                        text + runStart, runLen, dx.data());
            if (prev) SelectObject(hdc, prev);
        };
        for (int i = 1; i < len; ++i) {
            if (fontIndex[i] != runFont) {
                flushRun(i);
                runStart = i;
                runFont = fontIndex[i];
            }
        }
        flushRun(len);
    }

    // ------------------------------------------------------------------------
    // Cache management.
    // ------------------------------------------------------------------------
    void ReleaseCaches(State& s) {
        if (s.bgDc) {
            if (s.bgOldBitmap) SelectObject(s.bgDc, s.bgOldBitmap);
            DeleteDC(s.bgDc);
            s.bgDc = nullptr;
            s.bgOldBitmap = nullptr;
        }
        if (s.bgBitmap) { DeleteObject(s.bgBitmap); s.bgBitmap = nullptr; }
        if (s.backDc) {
            if (s.backOldBitmap) SelectObject(s.backDc, s.backOldBitmap);
            DeleteDC(s.backDc);
            s.backDc = nullptr;
            s.backOldBitmap = nullptr;
        }
        if (s.backBitmap) { DeleteObject(s.backBitmap); s.backBitmap = nullptr; }
        s.cacheWidth = 0;
        s.cacheHeight = 0;
    }

    void RenderBackgroundToCache(State& s, int width, int height) {
        if (!s.bgDc) return;
        Gdiplus::Graphics graphics(s.bgDc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // Solid themed dark surface for the log text (the procedural artwork
        // lives on the main window; the log panel stays clean for legibility).
        // A faint grid keeps a hint of texture.
        Gdiplus::SolidBrush baseBrush(Gdiplus::Color(255,
            GetRValue(PanelDark), GetGValue(PanelDark), GetBValue(PanelDark)));
        graphics.FillRectangle(&baseBrush, 0, 0, width, height);

        Gdiplus::Pen faintLine(Gdiplus::Color(s.backgroundGridAlpha,
            GetRValue(LogGraphFrame), GetGValue(LogGraphFrame), GetBValue(LogGraphFrame)), 1.0f);
        for (int y = 28; y < height; y += 28) {
            graphics.DrawLine(&faintLine, 0, y, width, y);
        }
    }

    void EnsureCaches(State& s, HDC referenceDc, int width, int height) {
        if (s.cacheWidth == width && s.cacheHeight == height && s.bgDc && s.backDc) {
            return;
        }
        ReleaseCaches(s);

        s.bgDc = CreateCompatibleDC(referenceDc);
        s.bgBitmap = CreateCompatibleBitmap(referenceDc, width, height);
        if (s.bgDc && s.bgBitmap) {
            s.bgOldBitmap = (HBITMAP)SelectObject(s.bgDc, s.bgBitmap);
            RenderBackgroundToCache(s, width, height);
        }

        s.backDc = CreateCompatibleDC(referenceDc);
        s.backBitmap = CreateCompatibleBitmap(referenceDc, width, height);
        if (s.backDc && s.backBitmap) {
            s.backOldBitmap = (HBITMAP)SelectObject(s.backDc, s.backBitmap);
        }

        s.cacheWidth = width;
        s.cacheHeight = height;
    }

    int ViewportLineCapacity(const State& s) {
        RECT rc;
        GetClientRect(s.hwnd, &rc);
        const int viewH = std::max(1, (int)(rc.bottom - rc.top));
        return std::max(1, viewH / std::max(1, s.lineHeight));
    }

    void UpdateScrollbar(State& s) {
        const int total = VisibleLineCount(s);
        const int page  = ViewportLineCapacity(s);

        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
        si.nMin   = 0;
        si.nMax   = std::max(0, total - 1);
        si.nPage  = (UINT)std::min(page, std::max(1, total));
        si.nPos   = s.topLine;
        SetScrollInfo(s.hwnd, SB_VERT, &si, TRUE);
    }

    void ClampScroll(State& s) {
        const int total = VisibleLineCount(s);
        const int page  = ViewportLineCapacity(s);
        const int maxTop = std::max(0, total - page);
        if (s.topLine > maxTop) s.topLine = maxTop;
        if (s.topLine < 0)      s.topLine = 0;
    }

    void ScrollToBottom(State& s) {
        const int total = VisibleLineCount(s);
        const int page  = ViewportLineCapacity(s);
        s.topLine = std::max(0, total - page);
    }

    void Repaint(State& s) {
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }

    // ------------------------------------------------------------------------
    // Selection helpers.
    // ------------------------------------------------------------------------
    bool HasSelection(const State& s) {
        return s.selectionAnchor >= 0 && s.selectionCaret >= 0;
    }

    void SelectionRange(const State& s, int& start, int& end) {
        start = std::min(s.selectionAnchor, s.selectionCaret);
        end   = std::max(s.selectionAnchor, s.selectionCaret);
    }

    // Map mouse-y to a logical line index. Clamps above content to 0 and
    // below content to the last logical line so drags off-edge still extend
    // the selection in the natural direction.
    int VisualYToLogicalLine(const State& s, int y) {
        const int logicalCount = LogicalLineCount(s);
        if (logicalCount <= 0) return -1;

        const int topMargin = 4;
        const int lineH = std::max(1, s.lineHeight);
        int visualIdx;
        if (y < topMargin) {
            visualIdx = s.topLine;          // snap to first visible row
        } else {
            visualIdx = s.topLine + (y - topMargin) / lineH;
        }
        if (visualIdx < 0) return 0;

        int accum = 0;
        for (int i = 0; i < logicalCount; ++i) {
            const Line* line = LineAt(s, i);
            if (!line) break;
            const int lv = line->segments.empty() ? 1 : line->visualLines;
            if (visualIdx < accum + lv) return i;
            accum += lv;
        }
        return logicalCount - 1;  // past end
    }

    std::wstring BuildSelectedText(const State& s) {
        if (!HasSelection(s)) return {};
        int a, b; SelectionRange(s, a, b);
        std::wstring out;
        for (int i = a; i <= b; ++i) {
            const Line* line = LineAt(s, i);
            if (line) out.append(JoinLine(*line));
            if (i < b) out.append(L"\r\n");
        }
        return out;
    }

    std::wstring BuildAllText(const State& s) {
        const int n = LogicalLineCount(s);
        std::wstring out;
        for (int i = 0; i < n; ++i) {
            const Line* line = LineAt(s, i);
            if (line) out.append(JoinLine(*line));
            if (i < n - 1) out.append(L"\r\n");
        }
        return out;
    }

    void CopyTextToClipboard(HWND owner, const std::wstring& text) {
        if (text.empty()) return;
        if (!OpenClipboard(owner)) return;
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem) {
            void* p = GlobalLock(hMem);
            if (p) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(hMem);
                if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
                    GlobalFree(hMem);
                }
            } else {
                GlobalFree(hMem);
            }
        }
        CloseClipboard();
    }

    void SelectAll(State& s) {
        const int n = LogicalLineCount(s);
        if (n <= 0) {
            s.selectionAnchor = s.selectionCaret = -1;
        } else {
            s.selectionAnchor = 0;
            s.selectionCaret = n - 1;
            s.followBottom = false;
        }
        Repaint(s);
    }

    void ClearSelection(State& s) {
        if (HasSelection(s)) {
            s.selectionAnchor = s.selectionCaret = -1;
            Repaint(s);
        }
    }

    void ShowContextMenu(State& s, int screenX, int screenY) {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        const UINT copyFlags = HasSelection(s) ? MF_STRING : (MF_STRING | MF_GRAYED);
        AppendMenuW(menu, copyFlags, IDM_COPY, L"Copy\tCtrl+C");
        AppendMenuW(menu, MF_STRING, IDM_COPY_ALL, L"Copy All");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_SELECT_ALL, L"Select All\tCtrl+A");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenX, screenY, 0, s.hwnd, nullptr);
        DestroyMenu(menu);
    }

    // ------------------------------------------------------------------------
    // Painting.
    // ------------------------------------------------------------------------
    void Paint(State& s, HDC hdc) {
        RECT rc;
        GetClientRect(s.hwnd, &rc);
        const int width  = std::max(1, (int)(rc.right - rc.left));
        const int height = std::max(1, (int)(rc.bottom - rc.top));

        EnsureCaches(s, hdc, width, height);

        HDC paintDc = s.backDc ? s.backDc : hdc;

        if (s.bgDc && paintDc != hdc) {
            BitBlt(paintDc, 0, 0, width, height, s.bgDc, 0, 0, SRCCOPY);
        } else {
            // Fallback: paint directly. Won't be atomic but keeps the
            // control usable if a compatible DC couldn't be created.
            RECT fillRc{ 0, 0, width, height };
            HBRUSH solid = CreateSolidBrush(PanelDark);
            FillRect(paintDc, &fillRc, solid);
            DeleteObject(solid);
        }

        HFONT oldFont = nullptr;
        if (s.font) oldFont = (HFONT)SelectObject(paintDc, s.font);
        SetBkMode(paintDc, TRANSPARENT);
        EnsureFallbackFonts(s);

        const int leftMargin = 18;
        const int topMargin  = 4;
        const int totalVisual = VisibleLineCount(s);
        const int pageVisual  = ViewportLineCapacity(s) + 1;
        const int lastVisual  = std::min(totalVisual, s.topLine + pageVisual);
        const int maxCharsPerLine = std::max(1, s.maxCharsPerLine);
        const int charWidth = std::max(1, s.charWidth);

        // Find which logical line contains the topVisual line, and how far
        // into it we start.
        int skip = s.topLine;
        int logicalIdx = 0;
        int charOffset = 0;                     // start char within current logical line
        const int logicalCount = LogicalLineCount(s);
        while (logicalIdx < logicalCount) {
            const Line* line = LineAt(s, logicalIdx);
            if (!line) break;
            const int lv = line->segments.empty() ? 1 : line->visualLines;
            if (skip < lv) {
                charOffset = skip * maxCharsPerLine;
                break;
            }
            skip -= lv;
            logicalIdx++;
        }

        // Selection range (line indices, inclusive). -1 -> none.
        int selStart = -1, selEnd = -1;
        if (HasSelection(s)) SelectionRange(s, selStart, selEnd);

        // Render visual lines from (logicalIdx, charOffset).
        int visualRowsRendered = 0;
        const int visualRowsToRender = lastVisual - s.topLine;
        while (visualRowsRendered < visualRowsToRender &&
               logicalIdx < logicalCount) {
            const Line* line = LineAt(s, logicalIdx);
            if (!line) break;

            const int y = topMargin + visualRowsRendered * s.lineHeight;
            const COLORREF heuristic = line->heuristicColor;

            // Selection highlight band for this visual row.
            if (selStart >= 0 && logicalIdx >= selStart && logicalIdx <= selEnd) {
                RECT hl{ 0, y, width, y + s.lineHeight };
                HBRUSH br = CreateSolidBrush(SelectionBg);
                FillRect(paintDc, &hl, br);
                DeleteObject(br);
            }

            // Visible character window for this visual line.
            const int rowStartChar = charOffset;
            const int rowEndChar   = charOffset + maxCharsPerLine;

            // Walk segments, drawing any portion that falls in this window.
            int segOffset = 0;
            for (const auto& seg : line->segments) {
                const int segLen = (int)seg.text.size();
                if (segLen == 0) continue;
                const int segStart = segOffset;
                const int segEnd   = segOffset + segLen;
                segOffset = segEnd;

                if (segEnd <= rowStartChar) continue;
                if (segStart >= rowEndChar) break;

                const int drawStart = std::max(segStart, rowStartChar);
                const int drawEnd   = std::min(segEnd,   rowEndChar);
                if (drawEnd <= drawStart) continue;

                const wchar_t* sub = seg.text.c_str() + (drawStart - segStart);
                const int subLen = drawEnd - drawStart;
                const int x = leftMargin + (drawStart - rowStartChar) * charWidth;

                const COLORREF c = seg.isExplicit ? seg.color : heuristic;
                SetTextColor(paintDc, c);
                DrawCellsWithFallback(paintDc, x, y, sub, subLen,
                                      charWidth, s.font,
                                      s.fallbackFonts, State::kFallbackCount);
            }

            visualRowsRendered++;

            // Advance to the next visual line — either still within this
            // logical line, or move on to the next.
            charOffset += maxCharsPerLine;
            if (charOffset >= std::max(1, line->charCount)) {
                charOffset = 0;
                logicalIdx++;
            }
        }

        if (oldFont) SelectObject(paintDc, oldFont);

        if (paintDc != hdc) {
            BitBlt(hdc, 0, 0, width, height, paintDc, 0, 0, SRCCOPY);
        }
    }

    // ------------------------------------------------------------------------
    // Scroll handlers.
    // ------------------------------------------------------------------------
    void ApplyTopLine(State& s, int newTop) {
        const int total = VisibleLineCount(s);
        const int page  = ViewportLineCapacity(s);
        const int maxTop = std::max(0, total - page);
        newTop = std::clamp(newTop, 0, maxTop);
        if (newTop == s.topLine) {
            UpdateScrollbar(s);
            return;
        }
        s.topLine = newTop;
        s.followBottom = (newTop >= maxTop);
        UpdateScrollbar(s);
        Repaint(s);
    }

    void HandleVScroll(State& s, WORD code) {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_ALL;
        GetScrollInfo(s.hwnd, SB_VERT, &si);

        int newTop = s.topLine;
        switch (code) {
        case SB_LINEUP:        newTop -= 1;             break;
        case SB_LINEDOWN:      newTop += 1;             break;
        case SB_PAGEUP:        newTop -= (int)si.nPage; break;
        case SB_PAGEDOWN:      newTop += (int)si.nPage; break;
        case SB_TOP:           newTop = 0;              break;
        case SB_BOTTOM:        newTop = INT_MAX;        break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: newTop = si.nTrackPos;   break;
        default: return;
        }
        ApplyTopLine(s, newTop);
    }

    void HandleMouseWheel(State& s, short delta) {
        UINT scrollLines = 3;
        SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &scrollLines, 0);
        if (scrollLines == WHEEL_PAGESCROLL) {
            scrollLines = (UINT)ViewportLineCapacity(s);
        }
        // delta > 0 → wheel forward → scroll up (decrease topLine).
        const int step = -(int)delta * (int)scrollLines / WHEEL_DELTA;
        ApplyTopLine(s, s.topLine + step);
    }

    // ------------------------------------------------------------------------
    // Append.
    // ------------------------------------------------------------------------
    void AppendImpl(State& s, const wchar_t* text, size_t len,
                    COLORREF color, bool isExplicit) {
        if (!text || len == 0) return;

        // Track how much currentLine was contributing to the visual total
        // before this call so we can apply a single delta at the end.
        int oldCurrentContribution =
            s.currentLine.segments.empty()
                ? 0
                : s.currentLine.visualLines;
        int totalDelta = 0;

        auto pushToCurrent = [&](const wchar_t* p, size_t n) {
            if (n == 0) return;
            auto& segs = s.currentLine.segments;
            if (!segs.empty() &&
                segs.back().color == color &&
                segs.back().isExplicit == isExplicit) {
                segs.back().text.append(p, n);
            } else {
                Segment seg;
                seg.text.assign(p, n);
                seg.color = color;
                seg.isExplicit = isExplicit;
                segs.push_back(std::move(seg));
            }
            s.currentLine.charCount += (int)n;
        };

        size_t i = 0;
        while (i < len) {
            // Skip \r (treat \r\n as \n; bare \r is ignored — progress lines
            // are stripped upstream by GuiSink).
            if (text[i] == L'\r') { ++i; continue; }

            size_t segStart = i;
            while (i < len && text[i] != L'\n' && text[i] != L'\r') ++i;
            if (i > segStart) {
                pushToCurrent(text + segStart, i - segStart);
            }

            if (i < len && text[i] == L'\n') {
                // Commit currentLine. Freeze its visual count + heuristic
                // colour. Empty committed lines display as one blank row.
                const bool wasEmpty = s.currentLine.segments.empty();
                if (wasEmpty) {
                    s.currentLine.visualLines = 1;
                    s.currentLine.heuristicColor = LogDefault;
                } else {
                    s.currentLine.visualLines = ComputeVisualLines(
                        s.currentLine.charCount, s.maxCharsPerLine);
                    s.currentLine.heuristicColor =
                        HeuristicColor(JoinLine(s.currentLine));
                }

                const int contributionGained = s.currentLine.visualLines;
                totalDelta += contributionGained - oldCurrentContribution;
                oldCurrentContribution = 0;   // new currentLine starts empty

                s.closedLines.push_back(std::move(s.currentLine));
                s.currentLine = Line{};
                s.currentLine.visualLines = 0;   // empty currentLine: no count

                ++i;
            }
        }

        // Re-evaluate the currentLine's final contribution after this batch.
        int newCurrentContribution = 0;
        if (!s.currentLine.segments.empty()) {
            s.currentLine.visualLines = ComputeVisualLines(
                s.currentLine.charCount, s.maxCharsPerLine);
            s.currentLine.heuristicColor =
                HeuristicColor(JoinLine(s.currentLine));
            newCurrentContribution = s.currentLine.visualLines;
        }
        totalDelta += newCurrentContribution - oldCurrentContribution;
        s.totalVisualLines += totalDelta;

        if (s.followBottom) ScrollToBottom(s);
        else                ClampScroll(s);
        UpdateScrollbar(s);
        Repaint(s);
    }

    // ------------------------------------------------------------------------
    // Window procedure.
    // ------------------------------------------------------------------------
    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_NCCREATE: {
            auto* s = new State();
            s->hwnd = hwnd;
            SetState(hwnd, s);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_NCDESTROY: {
            State* s = GetState(hwnd);
            if (s) {
                KillTimer(hwnd, kAutoScrollTimer);
                ReleaseCaches(*s);
                ReleaseFallbackFonts(*s);
                delete s;
                SetState(hwnd, nullptr);
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            State* s = GetState(hwnd);
            if (s) Paint(*s, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE: {
            State* s = GetState(hwnd);
            if (s) {
                HDC hdc = GetDC(hwnd);
                if (hdc) {
                    ComputeMetrics(*s, hdc);
                    ReleaseDC(hwnd, hdc);
                }
                ReleaseCaches(*s);
                RelayoutAllLines(*s);
                if (s->followBottom) ScrollToBottom(*s);
                else                 ClampScroll(*s);
                UpdateScrollbar(*s);
                Repaint(*s);
            }
            return 0;
        }
        case WM_VSCROLL: {
            State* s = GetState(hwnd);
            if (s) HandleVScroll(*s, LOWORD(wParam));
            return 0;
        }
        case WM_MOUSEWHEEL: {
            State* s = GetState(hwnd);
            if (s) HandleMouseWheel(*s, GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }
        case WM_LBUTTONDOWN: {
            State* s = GetState(hwnd);
            if (!s) return 0;
            SetFocus(hwnd);
            const int y = GET_Y_LPARAM(lParam);
            const int line = VisualYToLogicalLine(*s, y);
            if (line < 0) { ClearSelection(*s); return 0; }
            if ((wParam & MK_SHIFT) && s->selectionAnchor >= 0) {
                s->selectionCaret = line;
            } else {
                s->selectionAnchor = line;
                s->selectionCaret  = line;
            }
            s->selecting = true;
            // Pause auto-follow while the user is selecting so streamed output
            // doesn't scroll the highlighted lines out from under them.
            s->followBottom = false;
            SetCapture(hwnd);
            Repaint(*s);
            return 0;
        }
        case WM_MOUSEMOVE: {
            State* s = GetState(hwnd);
            if (!s || !s->selecting) return 0;
            const int y = GET_Y_LPARAM(lParam);
            RECT rc; GetClientRect(hwnd, &rc);
            if (y < 0 || y >= rc.bottom) {
                // Off-edge: let the timer scroll + extend selection one row
                // at a time. SetTimer with the same id resets the existing
                // timer rather than creating a second one.
                SetTimer(hwnd, kAutoScrollTimer, kAutoScrollPeriodMs, nullptr);
            } else {
                KillTimer(hwnd, kAutoScrollTimer);
                const int line = VisualYToLogicalLine(*s, y);
                if (line >= 0 && line != s->selectionCaret) {
                    s->selectionCaret = line;
                    Repaint(*s);
                }
            }
            return 0;
        }
        case WM_TIMER: {
            if (wParam != kAutoScrollTimer) break;
            State* s = GetState(hwnd);
            if (!s || !s->selecting) {
                KillTimer(hwnd, kAutoScrollTimer);
                return 0;
            }
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            RECT rc; GetClientRect(hwnd, &rc);
            int direction = 0;
            if (pt.y < 0)            direction = -1;
            else if (pt.y >= rc.bottom) direction = +1;
            if (direction == 0) {
                KillTimer(hwnd, kAutoScrollTimer);
                return 0;
            }
            ApplyTopLine(*s, s->topLine + direction);
            // Extend selection to the row now at the leading edge of the drag.
            const int edgeY = (direction < 0) ? 0 : rc.bottom - 1;
            const int line = VisualYToLogicalLine(*s, edgeY);
            if (line >= 0 && line != s->selectionCaret) {
                s->selectionCaret = line;
                Repaint(*s);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            State* s = GetState(hwnd);
            if (!s) return 0;
            if (s->selecting) {
                s->selecting = false;
                KillTimer(hwnd, kAutoScrollTimer);
                ReleaseCapture();
            }
            return 0;
        }
        case WM_CAPTURECHANGED: {
            State* s = GetState(hwnd);
            if (s) {
                s->selecting = false;
                KillTimer(hwnd, kAutoScrollTimer);
            }
            return 0;
        }
        case WM_CONTEXTMENU: {
            State* s = GetState(hwnd);
            if (!s) return 0;
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            // lParam = -1 means the menu came from the keyboard (Shift+F10).
            if (x == -1 && y == -1) {
                POINT pt{ 8, 8 };
                ClientToScreen(hwnd, &pt);
                x = pt.x; y = pt.y;
            }
            ShowContextMenu(*s, x, y);
            return 0;
        }
        case WM_COMMAND: {
            State* s = GetState(hwnd);
            if (!s) return 0;
            switch (LOWORD(wParam)) {
            case IDM_COPY:
                CopyTextToClipboard(hwnd, BuildSelectedText(*s));
                return 0;
            case IDM_COPY_ALL:
                CopyTextToClipboard(hwnd, BuildAllText(*s));
                return 0;
            case IDM_SELECT_ALL:
                SelectAll(*s);
                return 0;
            }
            return 0;
        }
        case WM_KEYDOWN: {
            State* s = GetState(hwnd);
            if (!s) return 0;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl && (wParam == 'C' || wParam == VK_INSERT)) {
                CopyTextToClipboard(hwnd, BuildSelectedText(*s));
                return 0;
            }
            if (ctrl && wParam == 'A') {
                SelectAll(*s);
                return 0;
            }
            break;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

}  // namespace

namespace OutputControl {

    const wchar_t* kClassName = L"OptiScanOutputControl";

    void RegisterClass(HINSTANCE hInstance) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInstance;
        wc.hCursor       = LoadCursorW(nullptr, IDC_IBEAM);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
    }

    HWND Create(HWND parent, int id, HINSTANCE hInstance) {
        return CreateWindowExW(
            0,
            kClassName,
            L"",
            WS_CHILD | WS_VSCROLL,
            0, 0, 0, 0,
            parent,
            (HMENU)(INT_PTR)id,
            hInstance,
            nullptr);
    }

    void SetFont(HWND hCtrl, HFONT font) {
        State* s = GetState(hCtrl);
        if (!s) return;
        s->font = font;
        // Force the fallback fonts to be rebuilt against the new primary's
        // size on the next paint.
        ReleaseFallbackFonts(*s);
        HDC hdc = GetDC(hCtrl);
        if (hdc) {
            ComputeMetrics(*s, hdc);
            ReleaseDC(hCtrl, hdc);
        }
        RelayoutAllLines(*s);
        if (s->followBottom) ScrollToBottom(*s);
        else                 ClampScroll(*s);
        UpdateScrollbar(*s);
        Repaint(*s);
    }

    void SetBackgroundImage(HWND hCtrl, Gdiplus::Image* image) {
        State* s = GetState(hCtrl);
        if (!s) return;
        s->backgroundImage = image;
        ReleaseCaches(*s);
        Repaint(*s);
    }

    void SetBackgroundTone(HWND hCtrl, BYTE toneAlpha, BYTE gridAlpha) {
        State* s = GetState(hCtrl);
        if (!s) return;
        if (s->backgroundToneAlpha == toneAlpha &&
            s->backgroundGridAlpha == gridAlpha) {
            return;
        }
        s->backgroundToneAlpha = toneAlpha;
        s->backgroundGridAlpha = gridAlpha;
        ReleaseCaches(*s);
        Repaint(*s);
    }

    void Append(HWND hCtrl, const wchar_t* text, size_t len,
                COLORREF color, bool isExplicit) {
        State* s = GetState(hCtrl);
        if (!s) return;
        AppendImpl(*s, text, len, color, isExplicit);
    }

    void Append(HWND hCtrl, const wchar_t* text,
                COLORREF color, bool isExplicit) {
        if (!text) return;
        size_t len = 0;
        while (text[len]) ++len;
        Append(hCtrl, text, len, color, isExplicit);
    }

    void Clear(HWND hCtrl) {
        State* s = GetState(hCtrl);
        if (!s) return;
        s->closedLines.clear();
        s->currentLine = Line{};
        s->currentLine.visualLines = 0;
        s->totalVisualLines = 0;
        s->topLine = 0;
        s->followBottom = true;
        s->selectionAnchor = s->selectionCaret = -1;
        s->selecting = false;
        UpdateScrollbar(*s);
        Repaint(*s);
    }

    void ApplyTheme(HWND hCtrl) {
        // Re-seed the per-line colour roles from the active theme. Segments
        // without an explicit SGR colour re-derive at paint time via the
        // heuristic, so a repaint re-colours the bulk of existing output.
        const Palette& p = ActiveTheme();
        const bool professionalLight = CurrentThemeId() == ThemeId::AppleLight;
        LogDefault    = professionalLight ? RGB(210, 217, 226) : p.fg;
        LogDim        = professionalLight ? RGB(139, 150, 165) : p.dim;
        LogHeading    = professionalLight ? RGB(245, 247, 250) : p.bright;
        LogCommand    = professionalLight ? RGB( 86, 156, 255) : p.accentWarm;
        LogOk         = p.ok;
        LogWarn       = p.warn;
        LogError      = p.error;
        LogInfo       = professionalLight ? RGB(126, 189, 255) : p.bright;
        LogGraphFrame = professionalLight ? RGB(139, 150, 165) : p.graphFrame;
        LogGraphBar   = professionalLight ? RGB( 86, 156, 255) : p.graphBar;
        PanelDark     = p.outputBg;
        SelectionBg   = professionalLight ? RGB( 42,  65,  96) : p.selection;
        // Force the cached background (base fill + tinted artwork + veil) to
        // re-render with the new theme, then repaint.
        if (State* s = hCtrl ? GetState(hCtrl) : nullptr) {
            ReleaseCaches(*s);
            Repaint(*s);
        }
    }

}  // namespace OutputControl
