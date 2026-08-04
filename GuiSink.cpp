#define NOMINMAX
#include "GuiSink.h"
#include "OutputControl.h"
#include "AccessibleAnnounce.h"
#include "Theme.h"

#include <commctrl.h>
#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>
#include <vector>

namespace {

    HWND g_hOutput        = nullptr;   // OutputControl HWND
    HWND g_hProgressText  = nullptr;
    HWND g_hProgressBar   = nullptr;
    HWND g_hAccessibleEdit = nullptr;  // read-only EDIT mirror for screen readers
    HWND g_hDrainWindow   = nullptr;
    UINT g_drainMessage   = 0;

    std::mutex g_mutex;

    // One unit of output waiting for the UI thread. Two flavours:
    //   * stream text  (direct == false) - cout/Console:: output that still
    //     needs ANSI-SGR + progress-line parsing.
    //   * direct line  (direct == true)  - a pre-coloured line (op headers,
    //     batch markers) that must be appended verbatim with an explicit
    //     colour, bypassing the parser. These used to be written to the
    //     OutputControl directly from the calling thread, which raced with
    //     the UI-thread drain and corrupted the log vectors; routing them
    //     through the same FIFO makes every control mutation happen on the
    //     UI thread while preserving ordering with surrounding stream output.
    struct QueueItem {
        std::wstring text;
        bool         direct = false;
        COLORREF     color  = 0;
    };
    std::deque<QueueItem> g_queue;      // output waiting for the UI thread
    bool g_drainPending = false;

    // UI-thread parser state. CR ('\r') starts a "progress" line; the chars
    // that follow accumulate into g_progressLine and are routed to the
    // progress strip instead of being appended to the log. A subsequent LF
    // commits and clears the progress state.
    bool         g_inProgressLine = false;
    std::wstring g_progressLine;

    // Progress-milestone announcement state (for screen readers). We speak at
    // 25 / 50 / 75 / 100 %; the quartile resets when a new operation's
    // progress value drops back toward zero.
    int g_progressLastPermille = -1;
    int g_progressAnnouncedQuartile = 0;

    // SGR colour state is preserved across drains because each cout flush
    // can split an escape from the text it colours.
    COLORREF g_sgrColor    = 0;
    bool     g_sgrExplicit = false;

    // Default colour for non-explicit runs. The actual rendered colour is
    // re-derived per line by OutputControl, so this is only a fallback; kept
    // in sync with the active theme's fg for consistency.
    COLORREF kDefault = RGB(216, 216, 211);

    // ----------------------------------------------------------------------
    // Progress strip helpers (CR-overwritten line goes here, not into the log)
    // ----------------------------------------------------------------------
    std::wstring TrimProgressText(const std::wstring& text) {
        size_t begin = 0;
        while (begin < text.size() && std::iswspace(text[begin])) ++begin;
        size_t end = text.size();
        while (end > begin && std::iswspace(text[end - 1])) --end;

        std::wstring trimmed = text.substr(begin, end - begin);
        const size_t bracketStart = trimmed.find(L'[');
        const size_t bracketEnd = (bracketStart == std::wstring::npos)
            ? std::wstring::npos
            : trimmed.find(L']', bracketStart);
        if (bracketStart != std::wstring::npos && bracketEnd != std::wstring::npos) {
            trimmed.erase(bracketStart, bracketEnd - bracketStart + 1);
        }
        begin = 0;
        while (begin < trimmed.size() && std::iswspace(trimmed[begin])) ++begin;
        end = trimmed.size();
        while (end > begin && std::iswspace(trimmed[end - 1])) --end;
        return trimmed.substr(begin, end - begin);
    }

    bool ExtractProgressPermille(const std::wstring& text, int& permille) {
        const size_t percent = text.find(L'%');
        if (percent == std::wstring::npos) return false;
        size_t begin = percent;
        while (begin > 0) {
            wchar_t ch = text[begin - 1];
            if ((ch >= L'0' && ch <= L'9') || ch == L'.' || std::iswspace(ch)) --begin;
            else break;
        }
        while (begin < percent && std::iswspace(text[begin])) ++begin;
        if (begin >= percent) return false;
        wchar_t* parseEnd = nullptr;
        double pct = std::wcstod(text.c_str() + begin, &parseEnd);
        if (!parseEnd || parseEnd == text.c_str() + begin) return false;
        pct = std::max(0.0, std::min(100.0, pct));
        permille = static_cast<int>(pct * 10.0 + 0.5);
        return true;
    }

    void AnnounceProgressMilestone(int permille) {
        // A drop back toward zero marks a new operation's progress restarting.
        if (permille + 30 < g_progressLastPermille) {
            g_progressAnnouncedQuartile = 0;
        }
        g_progressLastPermille = permille;
        const int quartile = permille / 250;   // 0..4 -> 0/25/50/75/100 %
        if (quartile > g_progressAnnouncedQuartile) {
            g_progressAnnouncedQuartile = quartile;
            if (quartile >= 1) {
                AccessibleAnnounce::Announce(std::to_wstring(quartile * 25) + L" percent");
            }
        }
    }

    void UpdateProgressUiThread(const std::wstring& text) {
        if (text.empty()) return;
        const std::wstring display = TrimProgressText(text);
        if (g_hProgressText && !display.empty()) {
            SetWindowTextW(g_hProgressText, display.c_str());
        }
        int permille = 0;
        if (ExtractProgressPermille(text, permille)) {
            if (g_hProgressBar) SendMessageW(g_hProgressBar, PBM_SETPOS, (WPARAM)permille, 0);
            AnnounceProgressMilestone(permille);
        }
    }

    void ClearProgressUiThread() {
        if (g_hProgressText) SetWindowTextW(g_hProgressText, L"Idle");
        if (g_hProgressBar)  SendMessageW(g_hProgressBar, PBM_SETPOS, 0, 0);
        g_progressLastPermille = -1;
        g_progressAnnouncedQuartile = 0;
    }

    // ----------------------------------------------------------------------
    // ANSI SGR parsing. We honour only the codes the app actually emits:
    //   ESC [ 38 ; 2 ; R ; G ; B m   24-bit foreground colour
    //   ESC [ 0 m / ESC [ 39 m       reset
    // ----------------------------------------------------------------------
    struct SgrState {
        COLORREF color = 0;
        bool explicit_ = false;
    };

    void ParseParams(const std::wstring& body, std::vector<int>& out) {
        out.clear();
        int cur = 0;
        bool any = false;
        for (wchar_t c : body) {
            if (c >= L'0' && c <= L'9') { cur = cur * 10 + (c - L'0'); any = true; }
            else if (c == L';') { out.push_back(any ? cur : 0); cur = 0; any = false; }
        }
        if (any) out.push_back(cur);
        if (out.empty()) out.push_back(0);
    }

    void ApplySgr(const std::vector<int>& params, SgrState& state) {
        for (size_t i = 0; i < params.size(); i++) {
            int p = params[i];
            if (p == 0 || p == 39) {
                state.color = 0;
                state.explicit_ = false;
            } else if (p == 38 && i + 4 < params.size() && params[i + 1] == 2) {
                int r = params[i + 2] & 0xFF;
                int g = params[i + 3] & 0xFF;
                int b = params[i + 4] & 0xFF;
                state.color = RGB(r, g, b);
                state.explicit_ = true;
                i += 4;
            }
        }
    }

    // ----------------------------------------------------------------------
    // Drain: parse ANSI escapes, route progress lines, append the rest to
    // the OutputControl.
    // ----------------------------------------------------------------------
    void AppendToMirrorEdit(const std::wstring& text);  // defined below

    void EmitSegment(const std::wstring& text, const SgrState& sgr) {
        if (text.empty() || !g_hOutput) return;
        if (sgr.explicit_) {
            OutputControl::Append(g_hOutput, text.c_str(), text.size(),
                                  sgr.color, /*isExplicit=*/true);
        } else {
            OutputControl::Append(g_hOutput, text.c_str(), text.size(),
                                  kDefault, /*isExplicit=*/false);
        }
    }

    void ProcessToOutput(const std::wstring& text) {
        if (text.empty()) return;

        SgrState sgr;
        sgr.color = g_sgrColor;
        sgr.explicit_ = g_sgrExplicit;

        // Buffer of characters that should be emitted with the current sgr
        // state to the OutputControl. Flushed on SGR change or end-of-input.
        std::wstring pending;

        // Plain copy of everything that lands in the log (no ANSI, no progress
        // lines), appended verbatim to the accessible mirror EDIT at the end.
        std::wstring mirror;

        auto flushPending = [&]() {
            EmitSegment(pending, sgr);
            pending.clear();
        };

        const wchar_t* p = text.c_str();
        const wchar_t* end = p + text.size();
        while (p < end) {
            wchar_t ch = *p;

            // CSI sequence: ESC [ params final.
            if (ch == 0x1B && p + 1 < end && p[1] == L'[') {
                p += 2;
                std::wstring body;
                wchar_t finalByte = 0;
                while (p < end) {
                    wchar_t d = *p++;
                    if (d >= 0x40 && d <= 0x7E) { finalByte = d; break; }
                    body.push_back(d);
                }
                if (finalByte == L'm') {
                    flushPending();
                    std::vector<int> params;
                    ParseParams(body, params);
                    ApplySgr(params, sgr);
                }
                continue;
            }
            // Other ESC: skip ESC + one byte.
            if (ch == 0x1B) {
                p++;
                if (p < end) p++;
                continue;
            }

            if (ch == L'\r') {
                // Start a progress line: commit anything buffered to the log
                // *without* a newline, then start collecting overwrite text.
                flushPending();
                g_progressLine.clear();
                g_inProgressLine = true;
                p++;
                continue;
            }
            if (ch == L'\n') {
                if (g_inProgressLine) {
                    // Commit the progress line to the native progress strip,
                    // not to the log.
                    UpdateProgressUiThread(g_progressLine);
                    g_progressLine.clear();
                    g_inProgressLine = false;
                } else {
                    pending.push_back(L'\n');
                    flushPending();
                    mirror.push_back(L'\n');
                }
                p++;
                continue;
            }

            // Regular character.
            if (g_inProgressLine) {
                g_progressLine.push_back(ch);
            } else {
                pending.push_back(ch);
                mirror.push_back(ch);
            }
            p++;
        }

        if (g_inProgressLine && !g_progressLine.empty()) {
            UpdateProgressUiThread(g_progressLine);
        }
        flushPending();
        AppendToMirrorEdit(mirror);

        g_sgrColor = sgr.color;
        g_sgrExplicit = sgr.explicit_;
    }

    // ----------------------------------------------------------------------
    // Accessible mirror: append plain text to a read-only multiline EDIT so a
    // screen reader can read the log natively. Always called on the UI thread.
    // ----------------------------------------------------------------------
    //
    // Keep the EDIT from growing without bound on very long scans: once it
    // exceeds kMirrorMaxChars we drop the oldest text back down to
    // kMirrorKeepChars. EDIT controls slow down noticeably past ~1M chars.
    constexpr int kMirrorMaxChars  = 1000000;
    constexpr int kMirrorKeepChars = 800000;

    // EDIT controls need "\r\n" line breaks; the log stream uses bare '\n'
    // (a bare '\r' upstream means a progress line and never reaches here).
    // Existing "\r\n" pairs are left intact.
    std::wstring ToCrlf(const std::wstring& in) {
        std::wstring out;
        out.reserve(in.size() + in.size() / 16 + 8);
        for (wchar_t c : in) {
            if (c == L'\n' && (out.empty() || out.back() != L'\r')) {
                out.push_back(L'\r');
            }
            out.push_back(c);
        }
        return out;
    }

    void AppendToMirrorEdit(const std::wstring& text) {
        HWND h = g_hAccessibleEdit;
        if (!h || text.empty()) return;

        const std::wstring crlf = ToCrlf(text);

        // Append at the end. EM_REPLACESEL is honoured on read-only EDITs
        // (ES_READONLY only blocks *user* keystrokes, not programmatic
        // messages); passing FALSE skips building an undo buffer.
        const int endPos = GetWindowTextLengthW(h);
        DWORD savedStart = 0, savedEnd = 0;
        SendMessageW(h, EM_GETSEL, reinterpret_cast<WPARAM>(&savedStart),
            reinterpret_cast<LPARAM>(&savedEnd));
        const bool followTail = savedEnd >= static_cast<DWORD>(endPos);
        const int firstVisibleLine = static_cast<int>(SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0));
        SendMessageW(h, EM_SETSEL, (WPARAM)endPos, (LPARAM)endPos);
        SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)crlf.c_str());

        int total = GetWindowTextLengthW(h);
        if (total > kMirrorMaxChars) {
            const int removeCount = total - kMirrorKeepChars;
            SendMessageW(h, EM_SETSEL, 0, (LPARAM)removeCount);
            SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)L"");
            total = GetWindowTextLengthW(h);
            savedStart = savedStart > static_cast<DWORD>(removeCount)
                ? savedStart - removeCount : 0;
            savedEnd = savedEnd > static_cast<DWORD>(removeCount)
                ? savedEnd - removeCount : 0;
        }

        if (followTail) {
            SendMessageW(h, EM_SETSEL, (WPARAM)total, (LPARAM)total);
            SendMessageW(h, EM_SCROLLCARET, 0, 0);
        }
        else {
            SendMessageW(h, EM_SETSEL, savedStart, savedEnd);
            const int currentFirst = static_cast<int>(SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0));
            SendMessageW(h, EM_LINESCROLL, 0, firstVisibleLine - currentFirst);
        }
    }

    // ----------------------------------------------------------------------
    // Worker-thread enqueue.
    // ----------------------------------------------------------------------
    void EnqueueItem(QueueItem item) {
        if (item.text.empty()) return;
        HWND hWnd = nullptr;
        UINT msg = 0;
        bool needPost = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_queue.push_back(std::move(item));
            hWnd = g_hDrainWindow;
            msg = g_drainMessage;
            if (!g_drainPending && hWnd && msg) {
                g_drainPending = true;
                needPost = true;
            }
        }
        if (needPost) PostMessageW(hWnd, msg, 0, 0);
    }

    void EnqueueAndNotify(std::wstring text) {
        if (text.empty()) return;
        EnqueueItem(QueueItem{ std::move(text), /*direct=*/false, 0 });
    }

    // ----------------------------------------------------------------------
    // streambuf subclasses (route cout/wcout/cerr/wcerr through the queue)
    // ----------------------------------------------------------------------
    class NarrowSinkBuf : public std::streambuf {
    public:
        NarrowSinkBuf() { setp(m_buf, m_buf + sizeof(m_buf) - 1); }
    protected:
        int_type overflow(int_type c) override {
            if (sync() != 0) return traits_type::eof();
            if (c != traits_type::eof()) sputc(static_cast<char>(c));
            return c;
        }
        int sync() override {
            std::ptrdiff_t n = pptr() - pbase();
            if (n <= 0) return 0;
            m_pending.append(pbase(), static_cast<size_t>(n));
            pbump(static_cast<int>(-n));
            size_t complete = m_pending.size();
            size_t lead = complete;
            while (lead > 0 &&
                (static_cast<unsigned char>(m_pending[lead - 1]) & 0xC0) == 0x80) --lead;
            if (lead > 0) {
                const unsigned char c = static_cast<unsigned char>(m_pending[lead - 1]);
                const size_t expected = c < 0x80 ? 1 :
                    ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 :
                    ((c & 0xF8) == 0xF0 ? 4 : 1)));
                const size_t available = complete - (lead - 1);
                if (expected > available) complete = lead - 1;
            }
            if (complete > 0) {
                GuiSink::AppendUtf8(m_pending.data(), complete);
                m_pending.erase(0, complete);
            }
            return 0;
        }
    private:
        char m_buf[4096];
        std::string m_pending;
    };

    class WideSinkBuf : public std::wstreambuf {
    public:
        WideSinkBuf() { setp(m_buf, m_buf + (sizeof(m_buf) / sizeof(m_buf[0])) - 1); }
    protected:
        int_type overflow(int_type c) override {
            if (sync() != 0) return traits_type::eof();
            if (c != traits_type::eof()) sputc(static_cast<wchar_t>(c));
            return c;
        }
        int sync() override {
            std::ptrdiff_t n = pptr() - pbase();
            if (n <= 0) return 0;
            GuiSink::AppendWide(pbase(), static_cast<size_t>(n));
            pbump(static_cast<int>(-n));
            return 0;
        }
    private:
        wchar_t m_buf[2048];
    };

    NarrowSinkBuf* g_coutBuf  = nullptr;
    NarrowSinkBuf* g_cerrBuf  = nullptr;
    WideSinkBuf*   g_wcoutBuf = nullptr;
    WideSinkBuf*   g_wcerrBuf = nullptr;

    class EmptyNarrowBuf : public std::streambuf {
    protected:
        int_type underflow() override { return traits_type::eof(); }
    };
    class EmptyWideBuf : public std::wstreambuf {
    protected:
        int_type underflow() override { return traits_type::eof(); }
    };
    EmptyNarrowBuf* g_cinBuf  = nullptr;
    EmptyWideBuf*   g_wcinBuf = nullptr;
    std::streambuf* g_oldCoutBuf = nullptr;
    std::streambuf* g_oldCerrBuf = nullptr;
    std::wstreambuf* g_oldWcoutBuf = nullptr;
    std::wstreambuf* g_oldWcerrBuf = nullptr;
    std::streambuf* g_oldCinBuf = nullptr;
    std::wstreambuf* g_oldWcinBuf = nullptr;

    bool g_installed = false;

}  // namespace

namespace GuiSink {

    void ApplyTheme() {
        std::lock_guard<std::mutex> lock(g_mutex);
        kDefault = ActiveTheme().fg;
    }

    void SetOutputWindow(HWND hOutputControl) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hOutput = hOutputControl;
        g_inProgressLine = false;
        g_progressLine.clear();
        g_sgrColor = 0;
        g_sgrExplicit = false;
    }

    HWND GetOutputWindow() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_hOutput;
    }

    void SetProgressWindows(HWND hText, HWND hBar) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hProgressText = hText;
        g_hProgressBar  = hBar;
        ClearProgressUiThread();
    }

    void SetAccessibleMirror(HWND hEdit) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hAccessibleEdit = hEdit;
    }

    void MirrorText(const wchar_t* text, size_t len) {
        if (!text || len == 0) return;
        AppendToMirrorEdit(std::wstring(text, len));
    }

    void SetDrainTarget(HWND hwnd, UINT message) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hDrainWindow = hwnd;
        g_drainMessage = message;
    }

    void AppendUtf8(const char* utf8, size_t len) {
        if (!utf8 || len == 0) return;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, nullptr, 0);
        std::wstring wide((size_t)(wlen > 0 ? wlen : 0), L'\0');
        if (wlen > 0) {
            MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, wide.data(), wlen);
        }
        EnqueueAndNotify(std::move(wide));
    }

    void AppendWide(const wchar_t* wide, size_t len) {
        if (!wide || len == 0) return;
        EnqueueAndNotify(std::wstring(wide, len));
    }

    void AppendDirectColored(const wchar_t* text, size_t len, COLORREF color) {
        if (!text || len == 0) return;
        EnqueueItem(QueueItem{ std::wstring(text, len), /*direct=*/true, color });
    }

    void DrainOutputQueue() {
        std::deque<QueueItem> local;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            local.swap(g_queue);
            g_drainPending = false;
        }
        if (local.empty()) return;

        // Coalesce consecutive stream chunks into one ProcessToOutput call
        // (the common case: a drain with no direct items runs the parser once,
        // exactly as before). A direct item flushes the pending stream run
        // first so its pre-coloured line lands in the correct FIFO position,
        // then is appended verbatim - bypassing ANSI/progress parsing so its
        // explicit colour and CR/LF survive. All of this runs on the UI thread.
        std::wstring streamRun;
        auto flushStream = [&]() {
            if (!streamRun.empty()) {
                ProcessToOutput(streamRun);
                streamRun.clear();
            }
        };
        for (auto& item : local) {
            if (item.direct) {
                flushStream();
                if (g_hOutput) {
                    OutputControl::Append(g_hOutput, item.text.c_str(),
                                          item.text.size(), item.color,
                                          /*isExplicit=*/true);
                }
                AppendToMirrorEdit(item.text);
            } else {
                streamRun.append(item.text);
            }
        }
        flushStream();
    }

    void ClearOutput() {
        HWND hOut;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            hOut = g_hOutput;
            g_queue.clear();
        }
        if (hOut) OutputControl::Clear(hOut);
        if (g_hAccessibleEdit) SetWindowTextW(g_hAccessibleEdit, L"");
        g_inProgressLine = false;
        g_progressLine.clear();
        g_sgrColor = 0;
        g_sgrExplicit = false;
        ClearProgressUiThread();
    }

    void InstallStreamRedirect() {
        if (g_installed) return;
        g_installed = true;
        g_coutBuf  = new NarrowSinkBuf();
        g_cerrBuf  = new NarrowSinkBuf();
        g_wcoutBuf = new WideSinkBuf();
        g_wcerrBuf = new WideSinkBuf();
        g_cinBuf   = new EmptyNarrowBuf();
        g_wcinBuf  = new EmptyWideBuf();
        g_oldCoutBuf = std::cout.rdbuf(g_coutBuf);
        g_oldCerrBuf = std::cerr.rdbuf(g_cerrBuf);
        g_oldWcoutBuf = std::wcout.rdbuf(g_wcoutBuf);
        g_oldWcerrBuf = std::wcerr.rdbuf(g_wcerrBuf);
        g_oldCinBuf = std::cin.rdbuf(g_cinBuf);
        g_oldWcinBuf = std::wcin.rdbuf(g_wcinBuf);
        std::cout.setf(std::ios::unitbuf);
        std::cerr.setf(std::ios::unitbuf);
        std::wcout.setf(std::ios::unitbuf);
        std::wcerr.setf(std::ios::unitbuf);
    }

    void UninstallStreamRedirect() {
        if (!g_installed) return;
        std::cout.flush(); std::cerr.flush(); std::wcout.flush(); std::wcerr.flush();
        std::cout.rdbuf(g_oldCoutBuf);
        std::cerr.rdbuf(g_oldCerrBuf);
        std::wcout.rdbuf(g_oldWcoutBuf);
        std::wcerr.rdbuf(g_oldWcerrBuf);
        std::cin.rdbuf(g_oldCinBuf);
        std::wcin.rdbuf(g_oldWcinBuf);
        delete g_coutBuf; delete g_cerrBuf;
        delete g_wcoutBuf; delete g_wcerrBuf;
        delete g_cinBuf; delete g_wcinBuf;
        g_coutBuf = g_cerrBuf = nullptr;
        g_wcoutBuf = g_wcerrBuf = nullptr;
        g_cinBuf = nullptr; g_wcinBuf = nullptr;
        g_installed = false;
    }

}  // namespace GuiSink
