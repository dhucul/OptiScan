// OptiScanUiMenu.cpp - Operations menu construction.

#include "framework.h"
#include "OptiScanUiInternal.h"

#include <string>

// Progress.h (pulled in elsewhere) does `#undef min` / `#undef max`, which
// would otherwise break the legacy `min(a, b)` / `max(a, b)` calls in the GUI
// layout / paint code. Restore the Windows-style macros for this module too.
#undef min
#undef max
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) < (b)) ? (b) : (a))

// Build an "Operations" menu listing every command, grouped to match the
// on-screen columns. Each item reuses the corresponding button's command id
// (IDC_INFO_BUTTON1 + index), so choosing it routes through the same
// WM_COMMAND handler as clicking the button - no duplicate dispatch logic.
// Menus are fully keyboard-navigable, which is the point: the whole option
// set becomes reachable without tabbing across 33 buttons.
// Recursively test whether a menu (or any of its submenus) contains a command.
static bool MenuContainsCommand(HMENU menu, UINT cmdId)
{
    if (!menu) return false;
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i)
    {
        if (GetMenuItemID(menu, i) == cmdId) return true;
        if (MenuContainsCommand(GetSubMenu(menu, i), cmdId)) return true;
    }
    return false;
}

// Turn a button caption ("1. Copy disc *") into an Operations-menu label
// ("&Copy disc (1)"): drop the prescan marker, move the number to the end so
// first-letter type-ahead works on the operation name, and assign an access
// key (&) on the first letter not already used in this submenu so every item
// has a distinct mnemonic. `usedKeys` accumulates the lowercased keys taken so
// far within the current submenu.
static std::wstring MakeOperationMenuLabel(const wchar_t* commandLabel, std::wstring& usedKeys)
{
    std::wstring s = commandLabel ? commandLabel : L"";
    const size_t marker = s.find(L" *");
    if (marker != std::wstring::npos) s.erase(marker);

    std::wstring number, name;
    const size_t dot = s.find(L". ");
    if (dot != std::wstring::npos) { number = s.substr(0, dot); name = s.substr(dot + 2); }
    else                           { name = s; }

    auto isAlpha = [](wchar_t c) {
        return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
    };
    auto toLower = [](wchar_t c) -> wchar_t {
        return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
    };
    for (size_t k = 0; k < name.size(); ++k)
    {
        if (!isAlpha(name[k])) continue;
        const wchar_t lc = toLower(name[k]);
        if (usedKeys.find(lc) != std::wstring::npos) continue;
        usedKeys.push_back(lc);
        name.insert(name.begin() + k, L'&');
        break;
    }

    std::wstring label = name;
    if (!number.empty()) label += L" (" + number + L")";
    return label;
}

void BuildOperationsMenu(HWND hWnd)
{
    HMENU bar = GetMenu(hWnd);
    if (!bar) return;

    struct Category { const wchar_t* name; int first; int last; };
    // Ranges are button indices (0-based). Clear (34) and Exit (35) are left
    // out: Exit lives in the File menu, Clear in the View menu, and both
    // remain reachable via Tab.
    static const Category categories[] = {
        { L"&Ripping",      0,  5  },
        { L"Disc &Quality", 6,  12 },
        { L"Disc I&nfo",    13, 18 },
        { L"Dri&ve",        19, 25 },
        { L"&Utility",      26, 33 },
    };

    HMENU operations = CreatePopupMenu();
    if (!operations) return;

    for (const auto& cat : categories)
    {
        HMENU sub = CreatePopupMenu();
        if (!sub) continue;
        std::wstring usedKeys;  // distinct access keys within this submenu
        for (int i = cat.first; i <= cat.last && i < COMMAND_BUTTON_COUNT; ++i)
        {
            const std::wstring label = MakeOperationMenuLabel(CommandLabels[i], usedKeys);
            AppendMenuW(sub, MF_STRING, (UINT_PTR)(IDC_INFO_BUTTON1 + i), label.c_str());
        }
        AppendMenuW(operations, MF_POPUP, (UINT_PTR)sub, cat.name);
    }

    // Insert right after the File menu, located by content rather than a fixed
    // index so it survives any future reordering of the menu bar.
    int insertAt = 1;
    const int barCount = GetMenuItemCount(bar);
    for (int i = 0; i < barCount; ++i)
    {
        if (MenuContainsCommand(GetSubMenu(bar, i), IDM_EXIT)) { insertAt = i + 1; break; }
    }
    if (insertAt > barCount) insertAt = barCount;

    InsertMenuW(bar, insertAt, MF_BYPOSITION | MF_POPUP, (UINT_PTR)operations, L"&Operations");
    if (IsWindowVisible(hWnd))
    {
        DrawMenuBar(hWnd);
    }
}

