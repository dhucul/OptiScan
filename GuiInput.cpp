#define NOMINMAX
#include "GuiInput.h"
#include "FontLoader.h"
#include "Theme.h"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <atomic>
#include <functional>
#include <optional>
#include <string>

namespace {

    std::atomic<HWND> g_hOwner{ nullptr };
    constexpr UINT kInvokeOnOwnerMessage = WM_APP + 0x3C0;

    struct OwnerInvocation {
        void (*run)(void*) = nullptr;
        void* context = nullptr;
    };

    template<typename Action>
    auto RunOnOwnerThread(Action&& action) -> decltype(action()) {
        using Result = decltype(action());
        HWND owner = g_hOwner.load(std::memory_order_acquire);
        DWORD ownerThread = owner ? GetWindowThreadProcessId(owner, nullptr) : 0;
        if (!owner || ownerThread == GetCurrentThreadId()) return action();

        std::optional<Result> value;
        auto thunk = [&]() { value.emplace(action()); };
        OwnerInvocation request{
            [](void* p) { (*static_cast<decltype(thunk)*>(p))(); }, &thunk
        };
        SendMessageW(owner, kInvokeOnOwnerMessage, 0,
            reinterpret_cast<LPARAM>(&request));
        return value ? std::move(*value) : Result{};
    }

    // Palette aligned with the menu buttons and output panel; seeded from the
    // active theme (see GuiInput::ApplyTheme). Mutable so the runtime theme
    // switch re-colours the prompt. Defaults are the Graphite values.
    COLORREF DialogBack = RGB(8, 10, 14);
    COLORREF DialogPanel = RGB(28, 32, 38);
    COLORREF DialogEdit = RGB(20, 24, 30);
    COLORREF DialogText = RGB(214, 220, 224);
    COLORREF DialogPromptText = RGB(190, 198, 206);
    COLORREF DialogAccent = RGB(142, 152, 164);
    COLORREF DialogButtonTop = RGB(46, 50, 56);
    COLORREF DialogButtonBottom = RGB(24, 27, 33);
    COLORREF DialogButtonBorder = RGB(146, 156, 166);
    COLORREF DialogButtonText = RGB(214, 220, 224);
    constexpr int kPanelCornerRadius = 18;

    HFONT g_hPromptFont = nullptr;
    HFONT g_hEditFont = nullptr;
    HBRUSH g_hBackBrush = nullptr;
    HBRUSH g_hPanelBrush = nullptr;
    HBRUSH g_hEditBrush = nullptr;

    std::wstring ToWide(const char* s) {
        if (!s) return std::wstring();
        int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (len <= 0) return std::wstring();
        std::wstring out(static_cast<size_t>(len), L'\0');
        int copied = MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len);
        if (copied <= 0) return std::wstring();
        out.resize(static_cast<size_t>(copied - 1));
        return out;
    }

    std::string FromWide(const std::wstring& w) {
        if (w.empty()) return std::string();
        int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                       nullptr, 0, nullptr, nullptr);
        if (len <= 0) return std::string();
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                            out.data(), len, nullptr, nullptr);
        return out;
    }

    HWND OwnerWindowSnapshot() {
        HWND owner = g_hOwner.load(std::memory_order_acquire);
        return owner && IsWindow(owner) ? owner : nullptr;
    }

    HWND ModalOwnerWindow() {
        HWND owner = OwnerWindowSnapshot();
        if (!owner) return nullptr;
        return owner;
    }

    // ----------------------------------------------------------------------
    // A minimal programmatic modal input dialog. No .rc entry — we register
    // a window class on first use and run our own modal loop.
    // ----------------------------------------------------------------------

    struct PromptCtx {
        std::wstring prompt;
        std::wstring value;
        bool        ok = false;
        bool        done = false;   // set when the prompt window is destroyed
        int         promptHeight = 110;  // message STATIC height (px), measured per message
    };

    // Shared dialog layout metrics. WM_CREATE positions controls from these and
    // RunPrompt sizes the window from the same numbers, so the two stay in sync.
    // With promptHeight == 110 these reproduce the original fixed layout exactly.
    constexpr int kDlgMargin   = 30;   // left margin / content inset
    constexpr int kDlgContentW = 700;  // content (static / edit) width
    constexpr int kDlgStaticTop = 30;
    constexpr int kDlgStaticToEdit = 16;
    constexpr int kDlgEditH    = 44;
    constexpr int kDlgEditToBtn = 20;
    constexpr int kDlgBtnH     = 40;
    constexpr int kDlgBtnW     = 108;
    constexpr int kDlgBottom   = 30;
    constexpr int kDlgClientW  = 760;  // kDlgMargin + kDlgContentW + kDlgMargin
    constexpr int kDlgMinPromptH = 110;
    constexpr int kDlgMaxPromptH = 600;

    inline int DlgEditTop(int promptH)  { return kDlgStaticTop + promptH + kDlgStaticToEdit; }
    inline int DlgBtnTop(int promptH)   { return DlgEditTop(promptH) + kDlgEditH + kDlgEditToBtn; }
    inline int DlgClientH(int promptH)  { return DlgBtnTop(promptH) + kDlgBtnH + kDlgBottom; }

    constexpr UINT_PTR ID_EDIT = 0x101;
    constexpr UINT_PTR ID_OK = IDOK;
    constexpr UINT_PTR ID_CANCEL = IDCANCEL;

    void DrawPromptButton(const DRAWITEMSTRUCT* dis) {
        if (!dis) return;
        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        const bool focused = (dis->itemState & ODS_FOCUS) != 0;

        RECT rc = dis->rcItem;
        HDC hdc = dis->hDC;
        COLORREF top = pressed ? RGB(56, 60, 66) : DialogButtonTop;
        COLORREF bottom = pressed ? RGB(30, 34, 40) : DialogButtonBottom;
        COLORREF border = focused ? RGB(204, 210, 216) : DialogButtonBorder;

        RECT topHalf = rc;
        RECT bottomHalf = rc;
        topHalf.bottom = rc.top + ((rc.bottom - rc.top) / 2);
        bottomHalf.top = topHalf.bottom;
        HBRUSH topBrush = CreateSolidBrush(top);
        HBRUSH bottomBrush = CreateSolidBrush(bottom);
        FillRect(hdc, &topHalf, topBrush);
        FillRect(hdc, &bottomHalf, bottomBrush);
        DeleteObject(topBrush);
        DeleteObject(bottomBrush);

        HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, border);
        HBRUSH hollow = (HBRUSH)GetStockObject(HOLLOW_BRUSH);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, hollow);
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        wchar_t text[64]{};
        GetWindowTextW(dis->hwndItem, text, ARRAYSIZE(text));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, DialogButtonText);
        DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    LRESULT CALLBACK PromptWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        PromptCtx* ctx = reinterpret_cast<PromptCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<PromptCtx*>(cs->lpCreateParams);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            HFONT hFont = g_hEditFont ? g_hEditFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT hPromptFont = g_hPromptFont ? g_hPromptFont : hFont;

            const int promptH = ctx->promptHeight;
            const int editTop = DlgEditTop(promptH);
            const int btnTop  = DlgBtnTop(promptH);
            // Right-align the OK/Cancel pair against the content's right edge.
            const int cancelLeft = kDlgMargin + kDlgContentW - kDlgBtnW;   // 622 @ default
            const int okLeft     = cancelLeft - 8 - kDlgBtnW;              // 506 @ default

            HWND hStatic = CreateWindowExW(0, L"STATIC", ctx->prompt.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                kDlgMargin, kDlgStaticTop, kDlgContentW, promptH,
                hWnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(hStatic, WM_SETFONT, (WPARAM)hPromptFont, TRUE);

            HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ctx->value.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                kDlgMargin, editTop, kDlgContentW, kDlgEditH,
                hWnd, (HMENU)ID_EDIT, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(hEdit, EM_SETSEL, 0, -1);
            SetFocus(hEdit);

            HWND hOk = CreateWindowExW(0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_DEFPUSHBUTTON,
                okLeft, btnTop, kDlgBtnW, kDlgBtnH, hWnd, (HMENU)ID_OK, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(hOk, WM_SETFONT, (WPARAM)hPromptFont, TRUE);

            HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                cancelLeft, btnTop, kDlgBtnW, kDlgBtnH, hWnd, (HMENU)ID_CANCEL, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(hCancel, WM_SETFONT, (WPARAM)hPromptFont, TRUE);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, DialogPromptText);
            return (INT_PTR)g_hPanelBrush;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, DialogEdit);
            SetTextColor(hdc, DialogText);
            return (INT_PTR)g_hEditBrush;
        }
        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect(hdc, &rc, g_hBackBrush);

            RECT panel = { 12, 12, rc.right - 12, rc.bottom - 12 };

            // Sleek rounded inner card, drawn with a thin accent stroke.
            HPEN pen = CreatePen(PS_SOLID, 1, DialogAccent);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, g_hPanelBrush);
            RoundRect(hdc, panel.left, panel.top, panel.right, panel.bottom,
                      kPanelCornerRadius, kPanelCornerRadius);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
            return 1;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis && dis->CtlType == ODT_BUTTON) {
                DrawPromptButton(dis);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            if (id == ID_OK) {
                if (ctx) {
                    HWND hEdit = GetDlgItem(hWnd, (int)ID_EDIT);
                    int len = GetWindowTextLengthW(hEdit);
					std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
					int copied = GetWindowTextW(hEdit, buf.data(), len + 1);
					buf.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
                    ctx->value = buf;
                    ctx->ok = true;
                }
                DestroyWindow(hWnd);
                return 0;
            }
            if (id == ID_CANCEL) {
                if (ctx) ctx->ok = false;
                DestroyWindow(hWnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (ctx) ctx->ok = false;
            DestroyWindow(hWnd);
            return 0;
        case WM_DESTROY:
            if (ctx) ctx->done = true;  // signal modal loop to exit
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    void EnsureClassRegistered() {
        static bool registered = false;
        if (registered) return;
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PromptWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"OptiScanPromptDialog";
        RegisterClassExW(&wc);
        registered = true;
    }

    // Run a private modal loop for the prompt window.
    bool RunPrompt(const std::wstring& title, const std::wstring& message,
                   std::wstring& valueInOut) {
        if (!g_hBackBrush) g_hBackBrush = CreateSolidBrush(DialogBack);
        if (!g_hPanelBrush) g_hPanelBrush = CreateSolidBrush(DialogPanel);
        if (!g_hEditBrush) g_hEditBrush = CreateSolidBrush(DialogEdit);
        if (!g_hPromptFont) {
            // Inter SemiBold (bundled), matching the command buttons.
            g_hPromptFont = CreateFontW(-21, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                DEFAULT_PITCH | FF_SWISS, UiSemiBoldFamily());
        }
        if (!g_hEditFont) {
            // JetBrains Mono (bundled) mirrors the main output box, so the number
            // the user types reads in the same typeface as the menu listing above
            // it. FW_BOLD selects the family's real Bold face.
            g_hEditFont = CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                FIXED_PITCH | FF_MODERN, MonoFamily());
        }
        EnsureClassRegistered();

        PromptCtx ctx;
        ctx.prompt = message;
        ctx.value = valueInOut;

        // Measure the message at the content width so the dialog grows to fit
        // multi-line option lists instead of clipping them behind the edit box.
        // Mirror the STATIC's wrapping (SS_LEFT word-wrap) with DT_WORDBREAK.
        {
            HDC hdc = GetDC(nullptr);
            HFONT pf = g_hPromptFont ? g_hPromptFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HGDIOBJ oldFont = SelectObject(hdc, pf);
            RECT calc = { 0, 0, kDlgContentW, 0 };
            DrawTextW(hdc, ctx.prompt.c_str(), -1, &calc,
                      DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            SelectObject(hdc, oldFont);
            ReleaseDC(nullptr, hdc);
            int measured = (calc.bottom - calc.top) + 12;  // padding under last line
            if (measured < kDlgMinPromptH) measured = kDlgMinPromptH;
            if (measured > kDlgMaxPromptH) measured = kDlgMaxPromptH;
            ctx.promptHeight = measured;
        }

        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        RECT desired = { 0, 0, kDlgClientW, DlgClientH(ctx.promptHeight) };
        AdjustWindowRect(&desired, style, FALSE);
        int width = desired.right - desired.left;
        int height = desired.bottom - desired.top;
        int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        if (HWND ownerWindow = OwnerWindowSnapshot()) {
            RECT owner;
            if (GetWindowRect(ownerWindow, &owner)) {
                x = owner.left + ((owner.right - owner.left) - width) / 2;
                y = owner.top + ((owner.bottom - owner.top) - height) / 2;
            }
        }

        HWND hOwner = ModalOwnerWindow();
        BOOL ownerWasEnabled = FALSE;
        if (hOwner) {
            ownerWasEnabled = IsWindowEnabled(hOwner);
            EnableWindow(hOwner, FALSE);
        }

        HWND hWnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_LAYERED,
            L"OptiScanPromptDialog",
            title.c_str(), style, x, y, width, height,
            hOwner, nullptr,
            GetModuleHandleW(nullptr), &ctx);
        if (!hWnd) {
            if (hOwner && ownerWasEnabled) EnableWindow(hOwner, TRUE);
            return false;
        }

        // Slight translucency lets the menu read through the prompt edges,
        // so it feels like a floating card rather than an opaque dialog.
        SetLayeredWindowAttributes(hWnd, 0, 240, LWA_ALPHA);

        ShowWindow(hWnd, SW_SHOW);
        SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        BringWindowToTop(hWnd);
        SetForegroundWindow(hWnd);
        UpdateWindow(hWnd);

        // Private modal loop. We use PeekMessage so we can exit cleanly when
        // ctx.done is set by WM_DESTROY, without disturbing the main app's
        // message queue with a WM_QUIT.
        MSG msg;
        while (!ctx.done) {
            BOOL got = PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);
            if (!got) {
                WaitMessage();
                continue;
            }
            if (msg.message == WM_QUIT) {
                // Re-post so the outer message loop sees it after we return.
                PostQuitMessage(static_cast<int>(msg.wParam));
                DestroyWindow(hWnd);
                break;
            }
            if (msg.message == WM_KEYDOWN && msg.hwnd && msg.wParam == VK_RETURN) {
                SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(ID_OK, BN_CLICKED), 0);
                continue;
            }
            if (msg.message == WM_KEYDOWN && msg.hwnd && msg.wParam == VK_ESCAPE) {
                SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(ID_CANCEL, BN_CLICKED), 0);
                continue;
            }
            if (!IsDialogMessageW(hWnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        if (hOwner && ownerWasEnabled) {
            EnableWindow(hOwner, TRUE);
            SetForegroundWindow(hOwner);
        }

        valueInOut = ctx.value;
        return ctx.ok;
    }

} // namespace

namespace GuiInput {

    void SetOwnerWindow(void* hwnd) {
        HWND owner = reinterpret_cast<HWND>(hwnd);
        g_hOwner.store(owner, std::memory_order_release);
    }

    bool HandleOwnerMessage(unsigned int message, intptr_t lParam, intptr_t* result) {
        if (message != kInvokeOnOwnerMessage) return false;
        auto* request = reinterpret_cast<OwnerInvocation*>(lParam);
        if (request && request->run) request->run(request->context);
        if (result) *result = 0;
        return true;
    }

    void ApplyTheme() {
        const Palette& p = ActiveTheme();
        DialogBack         = p.dlgBack;
        DialogPanel        = p.dlgPanel;
        DialogEdit         = p.dlgEdit;
        DialogText         = p.dlgText;
        DialogPromptText   = p.dlgPrompt;
        DialogAccent       = p.dlgAccent;
        DialogButtonTop    = p.dlgBtnTop;
        DialogButtonBottom = p.dlgBtnBottom;
        DialogButtonBorder = p.dlgBtnBorder;
        DialogButtonText   = p.dlgBtnText;
        // Drop cached brushes so the next prompt rebuilds them from the new
        // colours. The prompt window is modal and short-lived, so no live
        // window holds these handles at switch time.
        if (g_hBackBrush)  { DeleteObject(g_hBackBrush);  g_hBackBrush  = nullptr; }
        if (g_hPanelBrush) { DeleteObject(g_hPanelBrush); g_hPanelBrush = nullptr; }
        if (g_hEditBrush)  { DeleteObject(g_hEditBrush);  g_hEditBrush  = nullptr; }
    }

    int PromptInt(const char* title, const char* message,
                  int minVal, int maxVal, int defaultVal, bool* outOk) {
        std::wstring wTitle = ToWide(title ? title : "Input");
        std::wstring wMsg = ToWide(message ? message : "");
        wchar_t suffix[64];
        // ASCII hyphen-minus only — using a non-ASCII dash (en-dash,
        // em-dash) would require source-file encoding alignment with the
        // compiler's expectations, and UTF-8 byte escapes (\xE2\x80\x93)
        // in an L-string would be misread as three wchar_t values.
        wsprintfW(suffix, L"\r\n(Range: %d - %d, default %d)",
                  minVal, maxVal, defaultVal);
        wMsg += suffix;
        std::wstring value = std::to_wstring(defaultVal);
        bool ok = RunOnOwnerThread([&]() { return RunPrompt(wTitle, wMsg, value); });
        if (outOk) *outOk = ok;
        if (!ok) return defaultVal;
        try {
            int v = std::stoi(value);
            if (v < minVal) v = minVal;
            if (v > maxVal) v = maxVal;
            return v;
        } catch (...) {
            return defaultVal;
        }
    }

    std::string PromptString(const char* title, const char* message,
                             const std::string& defaultVal, bool* outOk) {
        std::wstring wTitle = ToWide(title ? title : "Input");
        std::wstring wMsg = ToWide(message ? message : "");
        std::wstring value = ToWide(defaultVal.c_str());
        bool ok = RunOnOwnerThread([&]() { return RunPrompt(wTitle, wMsg, value); });
        if (outOk) *outOk = ok;
        if (!ok) return defaultVal;
        return FromWide(value);
    }

    std::wstring PromptStringW(const wchar_t* title, const wchar_t* message,
                               const std::wstring& defaultVal, bool* outOk) {
        std::wstring value = defaultVal;
        bool ok = RunOnOwnerThread([&]() {
            return RunPrompt(title ? title : L"Input",
                message ? message : L"", value);
        });
        if (outOk) *outOk = ok;
        if (!ok) return defaultVal;
        return value;
    }

    int PromptYesNoCancel(const char* title, const char* message) {
        std::wstring wTitle = ToWide(title ? title : "Confirm");
        std::wstring wMsg = ToWide(message ? message : "");
        int r = RunOnOwnerThread([&]() {
            return MessageBoxW(ModalOwnerWindow(), wMsg.c_str(), wTitle.c_str(),
                MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
        });
        if (r == IDYES) return 1;
        if (r == IDNO) return 0;
        return -1;
    }

    bool PromptYesNo(const char* title, const char* message) {
        std::wstring wTitle = ToWide(title ? title : "Confirm");
        std::wstring wMsg = ToWide(message ? message : "");
        int r = RunOnOwnerThread([&]() {
            return MessageBoxW(ModalOwnerWindow(), wMsg.c_str(), wTitle.c_str(),
                MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
        });
        return r == IDYES;
    }

    std::wstring PromptForFolder(const wchar_t* title,
                                 const std::wstring& initialDir) {
        return RunOnOwnerThread([&]() {
        std::wstring result;

        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool needUninit = SUCCEEDED(hrInit);

        IFileOpenDialog* pDialog = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IFileOpenDialog, reinterpret_cast<void**>(&pDialog));
        if (SUCCEEDED(hr) && pDialog) {
            DWORD opts = 0;
            pDialog->GetOptions(&opts);
            pDialog->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
            if (title) pDialog->SetTitle(title);

            if (!initialDir.empty()) {
                IShellItem* pStart = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(initialDir.c_str(), nullptr,
                                                         IID_IShellItem,
                                                         reinterpret_cast<void**>(&pStart)))
                    && pStart) {
                    pDialog->SetFolder(pStart);
                    pStart->Release();
                }
            }

            hr = pDialog->Show(ModalOwnerWindow());
            if (SUCCEEDED(hr)) {
                IShellItem* pItem = nullptr;
                if (SUCCEEDED(pDialog->GetResult(&pItem)) && pItem) {
                    PWSTR pPath = nullptr;
                    if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath)) && pPath) {
                        result = pPath;
                        CoTaskMemFree(pPath);
                    }
                    pItem->Release();
                }
            }
            pDialog->Release();
        }

        if (needUninit) CoUninitialize();
        return result;
        });
    }

    void WaitForKey(const char* message) {
        std::wstring wMsg = ToWide(message ? message : "Continue?");
        RunOnOwnerThread([&]() {
            return MessageBoxW(ModalOwnerWindow(), wMsg.c_str(), L"OptiScan",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        });
    }

    void Shutdown() {
        if (g_hPromptFont) { DeleteObject(g_hPromptFont); g_hPromptFont = nullptr; }
        if (g_hEditFont) { DeleteObject(g_hEditFont); g_hEditFont = nullptr; }
        if (g_hBackBrush) { DeleteObject(g_hBackBrush); g_hBackBrush = nullptr; }
        if (g_hPanelBrush) { DeleteObject(g_hPanelBrush); g_hPanelBrush = nullptr; }
        if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = nullptr; }
    }

} // namespace GuiInput
