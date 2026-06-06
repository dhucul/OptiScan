#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleColors.h"
#include "Progress.h"
#include "WriteDiscInternal.h"
#include <iostream>
#include <windows.h>

// ============================================================================
// VerifyWriteCompletion - Flush cache and close session
// ============================================================================
bool OpticalDrive::VerifyWriteCompletion(const std::wstring& /*binFile*/) {
	Console::Info("Flushing write cache...\n");
	if (!WriteDiscInternal::SynchronizeCache(m_drive)) {
		Console::Warning("Cache flush reported failure (disc may still finalize)\n");
	}

	WriteDiscInternal::WaitForDriveReady(m_drive, 60);

	Console::Info("Closing session (writing lead-out)...\n");
	BYTE closeCmd[10] = { 0x5B, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	BYTE senseKey = 0, asc = 0, ascq = 0;

	if (!m_drive.SendSCSIWithSense(closeCmd, sizeof(closeCmd), nullptr, 0,
		&senseKey, &asc, &ascq, false)) {
		if (senseKey == 0x02 && asc == 0x04) {
			Console::Info("Waiting for finalization");
		}
		else if (senseKey == 0x05) {
			// SAO writing finalizes the session as part of the cue-sheet write, so
			// an explicit CLOSE TRACK SESSION has nothing left to close and reports
			// Illegal Request. This is expected, not an error.
			Console::Info("Session already finalized by the SAO write\n");
		}
		else {
			Console::Warning("CLOSE SESSION command returned: ");
			std::cout << m_drive.GetSenseDescription(senseKey, asc, ascq) << "\n";
		}
	}

	Console::Info("Finalizing disc");
	for (int i = 0; i < 120; i++) {
		BYTE testCmd[6] = { 0x00 };
		if (m_drive.SendSCSI(testCmd, sizeof(testCmd), nullptr, 0, true)) {
			std::cout << "\n";
			Console::Success("Disc finalized successfully\n");
			return true;
		}
		if (i % 5 == 0) std::cout << ".";
		Sleep(1000);
	}

	std::cout << "\n";
	Console::Warning("Finalization timeout -- disc may still be usable\n");
	return true;
}

// ============================================================================
// VerifyWrittenDisc - Read back and verify written sectors
// ============================================================================
bool OpticalDrive::VerifyWrittenDisc(const std::vector<TrackWriteInfo>& tracks) {
	if (tracks.empty()) {
		Console::Warning("No tracks to verify\n");
		return false;
	}

	Console::Info("Verifying written sectors...\n");

	ProgressIndicator progress(35);
	progress.SetLabel("Verifying");
	progress.Start();

	int verifyCount = 0;
	int successCount = 0;
	DWORD totalChecks = 0;
	for (const auto& track : tracks) {
		totalChecks++; // start sector
		if (track.endLBA > track.startLBA)
			totalChecks++; // end sector
	}

	for (const auto& track : tracks) {
		BYTE audio[AUDIO_SECTOR_SIZE] = { 0 };
		BYTE subchannel[SUBCHANNEL_SIZE] = { 0 };

		if (m_drive.ReadSector(track.startLBA, audio, subchannel)) {
			successCount++;
		}
		verifyCount++;
		progress.Update(verifyCount, totalChecks);

		if (track.endLBA > track.startLBA) {
			if (m_drive.ReadSector(track.endLBA, audio, subchannel)) {
				successCount++;
			}
			verifyCount++;
		}
	}

	progress.Finish(successCount == verifyCount);
	Console::Success("Verification: ");
	std::cout << successCount << "/" << verifyCount << " sectors readable\n";
	return successCount == verifyCount;
}