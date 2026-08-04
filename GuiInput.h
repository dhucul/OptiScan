// ============================================================================
// GuiInput.h - Modal-dialog replacements for std::cin / _getch.
//
// All helpers are blocking on the calling thread and run a private modal
// message loop until the user answers. Workflows that used to prompt at the
// console now call these to gather input from the user.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace GuiInput {

    // Owner window used as parent for modal dialogs. Set once at startup.
    void SetOwnerWindow(void* hwnd);

    // Dispatch the private prompt-invocation message on the owner/UI thread.
    bool HandleOwnerMessage(unsigned int message, intptr_t lParam, intptr_t* result);

    // Prompt for an integer in [minVal, maxVal]; returns defaultVal if the
    // user cancels or enters something invalid. If `outOk` is non-null it
    // is set to true when the user pressed OK and false on Cancel / close,
    // so callers can distinguish "user accepted the default" from
    // "user explicitly cancelled".
    int PromptInt(const char* title, const char* message,
                  int minVal, int maxVal, int defaultVal,
                  bool* outOk = nullptr);

    // Prompt for a free-form text answer. Returns defaultVal on cancel. If
    // `outOk` is non-null it is set to true when the user pressed OK and
    // false on Cancel / close — let callers distinguish "left default
    // unchanged" from "user explicitly cancelled".
    std::string PromptString(const char* title, const char* message,
                             const std::string& defaultVal = std::string(),
                             bool* outOk = nullptr);

    // Wide variant (for path prompts). Same outOk contract as PromptString.
    std::wstring PromptStringW(const wchar_t* title, const wchar_t* message,
                               const std::wstring& defaultVal = std::wstring(),
                               bool* outOk = nullptr);

    // Yes/no/cancel prompt. Returns 1 = yes, 0 = no, -1 = cancel.
    int PromptYesNoCancel(const char* title, const char* message);

    // Yes/no prompt. Returns true on Yes.
    bool PromptYesNo(const char* title, const char* message);

    // Replaces _getch() "press any key" pauses. Shows an info MessageBox.
    void WaitForKey(const char* message = nullptr);

    // Native Windows folder-picker (IFileDialog + FOS_PICKFOLDERS).
    // Returns the selected folder on OK, empty string on Cancel/close.
    // `initialDir` (if non-empty and existing) is shown as the starting folder.
    std::wstring PromptForFolder(const wchar_t* title,
                                 const std::wstring& initialDir = std::wstring());

    // Re-seed the prompt-dialog colours from the active theme and drop cached
    // brushes so the next prompt is drawn in the new theme.
    void ApplyTheme();

    // Release process-lifetime prompt fonts and brushes before bundled fonts
    // and the windowing subsystem are torn down.
    void Shutdown();
}
