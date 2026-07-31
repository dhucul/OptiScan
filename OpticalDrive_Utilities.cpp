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

	// Build the track type for each cached sector. Data sectors have a
	// firmware-aligned sync/header/EDC/ECC structure and must never be shifted
	// as PCM. Apply the sample offset independently to each contiguous audio
	// run, leaving data and all P-W subchannel bytes untouched.
	std::vector<bool> isAudio;
	isAudio.reserve(disc.rawSectors.size());
	for (const auto& track : disc.tracks) {
		if (disc.selectedSession > 0 && track.session != disc.selectedSession) continue;
		DWORD start = disc.pregapMode == PregapMode::Skip
			? track.startLBA : track.pregapLBA;
		if (track.endLBA < start) continue;
		for (DWORD lba = start;; ++lba) {
			isAudio.push_back(track.isAudio);
			if (lba == track.endLBA) break;
		}
	}
	if (isAudio.size() != disc.rawSectors.size()) {
		std::cerr << "Warning: Sector map mismatch, skipping offset correction\n";
		return;
	}

	for (size_t begin = 0; begin < isAudio.size();) {
		while (begin < isAudio.size() && !isAudio[begin]) ++begin;
		if (begin >= isAudio.size()) break;
		size_t end = begin;
		while (end < isAudio.size() && isAudio[end]) ++end;

		const size_t runBytes = (end - begin) * AUDIO_SECTOR_SIZE;
		if (static_cast<uint64_t>(std::abs(byteOffset)) >= runBytes) {
			std::cerr << "Warning: Offset exceeds an audio run; that run was not shifted\n";
			begin = end;
			continue;
		}

		std::vector<BYTE> audio;
		audio.reserve(runBytes);
		for (size_t i = begin; i < end; ++i)
			audio.insert(audio.end(), disc.rawSectors[i].begin(),
				disc.rawSectors[i].begin() + AUDIO_SECTOR_SIZE);

		std::vector<BYTE> corrected;
		if (byteOffset > 0) {
			corrected.assign(audio.begin() + static_cast<size_t>(byteOffset), audio.end());
			corrected.resize(audio.size(), 0);
		}
		else {
			size_t padding = static_cast<size_t>(-byteOffset);
			corrected.resize(padding, 0);
			corrected.insert(corrected.end(), audio.begin(), audio.end() - padding);
		}

		size_t position = 0;
		for (size_t i = begin; i < end; ++i) {
			memcpy(disc.rawSectors[i].data(), corrected.data() + position,
				AUDIO_SECTOR_SIZE);
			position += AUDIO_SECTOR_SIZE;
		}
		begin = end;
	}
}
