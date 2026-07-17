// FontLoader.cpp - see FontLoader.h.

#include "framework.h"      // pulls in <gdiplus.h>
#include "FontLoader.h"

#include <vector>

using namespace Gdiplus;

namespace {

bool                   g_loaded    = false;
bool                   g_haveInter = false;
bool                   g_haveMono  = false;
PrivateFontCollection* g_pfc       = nullptr;
std::vector<HANDLE>    g_memFonts;          // AddFontMemResourceEx handles

// Load one embedded RCDATA font blob into GDI (registered by face name, this
// process only) and into the GDI+ private collection. Returns true only when
// both paths accept it.
bool AddEmbeddedFont(HINSTANCE hInstance, const wchar_t* resName)
{
    HRSRC hRes = FindResourceW(hInstance, resName, RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hMem = LoadResource(hInstance, hRes);
    if (!hMem) return false;
    void*  data = LockResource(hMem);
    DWORD  size = SizeofResource(hInstance, hRes);
    if (!data || size == 0) return false;

    // GDI: registers the face for CreateFontW(...faceName...) across the
    // process. AddFontMemResourceEx copies the bytes, so the resource can stay
    // mapped where it is.
    DWORD  installed = 0;
    HANDLE h = AddFontMemResourceEx(data, size, nullptr, &installed);
    if (h) g_memFonts.push_back(h);

    // GDI+: the private collection copies the bytes too.
    Status st = g_pfc ? g_pfc->AddMemoryFont(data, size) : GenericError;

    return h != nullptr && st == Ok;
}

} // namespace

void LoadBundledFonts(HINSTANCE hInstance)
{
    if (g_loaded) return;
    g_loaded = true;

    g_pfc = new PrivateFontCollection();

    // Inter (proportional UI). The "Inter" family carries Regular + Bold;
    // SemiBold registers as its own family "Inter SemiBold".
    const bool interReg = AddEmbeddedFont(hInstance, L"INTER_REGULAR");
    const bool interSb  = AddEmbeddedFont(hInstance, L"INTER_SEMIBOLD");
    const bool interBd  = AddEmbeddedFont(hInstance, L"INTER_BOLD");
    g_haveInter = interReg && interSb && interBd;

    // JetBrains Mono (monospace). The "JetBrains Mono" family carries Regular +
    // Bold; Medium registers as its own family "JetBrains Mono Medium".
    const bool monoReg = AddEmbeddedFont(hInstance, L"JBMONO_REGULAR");
    const bool monoMed = AddEmbeddedFont(hInstance, L"JBMONO_MEDIUM");
    const bool monoBd  = AddEmbeddedFont(hInstance, L"JBMONO_BOLD");
    g_haveMono = monoReg && monoMed && monoBd;
}

void UnloadBundledFonts()
{
    for (HANDLE h : g_memFonts)
        if (h) RemoveFontMemResourceEx(h);
    g_memFonts.clear();

    delete g_pfc;
    g_pfc = nullptr;
    g_loaded = g_haveInter = g_haveMono = false;
}

const wchar_t* UiFamily()         { return g_haveInter ? L"Inter"                 : L"Segoe UI"; }
const wchar_t* UiSemiBoldFamily() { return g_haveInter ? L"Inter SemiBold"        : L"Segoe UI Semibold"; }
const wchar_t* MonoFamily()       { return g_haveMono  ? L"JetBrains Mono"        : L"Cascadia Mono"; }
const wchar_t* MonoMediumFamily() { return g_haveMono  ? L"JetBrains Mono Medium" : L"Cascadia Mono"; }

const Gdiplus::FontCollection* UiFontCollection()
{
    return g_haveInter ? g_pfc : nullptr;
}
