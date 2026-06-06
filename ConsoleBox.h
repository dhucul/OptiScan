// ============================================================================
// ConsoleBox.h - GUI-shim version
//
// Original drew Unicode box characters with ANSI colour. In the GUI the
// edit control uses a proportional system font, so the boxes are replaced
// with plain ASCII separators.
// ============================================================================
#pragma once

#include "ConsoleColor.h"
#include "ConsoleSymbols.h"
#include <iostream>
#include <cstring>

namespace Console {
    inline void BoxHeading(const char* title, int = 50) {
        std::cout << "\n=== " << title << " ===\n";
    }

    inline void BoxFooter(int = 50) {
        std::cout << std::string(50, '-') << "\n";
    }

    inline void BoxSeparator(int = 50) {
        std::cout << std::string(50, '-') << "\n";
    }

    inline void StatusOk  (const char* msg) { std::cout << " [OK]   " << msg << "\n"; }
    inline void StatusFail(const char* msg) { std::cout << " [FAIL] " << msg << "\n"; }
    inline void StatusWarn(const char* msg) { std::cout << " [WARN] " << msg << "\n"; }
}
