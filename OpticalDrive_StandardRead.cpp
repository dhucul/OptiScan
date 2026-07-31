#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include <iostream>
#include <chrono>

// ============================================================================
// Standard Disc Reading Mode
// ============================================================================

bool OpticalDrive::ReadDisc(DiscInfo& disc, int errorMode, std::function<void(int, int)> progress) {
	// Lock the tray for the duration of the rip so an accidental eject-button
	// press or OS eject can't interrupt it. Unlocked automatically on return.
	DriveDoorLockGuard doorLock(m_drive);
	DWORD total = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		if (disc.selectedSession > 0 && disc.tracks[i].session != disc.selectedSession) continue;

		DWORD start;
		if (disc.pregapMode == PregapMode::Skip) {
			start = disc.tracks[i].startLBA;
		}
		else {
			start = disc.tracks[i].pregapLBA;
		}

		DWORD count = disc.tracks[i].endLBA - start + 1;
		if (total > UINT32_MAX - count) {
			std::cerr << "Error: Disc too large\n";
			return false;
		}
		total += count;
	}

	try {
		disc.rawSectors.clear();
		disc.rawSectors.reserve(total);
	}
	catch (const std::bad_alloc&) {
		std::cerr << "Error: Not enough memory for " << total << " sectors\n";
		return false;
	}

	disc.errorCount = 0;
	disc.badSectors.clear();
	disc.readLog.clear();
	if (disc.loggingOutput != LogOutput::None) disc.readLog.reserve(total);

	std::cout << "  (Press ESC or Ctrl+C to cancel)\n" << std::flush;

	if (progress) progress(0, total);

	DWORD cur = 0;
	int totalRetries = 0;

	for (size_t i = 0; i < disc.tracks.size(); i++) {
		auto& t = disc.tracks[i];
		if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;

		DWORD start;
		if (disc.pregapMode == PregapMode::Skip) {
			start = t.startLBA;
		}
		else {
			start = t.pregapLBA;
		}

		int sectorSize = disc.includeSubchannel ? RAW_SECTOR_SIZE : AUDIO_SECTOR_SIZE;

		// Allocate once outside the sector loop
		std::vector<BYTE> sec(sectorSize, 0);

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				g_interrupt.SetInterrupted(true);
				std::cout << "\n\n*** Read cancelled ***\n";
				return false;
			}

			// Defeat cache every N sectors instead of every sector.
			// Drive caches are typically 0.5-8 MB (~256-4096 sectors).
			// Defeating every ~20 sectors balances accuracy vs. speed.
			constexpr DWORD CACHE_DEFEAT_INTERVAL = 20;
			if (disc.enableCacheDefeat && cur > 0 && (cur % CACHE_DEFEAT_INTERVAL) == 0) {
				DefeatDriveCache(lba, disc.leadOutLBA);
			}

			std::fill(sec.begin(), sec.end(), 0);
			int retries = 0, c2Errors = 0;
			auto sectorStart = std::chrono::steady_clock::now();
			bool ok = ReadSectorWithRetry(lba, sec.data(), sectorSize, t.isAudio,
				disc.includeSubchannel, retries, disc.enableC2Detection && t.isAudio, &c2Errors);
			double sectorTime = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - sectorStart).count();

			totalRetries += retries;
			if (c2Errors > 0) {
				disc.c2ErrorSectors.push_back(lba);
				disc.totalC2Errors += c2Errors;
			}

			if (!ok) {
				disc.errorCount++;
				disc.badSectors.push_back(lba);
				std::cerr << "\n  Read error at LBA " << lba << " (Track " << t.trackNumber << ")";
				if (errorMode == 1) { std::cerr << " - Aborting\n"; return false; }
				else if (errorMode == 2) std::cerr << " - Filled with silence\n";
				else { std::cerr << " - Skipped\n"; cur++; if (progress && (cur & 63) == 0) progress(cur, total); continue; }
			}

			bool logToFile = (disc.loggingOutput == LogOutput::File || disc.loggingOutput == LogOutput::Both);

			if (logToFile) {
				disc.readLog.emplace_back(lba, t.trackNumber, sectorTime);
			}

			disc.rawSectors.push_back(sec);
			cur++;
			if (progress && (cur & 63) == 0) progress(cur, total);
		}
	}

	// Ensure progress bar reaches 100%
	if (progress) progress(total, total);

	if (totalRetries > 0 || disc.errorCount > 0) {
		std::cout << "\n  Retries: " << totalRetries << ", Unrecoverable: " << disc.errorCount << "\n";
	}

	return true;
}
