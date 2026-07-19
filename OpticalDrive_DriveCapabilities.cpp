#define NOMINMAX
#include "OpticalDrive.h"
#include "PioneerVendor.h"
#include <iostream>
#include <iomanip>
#include <algorithm>

// ============================================================================
// Drive Capabilities Detection & Reporting
// ============================================================================

bool OpticalDrive::DetectDriveCapabilities(DriveCapabilities& caps) {
	// Delegate to the comprehensive ScsiDrive::DetectCapabilities
	// which queries INQUIRY, VPD 0x80, Mode Page 2A, GET PERFORMANCE,
	// C2 support, raw read, and overread capabilities
	bool result = m_drive.DetectCapabilities(caps);
	if (result) {
		m_hasAccurateStream = caps.supportsAccurateStream;
		m_capabilitiesDetected = true;
	}
	return result;
}

void OpticalDrive::PrintDriveCapabilities(const DriveCapabilities& caps) {
	auto yn = [](bool v) -> const char* { return v ? "YES" : "NO"; };

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              DRIVE CAPABILITIES REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	// --- Identification ---
	std::cout << "\n--- Identification ---\n";
	std::cout << "  Vendor:          " << caps.vendor << "\n";
	std::cout << "  Model:           " << caps.model << "\n";
	std::cout << "  Firmware:        " << (caps.firmware.empty() ? "(unknown)" : caps.firmware) << "\n";
	std::cout << "  Serial Number:   " << (caps.serialNumber.empty() ? "(not reported)" : caps.serialNumber) << "\n";

	// --- Core Ripping Capabilities ---
	std::cout << "\n--- Core Ripping Capabilities ---\n";
	std::cout << "  CD-DA Extraction:      " << yn(caps.supportsCDDA) << "\n";
	std::cout << "  Accurate Stream:       " << yn(caps.supportsAccurateStream) << "\n";
	std::cout << "  C2 Error Reporting:    " << yn(caps.supportsC2ErrorReporting) << "\n";
	std::cout << "  Raw Read:              " << yn(caps.supportsRawRead) << "\n";
	std::cout << "  CD-TEXT Reading:       " << yn(caps.supportsCDText) << "\n";

	// --- Subchannel & Overread ---
	std::cout << "\n--- Subchannel & Overread ---\n";
	std::cout << "  Raw Subchannel:        " << yn(caps.supportsSubchannelRaw) << "\n";
	std::cout << "  Q-Channel:             " << yn(caps.supportsSubchannelQ) << "\n";
	std::cout << "  Overread Lead-In:      " << yn(caps.supportsOverreadLeadIn) << "\n";
	std::cout << "  Overread Lead-Out:     " << yn(caps.supportsOverreadLeadOut) << "\n";

	// --- Media Type Support ---
	std::cout << "\n--- Media Type Support ---\n";
	std::cout << "  Reads:  CD-R=" << yn(caps.readsCDR)
		<< "  CD-RW=" << yn(caps.readsCDRW)
		<< "  DVD=" << yn(caps.readsDVD)
		<< "  BD=" << yn(caps.readsBD) << "\n";
	std::cout << "  Writes: CD-R=" << yn(caps.writesCDR)
		<< "  CD-RW=" << yn(caps.writesCDRW)
		<< "  DVD=" << yn(caps.writesDVD) << "\n";

	// --- Audio Playback ---
	std::cout << "\n--- Audio Playback ---\n";
	std::cout << "  Digital Audio Play:    " << yn(caps.supportsDigitalAudioPlay) << "\n";
	std::cout << "  Separate Volume:       " << yn(caps.supportsSeparateVolume) << "\n";
	std::cout << "  Separate Mute:         " << yn(caps.supportsSeparateMute) << "\n";
	std::cout << "  Composite Output:      " << yn(caps.supportsCompositeOutput) << "\n";

	// --- Mechanical Features ---
	std::cout << "\n--- Mechanical Features ---\n";
	const char* mechNames[] = { "Caddy", "Tray", "Pop-up", "Changer", "Reserved", "Slot" };
	const char* mechName = (caps.loadingMechanism >= 0 && caps.loadingMechanism <= 5)
		? mechNames[caps.loadingMechanism] : "Unknown";
	std::cout << "  Loading Mechanism:     " << mechName << "\n";
	std::cout << "  Eject:                 " << yn(caps.supportsEject) << "\n";
	std::cout << "  Lock Media:            " << yn(caps.supportsLockMedia) << "\n";
	std::cout << "  Multi-Session:         " << yn(caps.supportsMultiSession) << "\n";
	std::cout << "  Disc Changer:          " << yn(caps.isChanger) << "\n";

	// --- Write Capabilities ---
	bool canWrite = caps.writesCDR || caps.writesCDRW || caps.writesDVD
		|| caps.writesDVDRAM || caps.writesBD;
	std::cout << "\n--- Write Capabilities ---\n";
	if (!canWrite) {
		std::cout << "  (read-only drive)\n";
	}
	else {
		std::cout << "  Writes CD-R:           " << yn(caps.writesCDR) << "\n";
		std::cout << "  Writes CD-RW:          " << yn(caps.writesCDRW) << "\n";
		std::cout << "  Writes DVD:            " << yn(caps.writesDVD) << "\n";
		std::cout << "  Writes DVD-RAM:        " << yn(caps.writesDVDRAM) << "\n";
		std::cout << "  Writes BD:             " << yn(caps.writesBD) << "\n";
		std::cout << "  Test Write (Simulate): " << yn(caps.supportsTestWrite) << "\n";
		std::cout << "  Buffer Underrun Prot:  " << yn(caps.supportsBufferUnderrunProtection) << "\n";
		std::cout << "  Write TAO:             " << yn(caps.supportsWriteTAO) << "\n";
		std::cout << "  Write SAO/DAO:         " << yn(caps.supportsWriteSAO) << "\n";
	}

	// CD-Text write-path probe -- only meaningful for CD-R writers. Reveals
	// whether the drive accepts a P-W subchannel write mode (required to put
	// CD-Text in the lead-in), since WRITE BUFFER (0x3B) is rejected outright by
	// many non-Plextor drives. Non-destructive; restores default SAO afterward.
	if (caps.writesCDR) {
		ProbeCDTextWritePaths();
	}

	// --- Performance ---
	std::cout << "\n--- Performance ---\n";
	if (caps.maxReadSpeedKB > 0)
		std::cout << "  Max Read Speed:        " << caps.maxReadSpeedKB << " KB/s ("
		<< caps.maxReadSpeedKB / 176 << "x)\n";
	if (caps.currentReadSpeedKB > 0)
		std::cout << "  Current Read Speed:    " << caps.currentReadSpeedKB << " KB/s ("
		<< caps.currentReadSpeedKB / 176 << "x)\n";
	if (caps.maxWriteSpeedKB > 0)
		std::cout << "  Max Write Speed:       " << caps.maxWriteSpeedKB << " KB/s ("
		<< caps.maxWriteSpeedKB / 176 << "x)\n";
	else if (canWrite)
		std::cout << "  Max Write Speed:       (not reported)\n";
	else
		std::cout << "  Max Write Speed:       (read-only drive)\n";
	if (caps.currentWriteSpeedKB > 0)
		std::cout << "  Current Write Speed:   " << caps.currentWriteSpeedKB << " KB/s ("
		<< caps.currentWriteSpeedKB / 176 << "x)\n";
	if (caps.bufferSizeKB > 0)
		std::cout << "  Buffer Size:           " << caps.bufferSizeKB << " KB\n";

	if (!caps.supportedReadSpeeds.empty()) {
		std::cout << "  Supported Read Speeds: ";
		for (size_t i = 0; i < caps.supportedReadSpeeds.size(); i++) {
			if (i > 0) std::cout << ", ";
			std::cout << caps.supportedReadSpeeds[i] / 176 << "x";
		}
		std::cout << "\n";
	}

	if (!caps.supportedWriteSpeeds.empty()) {
		std::cout << "  Supported Write Speeds:";
		for (size_t i = 0; i < caps.supportedWriteSpeeds.size(); i++) {
			if (i > 0) std::cout << ", ";
			std::cout << caps.supportedWriteSpeeds[i] / 176 << "x";
		}
		std::cout << "\n";
	}

	// --- Drive Accuracy Rating ---
	std::cout << "\n--- Drive Accuracy Rating ---\n";

	int ratingScore = 0;
	constexpr int kMaxScore = 100;

	// CD-DA extraction is mandatory for any audio ripping (30 pts)
	if (caps.supportsCDDA) {
		ratingScore += 30;
		std::cout << "  [+30] CD-DA extraction supported\n";
	}
	else {
		std::cout << "  [  0] CD-DA extraction NOT supported (critical)\n";
	}

	// Accurate Stream means jitter-free delivery — no re-reads needed (25 pts)
	if (caps.supportsAccurateStream) {
		ratingScore += 25;
		std::cout << "  [+25] Accurate Stream reported\n";
	}
	else {
		std::cout << "  [  0] Accurate Stream NOT reported (will need verification reads)\n";
	}

	// C2 error reporting enables reliable error detection (20 pts)
	if (caps.supportsC2ErrorReporting) {
		ratingScore += 20;
		std::cout << "  [+20] C2 error reporting supported\n";
	}
	else {
		std::cout << "  [  0] C2 error reporting NOT supported\n";
	}

	// Overread lead-in/lead-out allows offset correction at disc edges (5+5 pts)
	if (caps.supportsOverreadLeadIn) {
		ratingScore += 5;
		std::cout << "  [+ 5] Overread into lead-in supported\n";
	}
	else {
		std::cout << "  [  0] Overread into lead-in NOT supported\n";
	}
	if (caps.supportsOverreadLeadOut) {
		ratingScore += 5;
		std::cout << "  [+ 5] Overread into lead-out supported\n";
	}
	else {
		std::cout << "  [  0] Overread into lead-out NOT supported\n";
	}

	// Raw read support for sector-level access (5 pts)
	if (caps.supportsRawRead) {
		ratingScore += 5;
		std::cout << "  [+ 5] Raw read supported\n";
	}
	else {
		std::cout << "  [  0] Raw read NOT supported\n";
	}

	// Subchannel support aids metadata verification (3+3 pts)
	if (caps.supportsSubchannelRaw) {
		ratingScore += 3;
		std::cout << "  [+ 3] Raw subchannel read supported\n";
	}
	if (caps.supportsSubchannelQ) {
		ratingScore += 3;
		std::cout << "  [+ 3] Q-channel de-interleaved read supported\n";
	}

	// Buffer size bonus: larger cache reduces re-read overhead (up to 4 pts)
	if (caps.bufferSizeKB >= 2048) {
		ratingScore += 4;
		std::cout << "  [+ 4] Large buffer (" << caps.bufferSizeKB << " KB)\n";
	}
	else if (caps.bufferSizeKB >= 512) {
		ratingScore += 2;
		std::cout << "  [+ 2] Moderate buffer (" << caps.bufferSizeKB << " KB)\n";
	}

	ratingScore = std::min(ratingScore, kMaxScore);

	const char* grade;
	const char* summary;
	if (ratingScore >= 90) {
		grade = "A+";
		summary = "Excellent -- ideal for bit-perfect secure ripping.";
	}
	else if (ratingScore >= 80) {
		grade = "A";
		summary = "Very good -- capable of accurate extraction with C2 or verification reads.";
	}
	else if (ratingScore >= 65) {
		grade = "B";
		summary = "Good -- usable but may need multi-pass verification for full confidence.";
	}
	else if (ratingScore >= 50) {
		grade = "C";
		summary = "Fair -- limited accuracy features; use Paranoid rip mode.";
	}
	else {
		grade = "D";
		summary = "Poor -- not recommended for accurate audio extraction.";
	}

	std::cout << "\n  Score: " << ratingScore << "/" << kMaxScore
		<< "  Grade: " << grade << "\n";
	std::cout << "  " << summary << "\n";

	// --- Pioneer vendor features (only on Pioneer drives) ---
	{
		PioneerVendor pioneer(GetDriveRef());
		if (pioneer.IsPioneerDrive()) {
			PioneerCapabilities pc;
			pioneer.ReadCapabilities(pc);
			pioneer.PrintCapabilitiesReport();
		}
	}

	// --- Hardware quality-scan backends ---
	// Which of the hardware C1/C2/CU, jitter, and FE/TE scans (menu options 6,
	// 9, 25, 30, 32) can actually run depends on the drive's vendor commands, not
	// just the capability flags above. Probe live and report so the user knows
	// which scans to expect. The probes read the disc, so one must be loaded.
	{
		ScsiDrive& d = GetDriveRef();
		std::cout << "\n  Hardware quality-scan backends:\n";
		if (!d.TestUnitReady()) {
			std::cout << "    (Load a disc and re-run to detect scan backends.)\n";
		}
		else {
			// Same precedence RunQCheckScan uses: Plextor Q-Check, then Pioneer,
			// then LiteOn/MediaTek. Jitter and FE/TE are LiteOn-only.
			bool qcheck  = d.SupportsQCheck();
			bool pioneer = !qcheck && d.SupportsPioneerScan();
			bool liteon  = !qcheck && !pioneer && d.SupportsLiteOnScan();
			bool jitter  = liteon && d.SupportsLiteOnJitter();
			bool fete    = liteon && d.SupportsLiteOnFeTe();

			const char* method = qcheck ? "Plextor Q-Check (0xE9/0xEB)"
				: pioneer ? "Pioneer (0x3B/0x3C)"
				: liteon  ? "LiteOn/MediaTek (0xDF)"
				: nullptr;

			auto row = [](const char* label, bool ok, const char* note) {
				std::cout << "    [" << (ok ? "YES" : " no") << "] " << label;
				if (ok && note && *note) std::cout << "  -- " << note;
				std::cout << "\n";
			};
			const char* qualityNote = pioneer
				? "Pioneer 0x3B/0x3C (C1/E22; verified C2/CU not measured)"
				: (method ? method : "");
			row("Hardware quality scan (opt 6)", method != nullptr, qualityNote);
			row("Jitter / beta scan (opt 30)", jitter, "LiteOn 0xDF/0x1B");
			row("Focus/tracking-error scan (opt 32)", fete, "LiteOn 0xDF/0x08, experimental");
			if (!method)
				std::cout << "    No hardware error-scan; software C2 scan (opt 7/8) via READ CD still works.\n";
		}
	}

	std::cout << std::string(60, '=') << "\n";
}

// ============================================================================
// CD-Text Write-Path Probe
// ----------------------------------------------------------------------------
// CD-Text is written into the disc lead-in's R-W subchannel during an SAO (or
// RAW) write -- it is NOT delivered by WRITE BUFFER (0x3B) on most non-Plextor
// drives. For the host to put CD-Text in the lead-in it must be able to send
// full 2448-byte sectors (2352 main + 96 subchannel) in a write mode whose
// block type carries P-W (packed 0x02 or raw 0x03). This probe runs MODE SELECT
// + MODE SENSE readback for every relevant write/block-type combination and
// reports, per mode, whether the drive ACCEPTS it as requested, silently
// DOWNGRADES it (accepts MODE SELECT but stores different parameters), or
// REJECTS it outright. No data is written to the disc; the drive's default SAO
// write parameters are restored at the end.
// ============================================================================
namespace {
	struct WriteModeProbe {
		BYTE pageByte2;    // Write Parameters page byte 2 (write type nibble)
		BYTE pageByte4;    // byte 4 (data block type)
		BYTE expectWrite;  // expected write type (low nibble) after readback
		BYTE expectBlock;  // expected block type after readback
		const char* label;
		bool carriesRW;    // block type carries P-W subchannel (CD-Text capable)
	};

	// Read back the Write Parameters page (0x05) and return its write type (low
	// nibble of byte 2) and block type (byte 4). DBD is set (CDB[1] bit 3) so the
	// drive returns no block descriptor and the page is always at offset 8.
	// Returns false if the page couldn't be read or the returned page code isn't
	// 0x05 (i.e. we can't trust the bytes).
	bool ReadWriteParamsPage(ScsiDrive& drive, BYTE& writeType, BYTE& blockType) {
		BYTE senseCdb[10] = { 0x5A, 0x08, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x00 };
		BYTE buf[60] = { 0 };
		if (!drive.SendSCSI(senseCdb, sizeof(senseCdb), buf, sizeof(buf), true))
			return false;
		WORD bdLen = (static_cast<WORD>(buf[6]) << 8) | buf[7];
		size_t pageOff = static_cast<size_t>(8) + bdLen;
		if (pageOff + 4 >= sizeof(buf)) return false;
		if ((buf[pageOff] & 0x3F) != 0x05) return false;   // not the page we asked for
		writeType = buf[pageOff + 2] & 0x0F;
		blockType = buf[pageOff + 4];
		return true;
	}

	// MODE SELECT one Write Parameters combination. Returns false (sense in
	// sk/asc/ascq) if the drive rejected the command outright. Mirrors the byte
	// layout of SetWriteParametersPage() in OpticalDrive_WriteDisc_CueSheet.cpp.
	bool SelectWriteParams(ScsiDrive& drive, const WriteModeProbe& m,
		BYTE& sk, BYTE& asc, BYTE& ascq) {
		BYTE modeData[60] = { 0 };
		BYTE* page = modeData + 8;   // 8-byte MODE SELECT(10) parameter header
		page[0] = 0x05;
		page[1] = 0x32;
		page[2] = m.pageByte2;
		page[3] = 0x00;
		page[4] = m.pageByte4;
		page[5] = 0x00;
		page[8] = 0x00;
		WORD totalLen = 60;
		BYTE selectCdb[10] = { 0x55, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		selectCdb[7] = static_cast<BYTE>((totalLen >> 8) & 0xFF);
		selectCdb[8] = static_cast<BYTE>(totalLen & 0xFF);
		sk = asc = ascq = 0;
		return drive.SendSCSIWithSense(selectCdb, sizeof(selectCdb), modeData, totalLen,
			&sk, &asc, &ascq, false);
	}
}

void OpticalDrive::ProbeCDTextWritePaths() {
	// write type: 0x02 = SAO (session-at-once), 0x03 = RAW.
	// block type: 0x00 = none (2352), 0x02 = packed P-W (2448), 0x03 = raw P-W (2448).
	static const WriteModeProbe modes[] = {
		{ 0x42, 0x00, 0x02, 0x00, "SAO       (audio only, 2352)", false },
		{ 0x42, 0x02, 0x02, 0x02, "SAO/R96P  (packed P-W, 2448)", true  },
		{ 0x42, 0x03, 0x02, 0x03, "SAO/R96R  (raw P-W,    2448)", true  },
		{ 0x43, 0x00, 0x03, 0x00, "RAW       (audio only, 2352)", false },
		{ 0x43, 0x02, 0x03, 0x02, "RAW/R96P  (packed P-W, 2448)", true  },
		{ 0x43, 0x03, 0x03, 0x03, "RAW/R96R  (raw P-W,    2448)", true  },
	};

	std::cout << "\n--- CD-Text Write-Path Probe ---\n";
	std::cout << "  (CD-Text needs a P-W mode; WRITE BUFFER 0x3B is a Plextor-only path)\n";

	ScsiDrive& drive = m_drive;

	// Write Parameters MODE SELECT typically only "sticks" when the drive is
	// write-ready with blank writable media. Settle the drive, then capture its
	// CURRENT (default) page so we can tell a real downgrade apart from a MODE
	// SELECT the drive accepted (GOOD status) but silently ignored.
	drive.WaitForDriveReady(15);

	BYTE defWrite = 0, defBlock = 0;
	if (!ReadWriteParamsPage(drive, defWrite, defBlock)) {
		Console::Warning("  Could not read back the Write Parameters page (0x05)\n");
		std::cout << "    The drive returned no valid page, so write-mode support can't be probed.\n"
			<< "    Load a blank CD-R and retry; some drives only expose write parameters when\n"
			<< "    writable media is present.\n";
		return;
	}

	std::cout << "  (drive default: write 0x" << std::hex << std::setfill('0') << std::setw(2)
		<< static_cast<int>(defWrite) << "/block 0x" << std::setw(2) << static_cast<int>(defBlock)
		<< std::dec << std::setfill(' ') << ")\n\n";

	bool saoR96r = false, rawR96r = false, saoR96p = false, rawR96p = false;
	int acceptedCount = 0, notAppliedCount = 0, downgradedCount = 0, rejectedCount = 0;

	for (const auto& mode : modes) {
		std::cout << "  " << mode.label << "  : ";

		BYTE sk = 0, asc = 0, ascq = 0;
		if (!SelectWriteParams(drive, mode, sk, asc, ascq)) {
			Console::Error("REJECTED");
			std::cout << " (" << drive.GetSenseDescription(sk, asc, ascq) << ")\n";
			rejectedCount++;
			continue;
		}

		BYTE gotWrite = 0, gotBlock = 0;
		if (!ReadWriteParamsPage(drive, gotWrite, gotBlock)) {
			Console::Warning("UNVERIFIABLE (readback failed)\n");
			continue;
		}

		if (gotWrite == mode.expectWrite && gotBlock == mode.expectBlock) {
			Console::Success("ACCEPTED\n");
			acceptedCount++;
			if (mode.carriesRW) {
				if (mode.expectWrite == 0x02 && mode.expectBlock == 0x03) saoR96r = true;
				if (mode.expectWrite == 0x03 && mode.expectBlock == 0x03) rawR96r = true;
				if (mode.expectWrite == 0x02 && mode.expectBlock == 0x02) saoR96p = true;
				if (mode.expectWrite == 0x03 && mode.expectBlock == 0x02) rawR96p = true;
			}
		}
		else if (gotWrite == defWrite && gotBlock == defBlock) {
			Console::Warning("NOT APPLIED");
			std::cout << " (MODE SELECT accepted but drive kept its default)\n";
			notAppliedCount++;
		}
		else {
			Console::Warning("DOWNGRADED");
			std::cout << " (drive stored write 0x" << std::hex << std::setfill('0')
				<< std::setw(2) << static_cast<int>(gotWrite) << "/block 0x"
				<< std::setw(2) << static_cast<int>(gotBlock)
				<< std::dec << std::setfill(' ') << " instead)\n";
			downgradedCount++;
		}
	}

	// Restore the drive's default audio-only SAO write parameters.
	{ BYTE sk = 0, asc = 0, ascq = 0; SelectWriteParams(drive, modes[0], sk, asc, ascq); }

	std::cout << "\n  CD-Text verdict: ";
	if (saoR96r || rawR96r) {
		Console::Success("host-side CD-Text is possible on this drive\n");
		if (saoR96r)
			std::cout << "    Path: SAO/R96R -- write the lead-in as 2448-byte sectors carrying the\n"
			<< "    CD-Text packs in R-W, then continue into the program area as one write.\n";
		else
			std::cout << "    Path: RAW/R96R -- write the whole disc raw (lead-in + program + lead-out)\n"
			<< "    with the CD-Text packs in the lead-in R-W.\n";
	}
	else if (saoR96p || rawR96p) {
		Console::Warning("only packed P-W (R96P) accepted -- test an R96P burn before relying on it\n");
	}
	else if (acceptedCount == 0 && downgradedCount == 0 && rejectedCount == 0 && notAppliedCount > 0) {
		Console::Warning("INCONCLUSIVE -- the drive ignored every MODE SELECT\n");
		std::cout << "    Each MODE SELECT returned GOOD but the drive kept its default parameters,\n"
			<< "    which usually means it is not write-ready. Load a BLANK CD-R (not a pressed\n"
			<< "    or finalized disc) and run this again -- write parameters often only stick\n"
			<< "    when writable media is present.\n";
	}
	else {
		Console::Error("host-side CD-Text does NOT look possible on this drive\n");
		std::cout << "    No P-W subchannel write mode was accepted, and WRITE BUFFER (0x3B) is\n"
			<< "    rejected too. Audio burns fine; CD-Text simply cannot be written here.\n";
	}
}

void OpticalDrive::ShowDriveRecommendations() {
	m_drive.DisplayDriveRecommendations();
}

void OpticalDrive::EnsureCapabilitiesDetected() {
	if (!m_capabilitiesDetected) {
		DriveCapabilities caps;
		DetectDriveCapabilities(caps);
	}
}

// ============================================================================
// Chipset / Controller Identification
// ============================================================================

bool OpticalDrive::DetectChipset(ChipsetInfo& info) {
	return m_drive.DetectChipset(info);
}

void OpticalDrive::PrintChipsetInfo(const ChipsetInfo& info) {
	auto familyName = [](ChipsetFamily f) -> const char* {
		switch (f) {
		case ChipsetFamily::MediaTek:  return "MediaTek";
		case ChipsetFamily::Renesas:   return "Renesas";
		case ChipsetFamily::Panasonic: return "Panasonic";
		case ChipsetFamily::Sanyo:     return "Sanyo";
		case ChipsetFamily::Philips:   return "Philips";
		case ChipsetFamily::Sony:      return "Sony";
		case ChipsetFamily::Plextor:   return "Plextor";
		case ChipsetFamily::LiteOn:    return "LiteOn";
		case ChipsetFamily::Pioneer:   return "Pioneer";
		case ChipsetFamily::Realtek:   return "Realtek";
		case ChipsetFamily::JMicron:   return "JMicron";
		case ChipsetFamily::ASMedia:   return "ASMedia";
		case ChipsetFamily::VIA:       return "VIA";
		case ChipsetFamily::NEC:       return "NEC/Renesas";
		case ChipsetFamily::Ricoh:     return "Ricoh";
		default:                       return "Unknown";
		}
	};

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "            CHIPSET / CONTROLLER REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	std::cout << "\n--- Controller Identification ---\n";
	std::cout << "  Chipset Family:     " << familyName(info.family) << "\n";
	std::cout << "  Chipset Name:       " << info.chipsetName << "\n";
	std::cout << "  Detection Method:   " << info.detectionMethod << "\n";
	std::cout << "  Confidence:         " << info.confidencePercent << "%\n";
	if (!info.plextorTLA.empty()) {
		std::cout << "  Plextor TLA:        #" << info.plextorTLA << "\n";
	}

	std::cout << "\n--- Interface ---\n";
	std::cout << "  Interface Type:     " << info.interfaceType << "\n";
	std::cout << "  USB Attached:       " << (info.isUSBAttached ? "YES" : "NO") << "\n";
	if (!info.usbBridge.empty()) {
		std::cout << "  USB Bridge Chip:    " << info.usbBridge << "\n";
	}

	if (info.knownAudioQuirks) {
		std::cout << "\n--- Audio Extraction Quirks ---\n";
		Console::Warning("  Known quirks detected:\n");
		std::cout << "  " << info.quirkDescription << "\n";
	}
	else {
		std::cout << "\n--- Audio Extraction Quirks ---\n";
		Console::Success("  No known audio extraction issues for this chipset.\n");
	}

	// Audio quality recommendation based on chipset
	std::cout << "\n--- Recommendation ---\n";
	switch (info.family) {
	case ChipsetFamily::Plextor:
	{
		Console::Success("  Excellent chipset for audio extraction.\n");
		bool qcheck = m_drive.SupportsQCheck();
		if (qcheck) {
			std::cout << "  Supports hardware C1/C2/CU scanning (Q-Check) and D8 vendor reads.\n";
		}
		else {
			std::cout << "  Supports D8 vendor reads. Q-Check (0xE9) not available on this model.\n";
		}
		break;
	}
	case ChipsetFamily::Pioneer:
		Console::Success("  Very good chipset for audio extraction.\n");
		std::cout << "  Pioneer drives support Accurate Stream; C2 reporting behavior is model-\n"
			<< "  and firmware-dependent and is kept separate from vendor E22 diagnostics.\n";
		if (m_drive.SupportsPioneerScan()) {
			std::cout << "  Supports hardware BLER/E22 scanning (0x3B/0x3C vendor commands).\n";
		}
		break;
	case ChipsetFamily::LiteOn:
		Console::Info("  Good chipset for audio extraction.\n");
		std::cout << "  MediaTek-based; supports D8 vendor reads on many models.\n";
		break;
	case ChipsetFamily::MediaTek:
		Console::Info("  Adequate chipset for audio extraction.\n");
		std::cout << "  Verify C2 accuracy with the C2 Validation Test (menu option 21).\n";
		break;
	case ChipsetFamily::Panasonic:
		Console::Warning("  Mixed results for audio extraction.\n");
		std::cout << "  Laptop/slim drives may have limited C2 accuracy. Use secure rip mode.\n";
		break;
	default:
		Console::Info("  Unknown chipset quality rating.\n");
		std::cout << "  Run C2 Validation and Drive Capabilities tests for more information.\n";
		break;
	}

	std::cout << std::string(60, '=') << "\n";
}
