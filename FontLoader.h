// FontLoader.h - loads the bundled Inter + JetBrains Mono fonts (embedded in
// the executable as RCDATA) *privately* into this process, for both the GDI
// text paths (CreateFontW by face name) and the GDI+ paths (a
// PrivateFontCollection). Nothing is ever written to disk or installed into the
// user's Windows font folder.
//
// Both families are SIL Open Font License; see assets\fonts\*-LICENSE / *-OFL.
#pragma once

#include <windows.h>

namespace Gdiplus { class FontCollection; }

// Load the embedded fonts. Call exactly once, after GdiplusStartup and before
// any UI font creation or painting. Idempotent (later calls no-op).
void LoadBundledFonts(HINSTANCE hInstance);

// Release the GDI mem-font handles and the GDI+ private collection. Call before
// GdiplusShutdown.
void UnloadBundledFonts();

// --- Face-name resolvers ----------------------------------------------------
// Each returns the bundled family when it loaded successfully, otherwise the
// closest stock Windows face, so the UI still renders if embedding ever fails.
//
// Note the RIBBI split in both families: the semibold / medium weights ship as
// their *own* families, so they must be named exactly (not requested via a
// heavier lfWeight on the base family).

const wchar_t* UiFamily();          // proportional UI       : Inter  / Segoe UI
const wchar_t* UiSemiBoldFamily();  // semibold UI (buttons) : Inter SemiBold / Segoe UI Semibold
const wchar_t* MonoFamily();        // monospace (reg + bold): JetBrains Mono / Cascadia Mono
const wchar_t* MonoMediumFamily();  // monospace medium      : JetBrains Mono Medium / Cascadia Mono

// GDI+ private collection to pair with UiFamily() in a Gdiplus::FontFamily
// ctor. Returns nullptr when Inter is not loaded, so the stock fallback face
// resolves against the system collection instead.
const Gdiplus::FontCollection* UiFontCollection();
