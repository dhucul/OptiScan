#define NOMINMAX
#include "OpticalDrive.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>

// ============================================================================
// Utility Functions
// ============================================================================

bool OpticalDrive::DefeatDriveCache(DWORD currentLBA, DWORD maxLBA) {
	constexpr DWORD CACHE_DEFEAT_DISTANCE = 750;

	DWORD farLBA;

	if (currentLBA > CACHE_DEFEAT_DISTANCE) {
		farLBA = currentLBA - CACHE_DEFEAT_DISTANCE;
	}
	else if (maxLBA > 0 && currentLBA + CACHE_DEFEAT_DISTANCE * 2 < maxLBA) {
		farLBA = currentLBA + CACHE_DEFEAT_DISTANCE;
	}
	else {
		// currentLBA <= CACHE_DEFEAT_DISTANCE here, so jump forward
		farLBA = currentLBA + CACHE_DEFEAT_DISTANCE;
		if (maxLBA > 0 && farLBA >= maxLBA) {
			farLBA = maxLBA > CACHE_DEFEAT_DISTANCE ? maxLBA - CACHE_DEFEAT_DISTANCE : 0;
		}
	}

	if (m_drive.SeekToLBA(farLBA)) {
		return true;
	}

	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
	return m_drive.ReadSectorAudioOnly(farLBA, buf.data());
}

uint32_t OpticalDrive::HashSector(const BYTE* data, int size) {
	uint32_t hash = 2166136261u;
	for (int i = 0; i < size; i++) {
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

void OpticalDrive::ApplyOffsetCorrection(DiscInfo& disc) {
	if (disc.driveOffset == 0 || disc.rawSectors.empty()) return;
	std::cout << "\nApplying offset correction: " << disc.driveOffset << " samples\n";

	int64_t byteOffset = static_cast<int64_t>(disc.driveOffset) * 4;

	int64_t maxOffset = static_cast<int64_t>(disc.rawSectors.size()) * AUDIO_SECTOR_SIZE;
	if (std::abs(byteOffset) >= maxOffset) {
		std::cerr << "Warning: Offset too large, skipping correction\n";
		return;
	}

	std::vector<BYTE> allAudio;
	allAudio.reserve(disc.rawSectors.size() * AUDIO_SECTOR_SIZE);

	for (auto& sec : disc.rawSectors)
		allAudio.insert(allAudio.end(), sec.begin(), sec.begin() + AUDIO_SECTOR_SIZE);

	std::vector<BYTE> corrected;
	if (byteOffset > 0) {
		corrected.assign(allAudio.begin() + static_cast<size_t>(byteOffset), allAudio.end());
		corrected.resize(allAudio.size(), 0);
	}
	else {
		corrected.resize(static_cast<size_t>(-byteOffset), 0);
		corrected.insert(corrected.end(), allAudio.begin(), allAudio.end() + byteOffset);
	}

	size_t pos = 0;
	for (auto& sec : disc.rawSectors) {
		if (pos + AUDIO_SECTOR_SIZE <= corrected.size())
			memcpy(sec.data(), &corrected[pos], AUDIO_SECTOR_SIZE);
		pos += AUDIO_SECTOR_SIZE;
	}
}