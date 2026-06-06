#include "OpticalDrive.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstdint>

bool OpticalDrive::DetectHiddenTrack(DiscInfo& disc) {
	if (disc.tracks.empty() || !disc.tracks[0].isAudio) return false;

	DWORD track1Start = disc.tracks[0].startLBA;
	if (track1Start <= 150) return false;

	std::cout << "\nChecking for hidden track audio...\n";
	m_drive.SetSpeed(4);

	DWORD htStart = 150;
	bool hasAudio = false;

	for (DWORD lba = htStart; lba < track1Start && lba < htStart + 75; lba++) {
		std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
		if (m_drive.ReadSectorAudioOnly(lba, buf.data())) {
			bool silent = true;
			for (size_t i = 0; i < AUDIO_SECTOR_SIZE && silent; i += 4) {
				int16_t left = *reinterpret_cast<int16_t*>(buf.data() + i);
				int16_t right = *reinterpret_cast<int16_t*>(buf.data() + i + 2);
				if (std::abs(left) > 100 || std::abs(right) > 100) {
					silent = false;
				}
			}
			if (!silent) {
				hasAudio = true;
				break;
			}
		}
	}

	m_drive.SetSpeed(0);

	if (hasAudio) {
		int frames = static_cast<int>(track1Start - 150);
		int seconds = frames / 75;
		Console::SetColor(Console::Color::Yellow);
		std::cout << "  Hidden track detected! Length: " << seconds / 60 << ":"
			<< std::setfill('0') << std::setw(2) << seconds % 60 << "."
			<< std::setw(2) << frames % 75 << std::setfill(' ')
			<< " (" << frames << " frames)\n";
		Console::Reset();

		// Preserve the subchannel-detected pregap boundary from ReadTOC.
		// Only ensure it includes the HTOA region (at minimum LBA 0).
		if (disc.tracks[0].pregapLBA > 0) {
			disc.tracks[0].pregapLBA = 0;
		}
		disc.hasHiddenTrack = true;
		return true;
	}

	std::cout << "  No hidden track audio found.\n";
	return false;
}

bool OpticalDrive::DetectHiddenLastTrack(DiscInfo& disc) {
	if (disc.tracks.empty() || disc.leadOutLBA == 0) return false;

	const TrackInfo& lastTrack = disc.tracks.back();
	if (!lastTrack.isAudio) return false;

	DWORD searchStart = lastTrack.endLBA + 1;
	if (searchStart >= disc.leadOutLBA) return false;

	std::cout << "\nChecking for hidden audio after last track...\n";
	m_drive.SetSpeed(4);

	bool hasAudio = false;
	DWORD scanLimit = std::min(searchStart + 75, disc.leadOutLBA);

	for (DWORD lba = searchStart; lba < scanLimit; lba++) {
		std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
		if (m_drive.ReadSectorAudioOnly(lba, buf.data())) {
			bool silent = true;
			for (size_t i = 0; i < AUDIO_SECTOR_SIZE && silent; i += 4) {
				int16_t left = *reinterpret_cast<int16_t*>(buf.data() + i);
				int16_t right = *reinterpret_cast<int16_t*>(buf.data() + i + 2);
				if (std::abs(left) > 100 || std::abs(right) > 100) {
					silent = false;
				}
			}
			if (!silent) {
				hasAudio = true;
				break;
			}
		}
	}

	m_drive.SetSpeed(0);

	if (hasAudio) {
		int frames = static_cast<int>(disc.leadOutLBA - searchStart);
		int seconds = frames / 75;
		Console::SetColor(Console::Color::Yellow);
		std::cout << "  Hidden track after last track detected! Length: "
			<< seconds / 60 << ":"
			<< std::setfill('0') << std::setw(2) << seconds % 60 << "."
			<< std::setw(2) << frames % 75 << std::setfill(' ')
			<< " (" << frames << " frames)\n";
		Console::Reset();
		return true;
	}

	std::cout << "  No hidden audio after last track found.\n";
	return false;
}