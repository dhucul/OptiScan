#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include <iostream>

// ============================================================================
// Burst Mode Reading (Maximum Speed, No Verification)
// ============================================================================

bool OpticalDrive::ReadDiscBurst(DiscInfo& disc, std::function<void(int, int)> progress, int speedOverride, int errorMode) {
	// Lock the tray for the duration of the read (auto-unlocked on return, before
	// any caller-level eject such as the disc swap in Compare Disc CRCs).
	DriveDoorLockGuard doorLock(m_drive);
	DWORD total = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		if (disc.selectedSession > 0 && disc.tracks[i].session != disc.selectedSession) continue;
		DWORD start = (disc.pregapMode == PregapMode::Skip) ? disc.tracks[i].startLBA : disc.tracks[i].pregapLBA;
		total += disc.tracks[i].endLBA - start + 1;
	}

	try {
		disc.rawSectors.clear();
		disc.rawSectors.reserve(total);
	}
	catch (const std::bad_alloc&) {
		std::cerr << "Error: Not enough memory\n";
		return false;
	}

	m_drive.SetSpeed(speedOverride);   // 0 = max (original behaviour)
	std::cout << "  BURST MODE - " << (speedOverride == 0 ? "Maximum speed" : (std::to_string(speedOverride) + "x")) << ", no verification\n";
	if (disc.enableCacheDefeat) {
		std::cout << "  Cache defeat: ENABLED (will reduce speed)\n";
	}
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n";

	constexpr DWORD BATCH_SIZE = 26;
	constexpr DWORD CACHE_DEFEAT_INTERVAL = 20;

	DWORD cur = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		auto& t = disc.tracks[i];
		if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;
		DWORD start = (disc.pregapMode == PregapMode::Skip) ? t.startLBA : t.pregapLBA;
		DWORD trackSectors = t.endLBA - start + 1;
		bool canBatch = t.isAudio && !disc.includeSubchannel;

		for (DWORD offset = 0; offset < trackSectors; ) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				return false;
			}

			if (disc.enableCacheDefeat && cur > 0 && (cur % CACHE_DEFEAT_INTERVAL) == 0) {
				DefeatDriveCache(start + offset, disc.leadOutLBA);
			}

			// Batch read: audio-only tracks without subchannel
			if (canBatch) {
				DWORD remaining = trackSectors - offset;
				DWORD chunk = (remaining < BATCH_SIZE) ? remaining : BATCH_SIZE;

				std::vector<BYTE> batchBuf(AUDIO_SECTOR_SIZE * chunk);
				bool ok = m_drive.ReadSectorsAudioOnly(start + offset, chunk, batchBuf.data());

				if (ok) {
					for (DWORD s = 0; s < chunk; s++) {
						std::vector<BYTE> sec(AUDIO_SECTOR_SIZE);
						memcpy(sec.data(), batchBuf.data() + s * AUDIO_SECTOR_SIZE, AUDIO_SECTOR_SIZE);
						disc.rawSectors.push_back(std::move(sec));
					}
					offset += chunk;
					cur += chunk;
					if (progress) progress(cur, total);
					continue;
				}

				// Batch failed — fall through to single-sector reads for this chunk
				// so individual bad sectors can be identified
			}

			// Single-sector fallback (also used for data tracks / subchannel)
			DWORD lba = start + offset;
			int sectorSize = (disc.includeSubchannel && t.isAudio) ? RAW_SECTOR_SIZE : AUDIO_SECTOR_SIZE;
			std::vector<BYTE> sec(sectorSize, 0);
			bool ok = false;

			if (t.isAudio) {
				if (disc.includeSubchannel) {
					ok = m_drive.ReadSector(lba, sec.data(), sec.data() + AUDIO_SECTOR_SIZE);
				}
				else {
					ok = m_drive.ReadSectorAudioOnly(lba, sec.data());
				}
			}
			else {
				ok = m_drive.ReadDataSector(lba, sec.data());
			}

			if (!ok) {
				disc.errorCount++;
				disc.badSectors.push_back(lba);
				// errorMode 1 = "Abort on error": stop the rip on the first
				// unrecoverable sector (burst is single-pass, so there is no
				// recovery to wait for). Modes 2/3 zero-fill and keep reading.
				if (errorMode == 1) {
					std::cerr << "\n  Read error at LBA " << lba
						<< " (Track " << t.trackNumber
						<< ") - aborting (error handling = Abort)\n";
					return false;
				}
			}

			disc.rawSectors.push_back(std::move(sec));
			offset++;
			cur++;
			if (progress && (cur & 63) == 0) progress(cur, total);
		}
	}

	// Ensure progress bar reaches 100%
	if (progress) progress(total, total);

	return true;
}