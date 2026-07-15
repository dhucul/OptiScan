// Theme.cpp - palette definitions, runtime switch, and persistence.

#include "Theme.h"
#include "ConsoleColor.h"   // Console::ApplyThemePalette

namespace {

// --- Graphite (original dark graphite/slate) --------------------------------
constexpr Palette kGraphite = {
    .fg = RGB(216, 216, 211),
    .bright = RGB(206, 210, 212),
    .dim = RGB(118, 128, 138),
    .accentWarm = RGB(198, 178, 150),
    .ok = RGB(148, 192, 166),
    .warn = RGB(210, 174, 112),
    .error = RGB(222, 126, 122),
    .cyan = RGB(156, 168, 180),
    .logInfo = RGB(206, 210, 212),          // == bright
    .graphFrame = RGB(150, 160, 170),
    .graphBar = RGB(174, 182, 190),
    .selection = RGB( 58,  66,  78),

    .backdropTop = RGB( 30,  35,  44),      // lifted slate
    .backdropBottom = RGB(  9,  11,  15),
    .backdropDepth = RGB(  4,   6,   9),
    .backdropGlow = RGB(126, 178, 212),
    .backdropInstrument = RGB(150, 170, 190),

    .canvasInk = RGB(228, 232, 236),
    .canvasInkMuted = RGB(176, 186, 196),

    .surfaceRaised = RGB( 44,  50,  60),
    .surfaceSunken = RGB( 58,  65,  77),
    .hairline = RGB( 78,  86,  98),
    .shadowInk = RGB(  0,   0,   0),

    .cardInk = RGB(216, 222, 226),
    .cardInkMuted = RGB(150, 160, 172),
    .disabledText = RGB( 96, 100, 104),

    .accentPrimary = RGB(126, 178, 212),
    .accentSurface = RGB( 52,  60,  72),
    .onAccentInk = RGB(  8,  12,  18),      // dark on light steel
    .dangerSurface = RGB( 46,  28,  30),
    .dangerBorder = RGB(110,  62,  62),
    .dangerInk = RGB(222, 126, 122),

    .outputBg = RGB( 14,  17,  22),
    .chromeAccent = RGB(126, 178, 212),
    .chromeText = RGB(178, 188, 196),

    .dlgBack = RGB(  8,  10,  14),
    .dlgPanel = RGB( 28,  32,  38),
    .dlgEdit = RGB( 20,  24,  30),
    .dlgText = RGB(214, 220, 224),
    .dlgPrompt = RGB(190, 198, 206),
    .dlgAccent = RGB(142, 152, 164),
    .dlgBtnTop = RGB( 46,  50,  56),
    .dlgBtnBottom = RGB( 24,  27,  33),
    .dlgBtnBorder = RGB(146, 156, 166),
    .dlgBtnText = RGB(214, 220, 224),
};

// --- Catppuccin Frappé -------------------------------------------------------
constexpr Palette kCatppuccinFrappe = {
    .fg = RGB(198, 208, 245),               // text
    .bright = RGB(205, 214, 245),
    .dim = RGB(115, 121, 148),              // overlay0
    .accentWarm = RGB(239, 159, 118),       // peach
    .ok = RGB(166, 209, 137),               // green
    .warn = RGB(229, 200, 144),             // yellow
    .error = RGB(231, 130, 132),            // red
    .cyan = RGB(153, 209, 219),             // sky
    .logInfo = RGB(205, 214, 245),          // == bright
    .graphFrame = RGB( 98, 104, 128),       // surface2
    .graphBar = RGB(133, 193, 220),
    .selection = RGB( 81,  87, 106),        // surface1

    .backdropTop = RGB( 62,  52,  86),      // mauve-shifted lift
    .backdropBottom = RGB( 30,  32,  44),   // mantle
    .backdropDepth = RGB( 24,  26,  36),
    .backdropGlow = RGB(202, 158, 230),     // mauve
    .backdropInstrument = RGB(180, 152, 220),

    .canvasInk = RGB(198, 208, 245),        // text
    .canvasInkMuted = RGB(165, 175, 215),

    .surfaceRaised = RGB( 56,  60,  80),    // base
    .surfaceSunken = RGB( 72,  76,  98),    // surface0
    .hairline = RGB( 88,  94, 114),         // surface1
    .shadowInk = RGB( 20,  22,  30),

    .cardInk = RGB(198, 208, 245),
    .cardInkMuted = RGB(165, 173, 206),
    .disabledText = RGB( 92,  97, 118),

    .accentPrimary = RGB(140, 170, 221),    // blue
    .accentSurface = RGB( 72,  76,  98),    // surface0
    .onAccentInk = RGB( 35,  38,  52),      // crust -- dark-on-accent, ~7:1
    .dangerSurface = RGB( 62,  45,  55),
    .dangerBorder = RGB(130,  78,  80),
    .dangerInk = RGB(231, 130, 132),        // red

    .outputBg = RGB( 48,  52,  70),         // base
    .chromeAccent = RGB(140, 170, 221),     // blue
    .chromeText = RGB(181, 191, 231),       // subtext1

    .dlgBack = RGB( 35,  38,  52),
    .dlgPanel = RGB( 41,  44,  59),
    .dlgEdit = RGB( 30,  32,  44),
    .dlgText = RGB(198, 208, 245),
    .dlgPrompt = RGB(181, 191, 231),
    .dlgAccent = RGB(140, 170, 221),
    .dlgBtnTop = RGB( 65,  69,  89),
    .dlgBtnBottom = RGB( 41,  44,  59),
    .dlgBtnBorder = RGB(115, 121, 148),
    .dlgBtnText = RGB(198, 208, 245),
};

// --- Nord -------------------------------------------------------------------
constexpr Palette kNord = {
    .fg = RGB(216, 222, 233),               // nord4
    .bright = RGB(236, 239, 244),           // nord6
    .dim = RGB( 97, 110, 136),
    .accentWarm = RGB(208, 135, 112),       // nord12 orange
    .ok = RGB(163, 190, 140),               // nord14
    .warn = RGB(235, 203, 139),             // nord13
    .error = RGB(191,  97, 106),            // nord11
    .cyan = RGB(136, 192, 208),             // nord8
    .logInfo = RGB(236, 239, 244),          // == bright (nord6)
    .graphFrame = RGB( 76,  86, 106),       // nord3
    .graphBar = RGB(143, 188, 187),         // nord7
    .selection = RGB( 67,  76,  94),        // nord2

    .backdropTop = RGB( 44,  54,  74),      // lifted nord10
    .backdropBottom = RGB( 28,  34,  45),
    .backdropDepth = RGB( 22,  27,  36),
    .backdropGlow = RGB(136, 192, 208),     // nord8
    .backdropInstrument = RGB(129, 161, 193), // nord9

    .canvasInk = RGB(236, 239, 244),        // nord6
    .canvasInkMuted = RGB(200, 212, 228),

    .surfaceRaised = RGB( 59,  66,  82),    // nord1
    .surfaceSunken = RGB( 76,  86, 106),    // nord2
    .hairline = RGB( 94, 104, 124),         // nord3
    .shadowInk = RGB( 20,  24,  31),

    .cardInk = RGB(236, 239, 244),          // nord6
    .cardInkMuted = RGB(178, 190, 208),
    .disabledText = RGB( 88,  96, 112),

    .accentPrimary = RGB(136, 192, 208),    // nord10
    .accentSurface = RGB( 67,  76,  94),    // nord2
    .onAccentInk = RGB( 46,  52,  64),      // nord6 -- white-on-nord9 is 2.4:1
    .dangerSurface = RGB( 60,  44,  50),
    .dangerBorder = RGB(120,  74,  80),
    .dangerInk = RGB(214, 134, 142),        // nord11

    .outputBg = RGB( 39,  44,  54),
    .chromeAccent = RGB(129, 161, 193),     // nord9
    .chromeText = RGB(216, 222, 233),

    .dlgBack = RGB( 34,  39,  49),
    .dlgPanel = RGB( 59,  66,  82),
    .dlgEdit = RGB( 40,  46,  57),
    .dlgText = RGB(216, 222, 233),
    .dlgPrompt = RGB(229, 233, 240),
    .dlgAccent = RGB(129, 161, 193),
    .dlgBtnTop = RGB( 67,  76,  94),
    .dlgBtnBottom = RGB( 59,  66,  82),
    .dlgBtnBorder = RGB( 76,  86, 106),
    .dlgBtnText = RGB(216, 222, 233),
};

// --- Arc-Dark ---------------------------------------------------------------
constexpr Palette kArcDark = {
    .fg = RGB(211, 218, 227),
    .bright = RGB(231, 235, 240),
    .dim = RGB(139, 145, 153),
    .accentWarm = RGB(240, 198, 116),       // subtle gold (Arc is mono-blue)
    .ok = RGB(126, 191, 106),
    .warn = RGB(240, 198, 116),
    .error = RGB(224, 108, 117),
    .cyan = RGB(108, 182, 227),
    .logInfo = RGB(231, 235, 240),          // == bright
    .graphFrame = RGB( 91,  98, 115),
    .graphBar = RGB(123, 168, 216),
    .selection = RGB( 69,  74,  90),

    .backdropTop = RGB( 38,  50,  72),
    .backdropBottom = RGB( 30,  33,  41),
    .backdropDepth = RGB( 24,  26,  33),
    .backdropGlow = RGB( 82, 148, 226),     // #5294e2
    .backdropInstrument = RGB(123, 168, 216),

    .canvasInk = RGB(231, 235, 240),
    .canvasInkMuted = RGB(186, 196, 210),

    .surfaceRaised = RGB( 57,  62,  75),    // #2f343f
    .surfaceSunken = RGB( 72,  78,  93),
    .hairline = RGB( 95, 102, 119),
    .shadowInk = RGB( 16,  18,  23),

    .cardInk = RGB(231, 235, 240),
    .cardInkMuted = RGB(170, 178, 190),
    .disabledText = RGB( 96, 101, 110),

    .accentPrimary = RGB(108, 168, 232),    // #5294e2
    .accentSurface = RGB( 56,  60,  74),    // #383c4a
    .onAccentInk = RGB( 16,  18,  23),      // Arc's own convention
    .dangerSurface = RGB( 52,  38,  42),
    .dangerBorder = RGB(122,  66,  72),
    .dangerInk = RGB(232, 124, 132),

    .outputBg = RGB( 56,  60,  74),         // #383c4a
    .chromeAccent = RGB( 82, 148, 226),     // #5294e2
    .chromeText = RGB(197, 205, 214),

    .dlgBack = RGB( 38,  41,  50),
    .dlgPanel = RGB( 47,  52,  63),
    .dlgEdit = RGB( 42,  46,  56),
    .dlgText = RGB(211, 218, 227),
    .dlgPrompt = RGB(189, 196, 206),
    .dlgAccent = RGB( 82, 148, 226),
    .dlgBtnTop = RGB( 64,  69,  82),
    .dlgBtnBottom = RGB( 47,  52,  63),
    .dlgBtnBorder = RGB( 91,  98, 115),
    .dlgBtnText = RGB(211, 218, 227),
};

// --- Apple Light (soft-white chrome, near-black text, blue accent) -----------
// The only light theme, and a hybrid: light chrome (nav rail, cards) wrapped
// around the same dark output console every other theme uses. That split is why
// the console roles below are light-on-dark like the dark themes' -- they are
// ink on outputBg (#151B23), not on the light chrome. The chrome's own ink
// lives in the card/canvas roles.
constexpr Palette kAppleLight = {
    .fg = RGB(210, 217, 226),               // console body text on #151B23
    .bright = RGB(245, 247, 250),           // console headings, "Output" title
    .dim = RGB(139, 150, 165),              // console muted text
    .accentWarm = RGB(255, 159,  67),       // #FF9F43 warm orange (title / >>>)
    .ok = RGB( 42, 158,  72),               // success green; also the ready dot
    .warn = RGB(190, 120,  24),             // balanced orange for graph severity
    .error = RGB(230,  64,  76),            // balanced red for graph severity
    .cyan = RGB(255, 177,  90),             // #FFB15A soft orange = [INFO]
    .logInfo = RGB(255, 177,  90),          // == cyan (dark themes use bright)
    .graphFrame = RGB(139, 150, 165),       // console box chars
    .graphBar = RGB(255, 159,  67),         // #FF9F43 orange bars
    .selection = RGB( 42,  65,  96),        // selection band on the dark console

    .backdropTop = RGB(100,  88, 180),      // saturated purple instrumentation
    .backdropBottom = RGB( 57,  49, 120),
    .backdropDepth = RGB( 40,  33,  91),
    .backdropGlow = RGB(151, 126, 255),     // lavender glow + rays
    .backdropInstrument = RGB(174, 151, 255), // arcs, dots, horizon, progress

    .canvasInk = RGB(255, 255, 255),        // page title on the purple canvas
    .canvasInkMuted = RGB(226, 220, 255),

    .surfaceRaised = RGB(247, 244, 251),    // lavender-white rail + card face
    .surfaceSunken = RGB(232, 226, 247),    // selected nav pill + pressed card
    .hairline = RGB(207, 201, 226),
    .shadowInk = RGB( 23,  32,  42),

    .cardInk = RGB( 23,  32,  42),
    .cardInkMuted = RGB( 53,  64,  82),
    .disabledText = RGB(152, 162, 179),

    .accentPrimary = RGB( 23, 105, 224),
    .accentSurface = RGB(235, 243, 255),
    .onAccentInk = RGB(255, 255, 255),
    .dangerSurface = RGB(253, 243, 247),
    .dangerBorder = RGB(246, 184, 194),
    .dangerInk = RGB(196,  35,  55),

    .outputBg = RGB( 21,  27,  35),         // #151B23 dark output console
    .chromeAccent = RGB( 76, 154, 232),     // #4C9AE8 -- ring hub/arc; kept
                                            // lighter than accentPrimary so it
                                            // stays legible on the purple canvas
    .chromeText = RGB(112, 126, 145),       // optical ring strokes

    .dlgBack = RGB(236, 236, 238),          // #ECECEE
    .dlgPanel = RGB(255, 255, 255),         // #FFFFFF
    .dlgEdit = RGB(245, 248, 254),          // #F5F8FE soft-white field
    .dlgText = RGB( 29,  29,  31),          // #1D1D1F
    .dlgPrompt = RGB( 58,  58,  60),        // #3A3A3C
    .dlgAccent = RGB( 23, 105, 224),        // == accentPrimary
    .dlgBtnTop = RGB(251, 252, 255),        // #FBFCFF soft-white
    .dlgBtnBottom = RGB(234, 241, 252),     // #EAF1FC faint cool
    .dlgBtnBorder = RGB(198, 211, 230),     // #C6D3E6 soft blue border
    .dlgBtnText = RGB( 29,  29,  31),       // #1D1D1F
};

// The live palette. InitializeTheme() re-applies whatever is persisted; this
// initial value only stands if the registry has nothing valid.
//
// constinit, not a PaletteFor(kDefaultTheme) call: a function call would make
// these dynamically initialized, and anything that touched ActiveTheme() from
// another TU's static initializer could then observe a zero-filled palette.
// constinit forces constant initialization and fails to compile if that ever
// stops holding. The static_assert is what keeps the hardcoded palette honest
// against kDefaultTheme.
static_assert(kDefaultTheme == ThemeId::Graphite,
              "g_active's initializer must name kDefaultTheme's palette");
constinit ThemeId g_activeId = kDefaultTheme;
constinit Palette g_active   = kGraphite;

constexpr wchar_t kRegSubKey[]    = L"Software\\OptiScan";
constexpr wchar_t kRegThemeValue[] = L"Theme";

}  // namespace

// Both switches deliberately omit `default:` so that adding a ThemeId fails to
// compile here rather than silently resolving to some other theme -- which is
// exactly how these two arms went on naming Catppuccin Frappe long after the
// default had moved. The trailing return only guards an out-of-range cast; it
// terminates immediately because kDefaultTheme is a handled enumerator.
const Palette& PaletteFor(ThemeId id) {
    switch (id) {
    case ThemeId::Graphite:         return kGraphite;
    case ThemeId::CatppuccinFrappe: return kCatppuccinFrappe;
    case ThemeId::Nord:             return kNord;
    case ThemeId::ArcDark:          return kArcDark;
    case ThemeId::AppleLight:       return kAppleLight;
    }
    return PaletteFor(kDefaultTheme);
}

const wchar_t* ThemeName(ThemeId id) {
    switch (id) {
    case ThemeId::Graphite:         return L"Graphite";
    case ThemeId::CatppuccinFrappe: return L"Catppuccin Frappé";
    case ThemeId::Nord:             return L"Nord";
    case ThemeId::ArcDark:          return L"Arc-Dark";
    case ThemeId::AppleLight:       return L"Apple Light";
    }
    return ThemeName(kDefaultTheme);
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
        return kDefaultTheme;
    }
    DWORD value = 0;
    DWORD size  = sizeof(value);
    DWORD type  = 0;
    LSTATUS st = RegQueryValueExW(hKey, kRegThemeValue, nullptr, &type,
                                  reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS || type != REG_DWORD || value >= static_cast<DWORD>(kThemeCount)) {
        return kDefaultTheme;
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

void ClearThemeIdFromRegistry() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return;   // nothing persisted: already following kDefaultTheme
    }
    RegDeleteValueW(hKey, kRegThemeValue);
    RegCloseKey(hKey);
}

void ApplyThemeAndPersist(ThemeId id) {
    SetActiveTheme(id);
    SaveThemeIdToRegistry(id);
}

void ResetThemeToDefault() {
    // Deliberately does NOT re-save: the point is to go back to *following*
    // kDefaultTheme, so that a future change to the default is picked up. If
    // this wrote the default's id instead, the user would be pinned to today's
    // default forever and the reset would be indistinguishable from choosing it
    // from the menu.
    ClearThemeIdFromRegistry();
    SetActiveTheme(kDefaultTheme);
}

void InitializeTheme() {
    SetActiveTheme(LoadThemeIdFromRegistry());
}
