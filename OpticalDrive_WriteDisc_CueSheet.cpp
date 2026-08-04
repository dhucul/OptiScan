#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleColors.h"
#include "WriteDiscInternal.h"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <vector>
#include <windows.h>

// ============================================================================
// Helper: Convert LBA to MSF absolute disc time
// ============================================================================
static void LBAtoMSF(int lba, BYTE& m, BYTE& s, BYTE& f) {
	int absFrame = lba + 150;
	m = static_cast<BYTE>(absFrame / (75 * 60));
	s = static_cast<BYTE>((absFrame / 75) % 60);
	f = static_cast<BYTE>(absFrame % 75);
}

// ============================================================================
// Helper: Set Write Parameters Mode Page 0x05
// ============================================================================
static bool SetWriteParametersPage(ScsiDrive& drive, int subchannelMode, bool quiet = false) {
	BYTE modeData[60] = { 0 };

	BYTE* page = modeData + 8;
	page[0] = 0x05;
	page[1] = 0x32;

	BYTE expectedWriteType;
	BYTE expectedBlockType;

	switch (subchannelMode) {
	case 2:
		page[2] = 0x43;
		page[4] = 0x03;
		expectedWriteType = 0x03;
		expectedBlockType = 0x03;
		break;
	case 1:
		page[2] = 0x43;
		page[4] = 0x02;
		expectedWriteType = 0x03;
		expectedBlockType = 0x02;
		break;
	case 4:
		page[2] = 0x42;
		page[4] = 0x03;
		expectedWriteType = 0x02;
		expectedBlockType = 0x03;
		break;
	case 3:
		page[2] = 0x42;
		page[4] = 0x02;
		expectedWriteType = 0x02;
		expectedBlockType = 0x02;
		break;
	case 5:  // Raw without subchannel
		page[2] = 0x43;
		page[4] = 0x00;
		expectedWriteType = 0x03;
		expectedBlockType = 0x00;
		break;
	default:
		page[2] = 0x42;
		page[4] = 0x00;
		expectedWriteType = 0x02;
		expectedBlockType = 0x00;
		break;
	}

	// Optional dry-run: set the Test Write (simulate) bit -- page 05h byte 2 bit 4
	// -- when OPTISCAN_SIMULATE_WRITE is set to a non-zero value. The drive then
	// runs the entire write sequence (cue sheet, CD-Text lead-in, program area)
	// with the laser held at read power, so nothing is committed to the disc and
	// the pipeline can be exercised without consuming a blank. Requires a drive
	// that advertises Test Write (Drive capabilities -> "Test Write (Simulate)").
	// Post-write readback verification will report a mismatch in this mode (the
	// disc stays blank) -- that is expected for a simulated burn.
	{
		char sim[8] = {};
		DWORD simLen = GetEnvironmentVariableA("OPTISCAN_SIMULATE_WRITE", sim, sizeof(sim));
		if (simLen > 0 && simLen < sizeof(sim) && sim[0] != '0') {
			page[2] |= 0x10;  // Test Write (simulate)
			if (!quiet)
				Console::Warning("SIMULATE (Test Write) enabled -- nothing will be committed to the disc\n");
		}
	}

	page[3] = 0x00;
	page[5] = 0x00;
	page[8] = 0x00;

	WORD totalLen = 60;
	BYTE selectCdb[10] = { 0x55, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	selectCdb[7] = static_cast<BYTE>((totalLen >> 8) & 0xFF);
	selectCdb[8] = static_cast<BYTE>(totalLen & 0xFF);

	BYTE senseKey = 0, asc = 0, ascq = 0;
	if (!drive.SendSCSIWithSense(selectCdb, sizeof(selectCdb), modeData, totalLen,
		&senseKey, &asc, &ascq, false)) {
		if (!quiet) {
			Console::Error("MODE SELECT for Write Parameters failed (");
			std::cout << drive.GetSenseDescription(senseKey, asc, ascq) << ")\n";
		}
		return false;
	}

	BYTE senseCdb[10] = { 0x5A, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x00 };
	BYTE verifyBuf[60] = { 0 };
	if (drive.SendSCSI(senseCdb, sizeof(senseCdb), verifyBuf, sizeof(verifyBuf), true)) {
		WORD bdLen = (static_cast<WORD>(verifyBuf[6]) << 8) | verifyBuf[7];
		if (static_cast<size_t>(8) + bdLen + 5 > sizeof(verifyBuf)) {
			if (!quiet) Console::Warning("Drive returned an unusable Write Parameters page\n");
			// MODE SELECT itself succeeded; an unusable optional readback is not
			// proof that the drive rejected the requested write parameters.
			return true;
		}
		BYTE* vPage = verifyBuf + 8 + bdLen;
		BYTE writeType = vPage[2] & 0x0F;
		BYTE blockType = vPage[4];
		const char* modeName = (writeType == 0x03) ? "Raw" : "SAO";
		const char* subName = (blockType == 0x03) ? "raw P-W" :
			(blockType == 0x02) ? "packed P-W" : "none";
		int blockSize = (blockType >= 0x02) ? 2448 : (blockType == 0x01) ? 2368 : 2352;

		if (!quiet) {
			Console::Success("Write parameters verified (");
			std::cout << modeName << ", Audio, " << blockSize << "-byte blocks, sub: " << subName << ")\n";
		}

		if (writeType != expectedWriteType || blockType != expectedBlockType) {
			if (!quiet) {
				Console::Warning("Drive silently changed write parameters (requested write type 0x");
				std::cout << std::hex << std::setfill('0') << std::setw(2)
					<< static_cast<int>(expectedWriteType) << "/block 0x"
					<< std::setw(2) << static_cast<int>(expectedBlockType)
					<< ", got 0x" << std::setw(2) << static_cast<int>(writeType)
					<< "/0x" << std::setw(2) << static_cast<int>(blockType)
					<< std::dec << std::setfill(' ') << ")\n";
			}
			return false;
		}
	}

	return true;
}

// ============================================================================
// Helper: Get SCSI CUE sheet CTL/ADR byte for a track
// ============================================================================
static BYTE GetCueCtlAdr(bool isAudio) {
	// ADR=1 (position data) in lower nibble
	// CTL: 0x00 for audio, 0x04 for data
	return isAudio ? 0x01 : 0x41;
}

// ============================================================================
// Helper: Get SCSI CUE sheet Data Form byte for a track
// ============================================================================
static BYTE GetCueDataForm(bool isAudio, int dataMode, int subchannelMode) {
	// Per MMC SEND CUE SHEET / libburn: the cue-sheet DATA FORM describes the
	// *main channel* only -- 0x00 audio, 0x10 data MODE1, 0x20 data MODE2.
	// Sub-channel writing is selected by the mode-page block size (2448), NOT by
	// the cue sheet, so no sub bits belong here. Adding them (0x02/0x03) is what
	// made strict drives reject the subchannel cue sheets.
	(void)subchannelMode;
	if (isAudio)       return 0x00;
	if (dataMode == 2) return 0x20;
	return 0x10;
}

// ============================================================================
// Helper: Data Form for Lead-in and Lead-out entries (the "pause" form)
// ============================================================================
// Per MMC SEND CUE SHEET (and libburn's SAO cookbook), the Lead-in and
// Lead-out cue-sheet entries must carry the *pause* data form, NOT the payload
// form used by the tracks:
//     audio        -> 0x01   (0x00 is audio *payload*, which is wrong here)
//     data MODE1   -> 0x14
//     data MODE2   -> 0x24
// Lenient drives accept a 0x00 lead-in/lead-out, but strict drives (e.g. the
// Pioneer BDR-S13U) reject it with "invalid field in parameter list". Using
// the correct pause form is what lets the native SAO write -- and therefore
// the original pre-gaps -- succeed on those drives.
static BYTE GetCueLeadDataForm(bool isAudio, int dataMode) {
	if (isAudio)       return 0x01;
	if (dataMode == 2) return 0x24;
	return 0x14;
}

// ============================================================================
// Helper: Build and send SCSI CUE sheet
// ============================================================================
bool WriteDiscInternal::BuildAndSendCueSheet(ScsiDrive& drive,
	const std::vector<OpticalDrive::TrackWriteInfo>& tracks,
	DWORD totalSectors, int subchannelMode, bool verbose, bool quiet,
	bool cdTextInLeadIn) {

	if (tracks.empty()) {
		Console::Error("Cannot build CUE sheet: no tracks\n");
		return false;
	}

	size_t entryCount = 2;  // lead-in TOC + first track INDEX 00
	entryCount++;            // lead-out
	for (size_t i = 0; i < tracks.size(); i++) {
		entryCount++;        // INDEX 01
		if (i > 0 && tracks[i].hasPregap && tracks[i].pregapLBA < tracks[i].startLBA) {
			entryCount++;    // INDEX 00 for pregap
		}
	}

	size_t cueSheetSize = entryCount * 8;
	std::vector<BYTE> cueSheet(cueSheetSize, 0);
	size_t ei = 0;

	// Lead-in: CTL/ADR matches the first track; data form is the *pause* form.
	BYTE leadInCtlAdr = GetCueCtlAdr(tracks[0].isAudio);
	BYTE leadInDataForm = GetCueLeadDataForm(tracks[0].isAudio, tracks[0].dataMode);

	// Per the MMC SEND CUE SHEET spec (and the libburn SAO cookbook), the lead-in
	// cue entry's DATA FORM is OR'd with 0x40 to declare that CD-Text will be
	// written into the lead-in's R-W subchannel. Only then does the drive accept
	// the 96-byte CD-Text blocks the host writes ahead of the program area.
	if (cdTextInLeadIn) leadInDataForm |= 0x40;

	// Lead-in TOC entry
	cueSheet[ei * 8 + 0] = leadInCtlAdr;
	cueSheet[ei * 8 + 1] = 0x00;
	cueSheet[ei * 8 + 2] = 0x00;
	cueSheet[ei * 8 + 3] = leadInDataForm;
	ei++;

	// First track INDEX 00 (start of disc at 00:00:00)
	cueSheet[ei * 8 + 0] = GetCueCtlAdr(tracks[0].isAudio);
	cueSheet[ei * 8 + 1] = static_cast<BYTE>(tracks[0].trackNumber);
	cueSheet[ei * 8 + 2] = 0x00;
	cueSheet[ei * 8 + 3] = GetCueDataForm(tracks[0].isAudio, tracks[0].dataMode, subchannelMode);
	cueSheet[ei * 8 + 5] = 0x00;
	cueSheet[ei * 8 + 6] = 0x00;
	cueSheet[ei * 8 + 7] = 0x00;
	ei++;

	// Track entries
	for (size_t i = 0; i < tracks.size(); i++) {
		const auto& t = tracks[i];
		BYTE ctlAdr = GetCueCtlAdr(t.isAudio);
		BYTE dataForm = GetCueDataForm(t.isAudio, t.dataMode, subchannelMode);

		// INDEX 00 (pregap) for tracks after the first
		if (i > 0 && t.hasPregap && t.pregapLBA < t.startLBA) {
			BYTE m, s, f;
			LBAtoMSF(static_cast<int>(t.pregapLBA), m, s, f);

			cueSheet[ei * 8 + 0] = ctlAdr;
			cueSheet[ei * 8 + 1] = static_cast<BYTE>(t.trackNumber);
			cueSheet[ei * 8 + 2] = 0x00;
			cueSheet[ei * 8 + 3] = dataForm;
			cueSheet[ei * 8 + 5] = m;
			cueSheet[ei * 8 + 6] = s;
			cueSheet[ei * 8 + 7] = f;
			ei++;
		}

		// INDEX 01
		BYTE m, s, f;
		LBAtoMSF(static_cast<int>(t.startLBA), m, s, f);

		cueSheet[ei * 8 + 0] = ctlAdr;
		cueSheet[ei * 8 + 1] = static_cast<BYTE>(t.trackNumber);
		cueSheet[ei * 8 + 2] = 0x01;
		cueSheet[ei * 8 + 3] = dataForm;
		cueSheet[ei * 8 + 5] = m;
		cueSheet[ei * 8 + 6] = s;
		cueSheet[ei * 8 + 7] = f;
		ei++;
	}

	// Lead-out
	{
		BYTE m, s, f;
		LBAtoMSF(static_cast<int>(totalSectors), m, s, f);
		// Lead-out CTL matches the last track; data form is the *pause* form.
		BYTE lastCtlAdr = GetCueCtlAdr(tracks.back().isAudio);
		BYTE lastDataForm = GetCueLeadDataForm(tracks.back().isAudio, tracks.back().dataMode);
		cueSheet[ei * 8 + 0] = lastCtlAdr;
		cueSheet[ei * 8 + 1] = 0xAA;
		cueSheet[ei * 8 + 2] = 0x01;
		cueSheet[ei * 8 + 3] = lastDataForm;
		cueSheet[ei * 8 + 5] = m;
		cueSheet[ei * 8 + 6] = s;
		cueSheet[ei * 8 + 7] = f;
	}

	if (verbose) {
		Console::Info("SCSI CUE sheet layout:\n");
		for (size_t i = 0; i < entryCount; i++) {
			BYTE* e = &cueSheet[i * 8];
			if (e[1] == 0x00)       std::cout << "  Lead-in ";
			else if (e[1] == 0xAA)  std::cout << "  Lead-out";
			else                    std::cout << "  Track " << std::setw(2) << static_cast<int>(e[1]);
			std::cout << "  Index " << static_cast<int>(e[2])
				<< "  CTL 0x" << std::hex << std::setfill('0') << std::setw(2)
				<< static_cast<int>(e[0])
				<< "  DataForm 0x" << std::setw(2)
				<< static_cast<int>(e[3]) << std::dec
				<< "  MSF " << std::setfill('0') << std::setw(2) << static_cast<int>(e[5])
				<< ":" << std::setw(2) << static_cast<int>(e[6])
				<< ":" << std::setw(2) << static_cast<int>(e[7])
				<< std::setfill(' ') << "\n";
		}
	}
	else if (!quiet) {
		bool isMixedMode = std::any_of(tracks.begin(), tracks.end(),
			[](const OpticalDrive::TrackWriteInfo& t) { return !t.isAudio; });
		Console::Info("Sending CUE sheet (");
		std::cout << entryCount << " entries"
			<< (isMixedMode ? ", mixed-mode" : ", audio-only")
			<< ")...\n";
	}

	BYTE cdb[10] = { 0x5D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	DWORD size = static_cast<DWORD>(cueSheetSize);
	cdb[6] = static_cast<BYTE>((size >> 16) & 0xFF);
	cdb[7] = static_cast<BYTE>((size >> 8) & 0xFF);
	cdb[8] = static_cast<BYTE>(size & 0xFF);

	BYTE senseKey = 0, asc = 0, ascq = 0;
	if (!drive.SendSCSIWithSense(cdb, sizeof(cdb), cueSheet.data(), size,
		&senseKey, &asc, &ascq, false)) {
		if (!quiet) {
			Console::Error("SEND CUE SHEET failed (");
			std::cout << drive.GetSenseDescription(senseKey, asc, ascq) << ")\n";
		}
		return false;
	}

	if (!quiet) {
		Console::Success("CUE sheet accepted (");
		std::cout << entryCount << " entries, " << tracks.size() << " tracks)\n";
	}
	return true;
}

// ============================================================================
// Helper: Prepare drive for writing
// ============================================================================
bool WriteDiscInternal::PrepareDriveForWrite(ScsiDrive& drive, int subchannelMode, bool quiet) {
	if (!quiet) Console::Info("Checking drive readiness...\n");
	if (!WriteDiscInternal::WaitForDriveReady(drive, 15)) {
		if (!quiet) Console::Error("Drive did not become ready\n");
		return false;
	}
	if (!quiet) Console::Success("Drive is ready\n");

	if (!quiet) Console::Info("Configuring write parameters...\n");
	if (!SetWriteParametersPage(drive, subchannelMode, quiet)) {
		if (!quiet) Console::Error("Failed to configure write parameters\n");
		return false;
	}

	return true;
}
