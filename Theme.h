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
    CatppuccinFrappe = 1,   // default
    Nord             = 2,
    ArcDark          = 3,
};

constexpr int kThemeCount = 4;

// Every colour role the app needs. All COLORREF (0x00BBGGRR via RGB()). Alpha
// for the semi-transparent GDI+ chrome is applied at the paint site, not here.
struct Palette {
    // --- Console / log text + data-viz roles (also feed Console::Theme) ------
    COLORREF fg;          // default body text
    COLORREF bright;      // headings / bright labels
    COLORREF dim;         // muted / indented text, graph axis
    COLORREF accentWarm;  // command echo (>>>), title, menu accent
    COLORREF ok;          // [OK]/[PASS] + graph "good"
    COLORREF warn;        // [WARN] + graph "moderate"
    COLORREF error;       // [ERROR]/[FAIL] + graph "bad"
    COLORREF cyan;        // [INFO]/neutral data
    COLORREF graphFrame;  // box-drawing frame chars
    COLORREF graphBar;    // block bar chars
    COLORREF selection;   // log selection highlight band

    // --- Surfaces / chrome (GDI+ paint) -------------------------------------
    COLORREF windowBase;   // near-black behind everything
    COLORREF panelSurface; // rounded command/output panel fill
    COLORREF outputBg;     // console/output background (OutputDark)
    COLORREF consoleBase;  // console inner frame fill (near-black)
    COLORREF chromeAccent; // steel-blue accent family: borders/dots/tech lines
    COLORREF chromeText;   // "READY"/eyebrow grey

    // --- Command buttons -----------------------------------------------------
    COLORREF btnTop;        // gradient top (normal)
    COLORREF btnBottom;     // gradient bottom (normal)
    COLORREF btnBorder;     // border (normal)
    COLORREF btnBorderFocus;// border (focused)
    COLORREF btnStripe;     // left accent stripe (normal)
    COLORREF btnNumber;     // number text
    COLORREF btnLabel;      // label text
    COLORREF disabledText;  // disabled button text

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

    // --- Background artwork tint --------------------------------------------
    // The embedded top.png / output.png art is dark-navy with gold accents.
    // A luminance-colourise ColorMatrix shifts its hue toward artTint so it
    // matches the theme. artTintStrength is the blend amount, 0..100 (%); 0
    // draws the art untouched (used by Graphite, whose native tone already
    // matches).
    COLORREF artTint;
    int      artTintStrength;
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

// Registry helpers (HKCU\Software\OptiScan\Theme). Load defaults to the
// Catppuccin Frappé theme when the value is absent or invalid.
ThemeId LoadThemeIdFromRegistry();
void    SaveThemeIdToRegistry(ThemeId id);

// Load the persisted theme and apply it. Call once at startup, before the UI
// paints.
void InitializeTheme();

// Implemented in OptiScanUi.cpp: refresh chrome tables + repaint. Declared here
// so SetActiveTheme can drive the UI without OptiScanUi.cpp knowing about the
// core. A no-op is acceptable in non-GUI link contexts.
void OnThemeChangedUi();
