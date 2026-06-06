// ============================================================================
// ConsoleColor.h - Colour helpers for the OptiScan GUI build
//
// The app is a Win32 GUI: stdout is redirected to a RichEdit control by
// GuiSink.  Colour calls emit ANSI 24-bit (truecolor) SGR escape sequences
// into the stream; GuiSink parses them and applies per-segment colour to the
// RichEdit text.  File output (std::ofstream) is unaffected because it does
// not flow through the cout redirect.
// ============================================================================
#pragma once

#include <windows.h>
#include <cstdio>
#include <iostream>

namespace Console {
    enum class Color : WORD {
        Black = 0,
        DarkBlue = 1,
        DarkGreen = 2,
        DarkCyan = 3,
        DarkRed = 4,
        DarkMagenta = 5,
        DarkYellow = 6,
        Gray = 7,
        DarkGray = 8,
        Blue = 9,
        Green = 10,
        Cyan = 11,
        Red = 12,
        Magenta = 13,
        Yellow = 14,
        White = 15
    };

    // Palette aligned with the graphite/slate output panel artwork and the
    // GuiSink Log* constants. Warm colours are restrained status accents;
    // most foreground text lives in cool greys so it matches the UI chrome.
    namespace Theme {
        constexpr int BgR = 14, BgG = 17, BgB = 22;
        constexpr int FgR = 216, FgG = 216, FgB = 211;
        constexpr int WhiteR = 204, WhiteG = 204, WhiteB = 198;
        constexpr int DimR = 118, DimG = 128, DimB = 138;
        constexpr int MenuR = 198, MenuG = 178, MenuB = 150;
        constexpr int YellowR = 210, YellowG = 174, YellowB = 112;
        constexpr int CyanR = 156, CyanG = 168, CyanB = 180;
        constexpr int GreenR = 148, GreenG = 192, GreenB = 166;
        constexpr int RedR = 222, RedG = 126, RedB = 122;
        constexpr int BorderR = 150, BorderG = 160, BorderB = 170;
        constexpr int GlowR = 198, GlowG = 178, GlowB = 150;
        constexpr int PanelR = 14, PanelG = 17, PanelB = 22;
        constexpr int SilverR = 174, SilverG = 182, SilverB = 190;
        constexpr int WaveR = 154, WaveG = 164, WaveB = 176;
        constexpr int LineR = 198, LineG = 178, LineB = 150;
    }

    // Build and write an SGR escape in a single std::cout operation so it
    // never gets split across flushes when unitbuf is enabled.
    inline void SetColorRGB(int r, int g, int b) {
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "\x1B[38;2;%d;%d;%dm", r, g, b);
        std::cout << buf;
    }

    inline void SetBgRGB(int r, int g, int b) {
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "\x1B[48;2;%d;%d;%dm", r, g, b);
        std::cout << buf;
    }

    inline void Reset() { std::cout << "\x1B[0m"; }

    namespace detail {
        // Map the legacy 16-colour enum to a truecolor triple that matches
        // the GUI's palette.
        inline void ColorToRGB(Color c, int& r, int& g, int& b) {
            switch (c) {
            case Color::Black:       r = 18;  g = 18;  b = 20;  break;
            case Color::DarkBlue:    r = 62;  g = 78;  b = 98;  break;
            case Color::DarkGreen:   r = 92;  g = 132; b = 108; break;
            case Color::DarkCyan:    r = 92;  g = 112; b = 126; break;
            case Color::DarkRed:     r = 150; g = 78;  b = 74;  break;
            case Color::DarkMagenta: r = 160; g = 96;  b = 168; break;
            case Color::DarkYellow:  r = 188; g = 156; b = 80;  break;
            case Color::Gray:        r = 168; g = 176; b = 184; break;
            case Color::DarkGray:    r = Theme::DimR;   g = Theme::DimG;   b = Theme::DimB;   break;
            case Color::Blue:        r = 142; g = 158; b = 184; break;
            case Color::Green:       r = Theme::GreenR; g = Theme::GreenG; b = Theme::GreenB; break;
            case Color::Cyan:        r = Theme::CyanR;  g = Theme::CyanG;  b = Theme::CyanB;  break;
            case Color::Red:         r = Theme::RedR;   g = Theme::RedG;   b = Theme::RedB;   break;
            case Color::Magenta:     r = 210; g = 130; b = 220; break;
            case Color::Yellow:      r = Theme::YellowR; g = Theme::YellowG; b = Theme::YellowB; break;
            case Color::White:       r = Theme::WhiteR; g = Theme::WhiteG; b = Theme::WhiteB; break;
            default:                 r = Theme::FgR;    g = Theme::FgG;    b = Theme::FgB;    break;
            }
        }
    }

    inline void SetColor(Color fg, Color = Color::Black) {
        int r, g, b;
        detail::ColorToRGB(fg, r, g, b);
        SetColorRGB(r, g, b);
    }

    // Retained no-ops for legacy CLI customisation.
    inline void ApplyDarkTheme() {}
    inline void SetFont(const wchar_t* = nullptr, short = 0) {}
    inline void SetFontBold(const wchar_t* = nullptr, short = 0) {}
    inline void SetWindowSize(short = 0, short = 0) {}
}
