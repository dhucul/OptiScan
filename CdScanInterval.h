#pragma once
#include <cstdint>

// Explicit geometry for a host-driven CD quality sample. Positions always
// identify the first sector, including the final (possibly partial) interval.
struct CdScanInterval {
	std::uint32_t startLba = 0;
	std::uint32_t sectors = 0;
	bool final = true;

	static constexpr CdScanInterval At(std::uint32_t cursor, std::uint32_t endLba) {
		if (cursor > endLba) return {cursor, 0, true};
		const std::uint64_t remaining = std::uint64_t{endLba} - cursor + 1;
		return {cursor, static_cast<std::uint32_t>(remaining < 75 ? remaining : 75),
			remaining <= 75};
	}
};
