// Theme.cpp - palette definitions, runtime switch, and persistence.

#include "Theme.h"
#include "ConsoleColor.h"   // Console::ApplyThemePalette

namespace {

// --- Graphite (original dark graphite/slate) --------------------------------
const Palette kGraphite = {
    /* fg            */ RGB(216, 216, 211),
    /* bright        */ RGB(206, 210, 212),
    /* dim           */ RGB(118, 128, 138),
    /* accentWarm    */ RGB(198, 178, 150),
    /* ok            */ RGB(148, 192, 166),
    /* warn          */ RGB(210, 174, 112),
    /* error         */ RGB(222, 126, 122),
    /* cyan          */ RGB(156, 168, 180),
    /* graphFrame    */ RGB(150, 160, 170),
    /* graphBar      */ RGB(174, 182, 190),
    /* selection     */ RGB( 58,  66,  78),

    /* windowBase    */ RGB(  3,   4,   5),
    /* panelSurface  */ RGB(  6,  10,  14),
    /* outputBg      */ RGB( 14,  17,  22),
    /* consoleBase   */ RGB(  4,   5,   6),
    /* chromeAccent  */ RGB(126, 178, 212),
    /* chromeText    */ RGB(178, 188, 196),

    /* btnTop        */ RGB( 38,  44,  54),
    /* btnBottom     */ RGB( 16,  20,  28),
    /* btnBorder     */ RGB(158, 168, 178),
    /* btnBorderFocus*/ RGB(218, 224, 230),
    /* btnStripe     */ RGB(166, 176, 188),
    /* btnNumber     */ RGB(178, 188, 196),
    /* btnLabel      */ RGB(216, 222, 226),
    /* disabledText  */ RGB( 96, 100, 104),

    /* dlgBack       */ RGB(  8,  10,  14),
    /* dlgPanel      */ RGB( 28,  32,  38),
    /* dlgEdit       */ RGB( 20,  24,  30),
    /* dlgText       */ RGB(214, 220, 224),
    /* dlgPrompt     */ RGB(190, 198, 206),
    /* dlgAccent     */ RGB(142, 152, 164),
    /* dlgBtnTop     */ RGB( 46,  50,  56),
    /* dlgBtnBottom  */ RGB( 24,  27,  33),
    /* dlgBtnBorder  */ RGB(146, 156, 166),
    /* dlgBtnText    */ RGB(214, 220, 224),

    /* artTint       */ RGB(  0,   0,   0),  // native art (strength 0)
    /* artTintStrength*/ 0,
};

// --- Catppuccin Frappé (default) --------------------------------------------
const Palette kCatppuccinFrappe = {
    /* fg            */ RGB(198, 208, 245),  // text
    /* bright        */ RGB(205, 214, 245),
    /* dim           */ RGB(115, 121, 148),  // overlay0
    /* accentWarm    */ RGB(239, 159, 118),  // peach
    /* ok            */ RGB(166, 209, 137),  // green
    /* warn          */ RGB(229, 200, 144),  // yellow
    /* error         */ RGB(231, 130, 132),  // red
    /* cyan          */ RGB(153, 209, 219),  // sky
    /* graphFrame    */ RGB( 98, 104, 128),  // surface2
    /* graphBar      */ RGB(133, 193, 220),
    /* selection     */ RGB( 81,  87, 106),  // surface1

    /* windowBase    */ RGB( 35,  38,  52),  // crust
    /* panelSurface  */ RGB( 41,  44,  59),  // mantle
    /* outputBg      */ RGB( 48,  52,  70),  // base
    /* consoleBase   */ RGB( 30,  32,  44),
    /* chromeAccent  */ RGB(140, 170, 221),  // blue
    /* chromeText    */ RGB(181, 191, 231),  // subtext1

    /* btnTop        */ RGB( 65,  69,  89),  // surface0
    /* btnBottom     */ RGB( 41,  44,  59),  // mantle
    /* btnBorder     */ RGB(115, 121, 148),  // overlay0
    /* btnBorderFocus*/ RGB(181, 191, 231),  // subtext1
    /* btnStripe     */ RGB(140, 170, 221),  // blue
    /* btnNumber     */ RGB(181, 191, 231),
    /* btnLabel      */ RGB(198, 208, 245),
    /* disabledText  */ RGB( 92,  97, 118),

    /* dlgBack       */ RGB( 35,  38,  52),
    /* dlgPanel      */ RGB( 41,  44,  59),
    /* dlgEdit       */ RGB( 30,  32,  44),
    /* dlgText       */ RGB(198, 208, 245),
    /* dlgPrompt     */ RGB(181, 191, 231),
    /* dlgAccent     */ RGB(140, 170, 221),
    /* dlgBtnTop     */ RGB( 65,  69,  89),
    /* dlgBtnBottom  */ RGB( 41,  44,  59),
    /* dlgBtnBorder  */ RGB(115, 121, 148),
    /* dlgBtnText    */ RGB(198, 208, 245),

    /* artTint       */ RGB(202, 158, 230),  // mauve
    /* artTintStrength*/ 75,
};

// --- Nord -------------------------------------------------------------------
const Palette kNord = {
    /* fg            */ RGB(216, 222, 233),  // nord4
    /* bright        */ RGB(236, 239, 244),  // nord6
    /* dim           */ RGB( 97, 110, 136),
    /* accentWarm    */ RGB(208, 135, 112),  // nord12 orange
    /* ok            */ RGB(163, 190, 140),  // nord14
    /* warn          */ RGB(235, 203, 139),  // nord13
    /* error         */ RGB(191,  97, 106),  // nord11
    /* cyan          */ RGB(136, 192, 208),  // nord8
    /* graphFrame    */ RGB( 76,  86, 106),  // nord3
    /* graphBar      */ RGB(143, 188, 187),  // nord7
    /* selection     */ RGB( 67,  76,  94),  // nord2

    /* windowBase    */ RGB( 40,  46,  57),
    /* panelSurface  */ RGB( 46,  52,  64),  // nord0
    /* outputBg      */ RGB( 39,  44,  54),
    /* consoleBase   */ RGB( 34,  39,  49),
    /* chromeAccent  */ RGB(129, 161, 193),  // nord9
    /* chromeText    */ RGB(216, 222, 233),

    /* btnTop        */ RGB( 59,  66,  82),  // nord1
    /* btnBottom     */ RGB( 43,  48,  59),
    /* btnBorder     */ RGB( 76,  86, 106),  // nord3
    /* btnBorderFocus*/ RGB(216, 222, 233),
    /* btnStripe     */ RGB(129, 161, 193),
    /* btnNumber     */ RGB(216, 222, 233),
    /* btnLabel      */ RGB(236, 239, 244),
    /* disabledText  */ RGB( 88,  96, 112),

    /* dlgBack       */ RGB( 34,  39,  49),
    /* dlgPanel      */ RGB( 59,  66,  82),
    /* dlgEdit       */ RGB( 40,  46,  57),
    /* dlgText       */ RGB(216, 222, 233),
    /* dlgPrompt     */ RGB(229, 233, 240),
    /* dlgAccent     */ RGB(129, 161, 193),
    /* dlgBtnTop     */ RGB( 67,  76,  94),
    /* dlgBtnBottom  */ RGB( 59,  66,  82),
    /* dlgBtnBorder  */ RGB( 76,  86, 106),
    /* dlgBtnText    */ RGB(216, 222, 233),

    /* artTint       */ RGB(110, 190, 206),  // frost (punchier than nord8)
    /* artTintStrength*/ 76,
};

// --- Arc-Dark ---------------------------------------------------------------
const Palette kArcDark = {
    /* fg            */ RGB(211, 218, 227),
    /* bright        */ RGB(231, 235, 240),
    /* dim           */ RGB(139, 145, 153),
    /* accentWarm    */ RGB(240, 198, 116),  // subtle gold (Arc is mono-blue)
    /* ok            */ RGB(126, 191, 106),
    /* warn          */ RGB(240, 198, 116),
    /* error         */ RGB(224, 108, 117),
    /* cyan          */ RGB(108, 182, 227),
    /* graphFrame    */ RGB( 91,  98, 115),
    /* graphBar      */ RGB(123, 168, 216),
    /* selection     */ RGB( 69,  74,  90),

    /* windowBase    */ RGB( 43,  46,  57),  // #2b2e39
    /* panelSurface  */ RGB( 47,  52,  63),  // #2f343f
    /* outputBg      */ RGB( 56,  60,  74),  // #383c4a
    /* consoleBase   */ RGB( 38,  41,  50),
    /* chromeAccent  */ RGB( 82, 148, 226),  // #5294e2
    /* chromeText    */ RGB(197, 205, 214),

    /* btnTop        */ RGB( 64,  69,  82),
    /* btnBottom     */ RGB( 43,  47,  58),
    /* btnBorder     */ RGB( 91,  98, 115),
    /* btnBorderFocus*/ RGB(231, 235, 240),
    /* btnStripe     */ RGB( 82, 148, 226),
    /* btnNumber     */ RGB(197, 205, 214),
    /* btnLabel      */ RGB(231, 235, 240),
    /* disabledText  */ RGB( 96, 101, 110),

    /* dlgBack       */ RGB( 38,  41,  50),
    /* dlgPanel      */ RGB( 47,  52,  63),
    /* dlgEdit       */ RGB( 42,  46,  56),
    /* dlgText       */ RGB(211, 218, 227),
    /* dlgPrompt     */ RGB(189, 196, 206),
    /* dlgAccent     */ RGB( 82, 148, 226),
    /* dlgBtnTop     */ RGB( 64,  69,  82),
    /* dlgBtnBottom  */ RGB( 47,  52,  63),
    /* dlgBtnBorder  */ RGB( 91,  98, 115),
    /* dlgBtnText    */ RGB(211, 218, 227),

    /* artTint       */ RGB( 92, 148, 210),  // arc blue (softened)
    /* artTintStrength*/ 68,
};

// --- Apple Light (soft-white, near-black text, blue accent) ------------------
// First light theme. Surfaces are a soft off-white family (panels/log), body
// text is Apple's near-black #1D1D1F, and Apple blue #0071E3 is the single
// accent (numbers, stripes, wordmark, chrome) — deliberately no warm/orange.
// Status colours stay the SF system palette (green/amber/red/blue) so log
// severity still reads clearly. The procedural backdrop reads these live; the
// edge vignette in OptiScanUiPaint.cpp softens itself on light themes.
const Palette kAppleLight = {
    /* fg            */ RGB( 29,  29,  31),  // #1D1D1F near-black body text
    /* bright        */ RGB( 10,  10,  12),  // #0A0A0C headings
    /* dim           */ RGB(110, 110, 115),  // #6E6E73 secondary gray
    /* accentWarm    */ RGB(  0, 113, 227),  // #0071E3 Apple blue (title / >>>)
    /* ok            */ RGB( 30, 142,  62),  // #1E8E3E SF green (for white bg)
    /* warn          */ RGB(154, 106,   0),  // #9A6A00 amber (semantic [WARN])
    /* error         */ RGB(215,   0,  21),  // #D70015 SF red
    /* cyan          */ RGB(  0, 113, 227),  // #0071E3 SF blue = [INFO]
    /* graphFrame    */ RGB(142, 142, 147),  // #8E8E93 mid gray box chars
    /* graphBar      */ RGB(  0, 113, 227),  // #0071E3 blue bars
    /* selection     */ RGB(211, 227, 255),  // #D3E3FF light-blue selection

    /* windowBase    */ RGB(244, 245, 247),  // #F4F5F7 professional neutral canvas
    /* panelSurface  */ RGB(255, 255, 255),  // #FFFFFF cards and navigation rail
    /* outputBg      */ RGB( 21,  27,  35),  // #151B23 high-contrast output console
    /* consoleBase   */ RGB( 21,  27,  35),  // #151B23 console frame
    /* chromeAccent  */ RGB( 76, 154, 232),  // #4C9AE8 sky blue (rings/borders)
    /* chromeText    */ RGB(110, 110, 115),  // #6E6E73 eyebrow gray

    /* btnTop        */ RGB(255, 255, 255),  // #FFFFFF bright white card top (pops off base)
    /* btnBottom     */ RGB(242, 247, 253),  // #F2F7FD faint cool bottom
    /* btnBorder     */ RGB(186, 199, 220),  // #BAC7DC soft blue border
    /* btnBorderFocus*/ RGB(  0, 113, 227),  // #0071E3 blue focus ring
    /* btnStripe     */ RGB(  0, 113, 227),  // #0071E3 blue stripe
    /* btnNumber     */ RGB(  0, 113, 227),  // #0071E3 blue number
    /* btnLabel      */ RGB( 29,  29,  31),  // #1D1D1F
    /* disabledText  */ RGB(176, 176, 181),  // #B0B0B5

    /* dlgBack       */ RGB(236, 236, 238),  // #ECECEE
    /* dlgPanel      */ RGB(255, 255, 255),  // #FFFFFF
    /* dlgEdit       */ RGB(245, 248, 254),  // #F5F8FE soft-white field
    /* dlgText       */ RGB( 29,  29,  31),  // #1D1D1F
    /* dlgPrompt     */ RGB( 58,  58,  60),  // #3A3A3C
    /* dlgAccent     */ RGB(  0, 113, 227),  // #0071E3 blue
    /* dlgBtnTop     */ RGB(251, 252, 255),  // #FBFCFF soft-white
    /* dlgBtnBottom  */ RGB(234, 241, 252),  // #EAF1FC faint cool
    /* dlgBtnBorder  */ RGB(198, 211, 230),  // #C6D3E6 soft blue border
    /* dlgBtnText    */ RGB( 29,  29,  31),  // #1D1D1F

    /* artTint       */ RGB(221, 227, 236),  // #DDE3EC vestigial; harmless
    /* artTintStrength*/ 0,
};

// The live palette (defaults to Apple Light; InitializeTheme() re-applies
// whatever is persisted).
ThemeId g_activeId = ThemeId::AppleLight;
Palette g_active   = kAppleLight;

constexpr wchar_t kRegSubKey[]    = L"Software\\OptiScan";
constexpr wchar_t kRegThemeValue[] = L"Theme";

}  // namespace

const Palette& PaletteFor(ThemeId id) {
    switch (id) {
    case ThemeId::Graphite:         return kGraphite;
    case ThemeId::CatppuccinFrappe: return kCatppuccinFrappe;
    case ThemeId::Nord:             return kNord;
    case ThemeId::ArcDark:          return kArcDark;
    case ThemeId::AppleLight:       return kAppleLight;
    default:                        return kCatppuccinFrappe;
    }
}

const wchar_t* ThemeName(ThemeId id) {
    switch (id) {
    case ThemeId::Graphite:         return L"Graphite";
    case ThemeId::CatppuccinFrappe: return L"Catppuccin Frappé";
    case ThemeId::Nord:             return L"Nord";
    case ThemeId::ArcDark:          return L"Arc-Dark";
    case ThemeId::AppleLight:       return L"Apple Light";
    default:                        return L"Catppuccin Frappé";
    }
}

const Palette& ActiveTheme() { return g_active; }
ThemeId        CurrentThemeId() { return g_activeId; }

void SetActiveTheme(ThemeId id) {
    g_activeId = id;
    g_active   = PaletteFor(id);
    Console::ApplyThemePalette(g_active);   // truecolor console/graph palette
    OnThemeChangedUi();                       // chrome tables + repaint (GUI)
}

ThemeId LoadThemeIdFromRegistry() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return ThemeId::AppleLight;
    }
    DWORD value = 0;
    DWORD size  = sizeof(value);
    DWORD type  = 0;
    LSTATUS st = RegQueryValueExW(hKey, kRegThemeValue, nullptr, &type,
                                  reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS || type != REG_DWORD || value >= static_cast<DWORD>(kThemeCount)) {
        return ThemeId::AppleLight;
    }
    return static_cast<ThemeId>(value);
}

void SaveThemeIdToRegistry(ThemeId id) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return;
    }
    DWORD value = static_cast<DWORD>(id);
    RegSetValueExW(hKey, kRegThemeValue, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hKey);
}

void ApplyThemeAndPersist(ThemeId id) {
    SetActiveTheme(id);
    SaveThemeIdToRegistry(id);
}

void InitializeTheme() {
    SetActiveTheme(LoadThemeIdFromRegistry());
}
