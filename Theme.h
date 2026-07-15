// ============================================================================
// Theme.h - Single source of truth for OptiScan's colour palette.
//
// Historically the palette was copy-pasted across ConsoleColor.h,
// OutputControl.cpp, GuiSink.cpp, OptiScanUi.cpp and GuiInput.cpp, plus dozens
// of inline Gdiplus::Color(...) literals in OptiScanUiPaint.cpp. This module
// centralises every role into one `Palette` struct, defines the selectable
// themes, and drives a runtime switch:
//
//   SetActiveTheme(id) -> updates ActiveTheme() + Console::Theme, then calls
//   OnThemeChangedUi() (implemented in OptiScanUi.cpp) which refreshes the
//   per-module colour tables, rebuilds cached brushes and repaints.
//
// The choice persists to HKCU\Software\OptiScan\Theme (REG_DWORD).
// ============================================================================
#pragma once

#include <windows.h>

enum class ThemeId : int {
    Graphite         = 0,   // original dark graphite/slate look
    CatppuccinFrappe = 1,
    Nord             = 2,
    ArcDark          = 3,
    AppleLight       = 4,   // soft-white light theme over a purple canvas
};

constexpr int kThemeCount = 5;

// The theme used when nothing is persisted, or the persisted value is invalid.
// Single source of truth -- the initial palette, both registry fallbacks, and
// the PaletteFor/ThemeName out-of-range guards all derive from this. It used to
// be spelled out in six places and they drifted (two still named Catppuccin
// Frappe long after the default had moved on).
constexpr ThemeId kDefaultTheme = ThemeId::Graphite;

// Every colour role the app needs. All COLORREF (0x00BBGGRR via RGB()). Alpha
// for the semi-transparent GDI+ chrome is applied at the paint site, not here.
struct Palette {
    // --- Console / log text + data-viz roles (also feed Console::Theme) ------
    // These are all ink on outputBg, which is a dark console in every theme --
    // including Apple Light. They are NOT chrome roles; do not reuse them for
    // the nav rail or cards, whose surface inverts between light and dark.
    COLORREF fg;          // default body text
    COLORREF bright;      // headings / bright labels
    COLORREF dim;         // muted / indented text, graph axis
    COLORREF accentWarm;  // command echo (>>>), title, menu accent
    COLORREF ok;          // [OK]/[PASS] + graph "good"
    COLORREF warn;        // [WARN] + graph "moderate"
    COLORREF error;       // [ERROR]/[FAIL] + graph "bad"
    COLORREF cyan;        // [INFO]/neutral data
    COLORREF logInfo;     // [INFO] log tag; cyan on Apple Light, bright elsewhere
    COLORREF graphFrame;  // box-drawing frame chars
    COLORREF graphBar;    // block bar chars
    COLORREF selection;   // log selection highlight band

    // --- Backdrop: the instrumentation canvas behind the content area --------
    // Deliberately independent of the chrome surfaces below. Apple Light pairs
    // a saturated purple canvas with a near-white rail, so deriving the canvas
    // from a surface role would flatten it.
    COLORREF backdropTop;        // 12-degree gradient start (upper-left)
    COLORREF backdropBottom;     // gradient end (lower-right)
    COLORREF backdropDepth;      // vertical darkening pass; alpha at the site
    COLORREF backdropGlow;       // radial scan glow + the rays
    COLORREF backdropInstrument; // scan arcs, dot matrix, horizon, progress fill

    // --- Ink drawn directly on the backdrop ----------------------------------
    COLORREF canvasInk;       // page title, group section headings
    COLORREF canvasInkMuted;  // page subtitle

    // --- Chrome surfaces: navigation rail + command cards --------------------
    COLORREF surfaceRaised;  // rail fill + normal card face
    COLORREF surfaceSunken;  // selected nav pill + pressed card face
    COLORREF hairline;       // nav divider, sidebar rule, card border
    COLORREF shadowInk;      // card drop-shadow tint; alpha at the site

    // --- Ink on the chrome surfaces ------------------------------------------
    // These invert with the surface: near-black on Apple Light's white rail,
    // near-white on the dark themes' rails.
    COLORREF cardInk;       // brand, card title/label, "Drive ready"
    COLORREF cardInkMuted;  // unselected nav, sidebar detail, card description
    COLORREF disabledText;  // disabled card ink

    // --- Accent ---------------------------------------------------------------
    COLORREF accentPrimary;  // nav stripe/label, focus border, number, action pill
    COLORREF accentSurface;  // number badge + hero icon tile fill
    COLORREF onAccentInk;    // ink ON accentPrimary; white fails on light accents

    // --- Danger: the exit card ------------------------------------------------
    COLORREF dangerSurface;
    COLORREF dangerBorder;
    COLORREF dangerInk;

    // --- Output console + brand ring ------------------------------------------
    COLORREF outputBg;     // console/output card fill (OutputDark)
    COLORREF chromeAccent; // optical ring hub + arc
    COLORREF chromeText;   // optical ring strokes

    // --- Input dialog (GuiInput) --------------------------------------------
    COLORREF dlgBack;
    COLORREF dlgPanel;
    COLORREF dlgEdit;
    COLORREF dlgText;
    COLORREF dlgPrompt;
    COLORREF dlgAccent;
    COLORREF dlgBtnTop;
    COLORREF dlgBtnBottom;
    COLORREF dlgBtnBorder;
    COLORREF dlgBtnText;
};

// The live palette. Never returns a dangling reference — points at a static.
const Palette& ActiveTheme();
ThemeId        CurrentThemeId();
const wchar_t* ThemeName(ThemeId id);
const Palette& PaletteFor(ThemeId id);

// Switch the active theme. Updates ActiveTheme(), Console::Theme, every
// per-module colour table, then repaints. Safe to call before the main window
// exists (chrome tables are set; window invalidation is skipped).
void SetActiveTheme(ThemeId id);

// SetActiveTheme + persist to the registry. Use for user-driven selection.
void ApplyThemeAndPersist(ThemeId id);

// Forget the persisted choice and apply kDefaultTheme. Unlike picking the
// default from the menu, this leaves nothing saved, so the app keeps following
// kDefaultTheme if it ever changes.
void ResetThemeToDefault();

// Registry helpers (HKCU\Software\OptiScan\Theme). Load falls back to
// kDefaultTheme when the value is absent or invalid.
ThemeId LoadThemeIdFromRegistry();
void    SaveThemeIdToRegistry(ThemeId id);
void    ClearThemeIdFromRegistry();

// Load the persisted theme and apply it. Call once at startup, before the UI
// paints.
void InitializeTheme();

// Implemented in OptiScanUi.cpp: refresh chrome tables + repaint. Declared here
// so SetActiveTheme can drive the UI without OptiScanUi.cpp knowing about the
// core. A no-op is acceptable in non-GUI link contexts.
void OnThemeChangedUi();
