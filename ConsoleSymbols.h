// ============================================================================
// ConsoleSymbols.h - Unicode symbols and special characters
// ============================================================================
#pragma once

namespace Console {
	namespace Sym {
		// Status symbols
		constexpr const char* Check = "[OK]";
		constexpr const char* Cross = "[X]";
		constexpr const char* Warn = "[WARN]";
		constexpr const char* InfoSym = "[i]";
		constexpr const char* Bullet = "-";
		constexpr const char* Arrow = ">";
		constexpr const char* Disc = "[disc]";
		constexpr const char* Music = "[music]";

		// Box drawing
		constexpr const char* TopLeft = "\xe2\x94\x8c";
		constexpr const char* TopRight = "\xe2\x94\x90";
		constexpr const char* BottomLeft = "\xe2\x94\x94";
		constexpr const char* BottomRight = "\xe2\x94\x98";
		constexpr const char* Horizontal = "\xe2\x94\x80";
		constexpr const char* Vertical = "\xe2\x94\x82";
		constexpr const char* TeeRight = "\xe2\x94\x9c";
		constexpr const char* TeeLeft = "\xe2\x94\xa4";

		// Block characters (full and light)
		constexpr const char* BlockFull = "\xe2\x96\x88";
		constexpr const char* BlockLight = "\xe2\x96\x91";

		// Vertical block elements (1/8 to 8/8) for smooth bar graphs
		constexpr const char* Bar1 = "\xe2\x96\x81";  // ▁
		constexpr const char* Bar2 = "\xe2\x96\x82";  // ▂
		constexpr const char* Bar3 = "\xe2\x96\x83";  // ▃
		constexpr const char* Bar4 = "\xe2\x96\x84";  // ▄
		constexpr const char* Bar5 = "\xe2\x96\x85";  // ▅
		constexpr const char* Bar6 = "\xe2\x96\x86";  // ▆
		constexpr const char* Bar7 = "\xe2\x96\x87";  // ▇
		constexpr const char* Bar8 = "\xe2\x96\x88";  // █ (same as BlockFull)

		// Array for indexed access (0=empty, 1-8=fill levels)
		constexpr const char* BarLevels[] = {
			" ",
			"\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
			"\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"
		};
	}
}
