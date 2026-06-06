#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleColors.h"
#include "WriteDiscInternal.h"
#include <iostream>
#include <windows.h>

// ============================================================================
// Helper: Wait for drive to become ready (poll TEST UNIT READY)
// ============================================================================
bool WriteDiscInternal::WaitForDriveReady(ScsiDrive& drive, int timeoutSeconds) {
	for (int i = 0; i < timeoutSeconds * 4; i++) {
		BYTE testCmd[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		BYTE sk = 0, asc = 0, ascq = 0;
		if (drive.SendSCSIWithSense(testCmd, sizeof(testCmd), nullptr, 0,
			&sk, &asc, &ascq, true)) {
			return true;
		}
		// NOT READY (0x02) with "becoming ready" (ASC 0x04) — keep polling
		if (sk == 0x02 && asc == 0x04) {
			Sleep(250);
			continue;
		}
		// UNIT ATTENTION (0x06) — transient after blank/OPC/media change, retry
		if (sk == 0x06) {
			Sleep(250);
			continue;
		}
		// NOT READY with "medium not present" (0x3A) — may be transient during
		// post-blank media re-detection on some drives
		if (sk == 0x02 && asc == 0x3A) {
			Sleep(500);
			continue;
		}
		return false;
	}
	return false;
}

// ============================================================================
// Helper: Flush drive write buffer via SYNCHRONIZE CACHE (0x35)
// ============================================================================
bool WriteDiscInternal::SynchronizeCache(ScsiDrive& drive) {
	BYTE cdb[10] = { 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	BYTE sk = 0, asc = 0, ascq = 0;
	if (!drive.SendSCSIWithSense(cdb, sizeof(cdb), nullptr, 0, &sk, &asc, &ascq, false)) {
		// Drive not ready or unit attention — wait and let finalization continue
		if ((sk == 0x02 && asc == 0x04) || sk == 0x06) {
			return WaitForDriveReady(drive, 60);
		}
		return false;
	}
	return true;
}

// ============================================================================
// Helper: Deinterleave raw P-W subchannel to packed format
// ============================================================================
void WriteDiscInternal::DeinterleaveSubchannel(const BYTE* raw, BYTE* packed) {
	memset(packed, 0, SUBCHANNEL_SIZE);
	for (int i = 0; i < 96; i++) {
		for (int ch = 0; ch < 8; ch++) {
			if (raw[i] & (0x80 >> ch)) {
				int outByte = ch * 12 + (i / 8);
				int outBit = 7 - (i % 8);
				packed[outByte] |= (1 << outBit);
			}
		}
	}
}

// ============================================================================
// Helper: Find which track owns a given bin-file sector position
// ============================================================================
size_t WriteDiscInternal::FindTrackForSector(
	const std::vector<OpticalDrive::TrackWriteInfo>& tracks,
	DWORD binSector, bool& isInPregap) {
	isInPregap = false;
	for (size_t i = tracks.size(); i > 0; i--) {
		size_t idx = i - 1;
		if (binSector >= tracks[idx].startLBA) return idx;
		if (tracks[idx].hasPregap && binSector >= tracks[idx].pregapLBA) {
			isInPregap = true;
			return idx;
		}
	}
	return 0;
}