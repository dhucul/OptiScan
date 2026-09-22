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

inline constexpr std::uint32_t kCdScanReadChunkSectors = 16;

// Continue across defective chunks so the decoder can collect error evidence,
// while keeping complete coverage separate from merely attempting a range.
template<class Reader>
bool ReadCdScanChunks(std::uint32_t lba, std::uint32_t sectors, Reader&& read) {
	if (sectors == 0 || std::uint64_t{lba} + sectors > 0x100000000ULL) return false;
	bool complete = true;
	for (std::uint32_t offset = 0; offset < sectors;) {
		const auto remaining = sectors - offset;
		const auto count = remaining < kCdScanReadChunkSectors ? remaining : kCdScanReadChunkSectors;
		if (!read(lba + offset, count)) complete = false;
		offset += count;
	}
	return complete;
}
