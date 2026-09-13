#define NOMINMAX
#include "InterruptHandler.h"
#include <chrono>
#include "OpticalDrive.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstdint>

bool OpticalDrive::DetectHiddenTrack(DiscInfo& disc) {
    disc.hasHiddenTrack = false;
    disc.hiddenTrackChecked = false;
    if (disc.tracks.empty() || !disc.tracks[0].isAudio) return false;
    const DWORD end = disc.tracks[0].startLBA;
    if (end == 0) { disc.hiddenTrackChecked = true; return false; }
    // Preserve the whole leading region regardless of this diagnostic's result.
    disc.tracks[0].pregapLBA = 0;
    ScopedDriveSpeed restoreSpeed(m_drive);
    m_drive.SetSpeed(4);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool failedRead = false;
    std::vector<BYTE> data(AUDIO_SECTOR_SIZE);
    for (DWORD lba = 0; lba < end; ++lba) {
        if (g_interrupt.IsInterrupted() || std::chrono::steady_clock::now() >= deadline) {
            Console::Warning("Hidden-audio check incomplete; all leading audio will still be preserved in Include mode.\n");
            return false;
        }
        if (!m_drive.ReadSectorAudioOnly(lba, data.data())) { failedRead = true; continue; }
        for (size_t i = 0; i < data.size(); ++i) {
            if (data[i] != 0) {
                disc.hasHiddenTrack = true;
                disc.hiddenTrackChecked = true;
                Console::Info("Hidden Track 1 audio is present; its full program-area pregap is preserved.\n");
                return true;
            }
        }
    }
    disc.hiddenTrackChecked = !failedRead;
    if (failedRead) Console::Warning("Hidden-audio presence is unknown because some sectors could not be read.\n");
    else Console::Info("Track 1 program-area pregap was checked in full and contains digital silence.\n");
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
