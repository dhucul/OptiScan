// ============================================================================
// ConsoleFormat.h - GUI-shim version
//
// Tags status messages with a short label and writes them through std::cout,
// which is redirected to the GUI edit control by GuiSink.
// ============================================================================
#pragma once

#include "ConsoleColor.h"
#include <iostream>

namespace Console {
    inline void Success(const char* msg) { std::cout << "[OK] "    << msg; }
    inline void Error  (const char* msg) { std::cout << "[ERROR] " << msg; }
    inline void Warning(const char* msg) { std::cout << "[WARN] "  << msg; }
    inline void Info   (const char* msg) { std::cout << msg; }
    inline void Heading(const char* msg) { std::cout << msg; }
}
