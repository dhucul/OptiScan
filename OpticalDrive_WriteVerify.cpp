#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleColors.h"
#include "Progress.h"
#include "WriteDiscInternal.h"
#include "WorkflowChecks.h"
#include "InterruptHandler.h"
#include <iostream>
#include <windows.h>

// ============================================================================
// VerifyWriteCompletion - Flush cache and close session
// ============================================================================
bool OpticalDrive::VerifyWriteCompletion(const std::wstring& /*binFile*/) {
    Console::Info("Flushing write cache...\n");
    const bool flushed = WriteDiscInternal::SynchronizeCache(m_drive);
    if (!flushed) Console::Warning("Cache flush failed; checking final session state.\n");
    if (!WriteDiscInternal::WaitForDriveReady(m_drive, 60)) {
        Console::Error("Drive did not become ready for session finalization.\n");
        return false;
    }
    BYTE closeCmd[10] = {0x5B, 0, 2};
    BYTE sk = 0, asc = 0, ascq = 0;
    const bool closed = m_drive.SendSCSIWithSense(closeCmd, sizeof(closeCmd),
        nullptr, 0, &sk, &asc, &ascq, false);
    // SAO may have finalized already, and asynchronous close may be busy.
    // Neither case proves success until READ DISC INFORMATION confirms it.
    if (!closed && sk != 0x05 && !(sk == 0x02 && asc == 0x04)) {
        Console::Error("Session close failed.\n");
        return false;
    }
    for (int attempt = 0; attempt < 120; ++attempt) {
        if (g_interrupt.IsInterrupted()) return false;
        if (m_drive.TestUnitReady()) {
            BYTE command[10] = {0x51, 0, 0, 0, 0, 0, 0, 0, 34, 0};
            BYTE data[34]{};
            if (m_drive.SendSCSI(command, sizeof(command), data, sizeof(data)) &&
                WorkflowChecks::DiscSessionComplete(data, sizeof(data))) {
                Console::Success("Disc and last session are finalized (content not read back).\n");
                return true;
            }
        }
        Sleep(1000);
    }
    Console::Error("Could not confirm a complete disc/session.\n");
    return false;
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
