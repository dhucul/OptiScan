#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleColors.h"
#include "GuiInput.h"
#include "Progress.h"
#include "InterruptHandler.h"
#include "WriteDiscInternal.h"
#include "PioneerVendor.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <limits>
#include <vector>
#include <windows.h>

// ============================================================================
// WriteDisc - Write disc from .bin/.cue/.sub files
// ============================================================================
bool OpticalDrive::WriteDisc(const std::wstring& binFile,
	const std::wstring& cueFile, const std::wstring& subFile,
	int speed, bool usePowerCalibration, bool discAlreadyBlanked,
	bool /*attemptSubchannel_deprecated*/) {

	Console::BoxHeading("Write Disc from Files");

	// Validate the complete source image before checking or erasing destination
	// media. Invalid inputs must never cause an existing CD-RW to be destroyed.
	std::ifstream binStream(binFile, std::ios::binary);
	if (!binStream.is_open()) {
		Console::Error("Cannot open .bin file: ");
		std::wcout << binFile << L"\n";
		return false;
	}
	binStream.seekg(0, std::ios::end);
	long long fileSize = binStream.tellg();
	binStream.close();

	constexpr DWORD PREGAP_SECTORS = 150;
	constexpr DWORD MAX_PROGRAM_SECTORS =
		static_cast<DWORD>((std::numeric_limits<int32_t>::max)()) -
		PREGAP_SECTORS;
	if (fileSize <= 0 || fileSize % AUDIO_SECTOR_SIZE != 0) {
		Console::Error("Invalid .bin file size; expected a non-empty whole number of sectors\n");
		return false;
	}
	const unsigned long long sectorCount =
		static_cast<unsigned long long>(fileSize / AUDIO_SECTOR_SIZE);
	if (sectorCount > MAX_PROGRAM_SECTORS) {
		Console::Error("Invalid .bin file size; sector count exceeds the write-address limit\n");
		return false;
	}
	DWORD totalSectors = static_cast<DWORD>(sectorCount);

	// Subchannel is drive-generated via the SAO path.  Raw P-W writes are not
	// portable: some recorders accept MODE SELECT and SEND CUE SHEET, then reject
	// the first 2448-byte WRITE at LBA -150 with INVALID ADDRESS FOR WRITE.
	// Validate a supplied .sub file for user feedback, but do not let its presence
	// silently switch the recording mode.
	bool hasSubchannel = false;
	bool needsDeinterleave = false;
	if (!subFile.empty()) {
		std::ifstream subStream(subFile, std::ios::binary);
		if (subStream.is_open()) {
			subStream.seekg(0, std::ios::end);
			long long subSize = subStream.tellg();
			subStream.close();

			long long expectedSubSize = static_cast<long long>(totalSectors) * SUBCHANNEL_SIZE;
			if (subSize >= expectedSubSize) {
				Console::Success("Subchannel file validated (");
				std::cout << (subSize / 1024) << " KB, " << totalSectors << " sectors)\n";
				Console::Info("Using SAO (drive-generated subchannel; pregaps remain exact)\n");
			}
			else {
				Console::Warning("Subchannel file size mismatch (expected ");
				std::cout << expectedSubSize << " bytes, got " << subSize
					<< ") -- writing without subchannel\n";
			}
		}
		else {
			Console::Warning("Cannot open .sub file -- writing without subchannel data\n");
		}
	}

	// Parse CUE sheet -- also extracts TITLE/PERFORMER/CATALOG for CD-Text and MCN
	std::vector<TrackWriteInfo> tracks;
	std::string discTitle, discPerformer, discMCN;
	if (!ParseCueSheet(cueFile, tracks, discTitle, discPerformer, discMCN)) {
		Console::Error("Failed to parse CUE sheet\n");
		return false;
	}

	// Set the final audio boundary now that the BIN size is known. A non-zero
	// end on the last retained track means trailing data tracks were filtered.
	if (tracks.back().endLBA == (std::numeric_limits<DWORD>::max)()) {
		tracks.back().endLBA = totalSectors - 1;
	}
	else if (tracks.back().endLBA >= totalSectors) {
		Console::Error("CUE sheet extends beyond the end of the .bin file\n");
		return false;
	}
	else if (tracks.back().endLBA < totalSectors - 1) {
		const DWORD audioSectors = tracks.back().endLBA + 1;
		Console::Info("Trimming to audio content: ");
		std::cout << audioSectors << " of " << totalSectors << " sectors\n";
		totalSectors = audioSectors;
	}
	// Validate every range after any enhanced-CD trim. This also catches
	// unsigned underflow produced by a malformed INDEX 00 before any writer can
	// turn it into a huge stream or silently pad a short BIN read with zeroes.
	for (size_t i = 0; i < tracks.size(); ++i) {
		const auto& track = tracks[i];
		if (track.trackNumber < 1 || track.trackNumber > 99 ||
			(i > 0 && track.trackNumber <= tracks[i - 1].trackNumber)) {
			Console::Error("CUE sheet contains an invalid or non-increasing track number\n");
			return false;
		}
		if (track.startLBA >= totalSectors ||
			track.endLBA < track.startLBA ||
			track.endLBA >= totalSectors) {
			Console::Error("CUE track ");
			std::cout << track.trackNumber
				<< " has a sector range outside the .bin file\n";
			return false;
		}
		if (track.hasPregap && track.pregapLBA > track.startLBA) {
			Console::Error("CUE track ");
			std::cout << track.trackNumber << " has an invalid pregap boundary\n";
			return false;
		}
	}

	bool blankedForThisWrite = discAlreadyBlanked;

	auto readDiscState = [&](BYTE& discStatus, bool& isErasable) -> bool {
		if (!WriteDiscInternal::WaitForDriveReady(m_drive, 15)) {
			Console::Error("Drive did not become ready for media validation\n");
			return false;
		}

		BYTE discInfoCmd[10] = {
			0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0x00
		};
		BYTE discInfoResp[252] = { 0 };
		BYTE sk = 0, asc = 0, ascq = 0;
		if (!m_drive.SendSCSIWithSense(discInfoCmd, sizeof(discInfoCmd),
			discInfoResp, sizeof(discInfoResp), &sk, &asc, &ascq, true)) {
			Console::Error("Could not read disc status (");
			std::cout << m_drive.GetSenseDescription(sk, asc, ascq) << ")\n";
			return false;
		}

		const WORD responseLength =
			(static_cast<WORD>(discInfoResp[0]) << 8) | discInfoResp[1];
		if (responseLength < 1) {
			Console::Error("Drive returned incomplete disc-status data\n");
			return false;
		}

		discStatus = discInfoResp[2] & 0x03;
		isErasable = (discInfoResp[2] & 0x10) != 0;
		return true;
	};

	auto ensureBlankWritableMedia = [&](bool allowBlanking, bool announce) -> bool {
		BYTE discStatus = 0;
		bool isErasable = false;
		if (!readDiscState(discStatus, isErasable)) return false;

		if (announce) {
			Console::Success("Disc type: ");
			std::cout << (isErasable ? "CD-RW (rewritable)\n" : "CD-R (write-once)\n");
		}

		if (discStatus == 0x00) {
			if (announce)
				Console::Success("Disc is empty and ready for writing\n");
			return true;
		}

		if (discStatus != 0x01 && discStatus != 0x02) {
			Console::Error("Drive returned an unsupported disc status (0x");
			std::cout << std::hex << static_cast<int>(discStatus)
				<< std::dec << ")\n";
			return false;
		}

		if (!isErasable) {
			Console::Error("CD-R already contains data and cannot be erased\n");
			Console::Info("Insert a blank CD-R disc and try again\n");
			return false;
		}

		if (!allowBlanking) {
			Console::Error("Expected a blank disc, but the loaded CD-RW contains data\n");
			return false;
		}

		Console::Warning(discStatus == 0x01
			? "CD-RW disc has an incomplete session\n"
			: "CD-RW disc is fully written\n");
		if (!GuiInput::PromptYesNo("Blank disc?",
			"The disc must be blanked before writing. Erase all data now?")) {
			Console::Info("Write operation cancelled\n");
			return false;
		}

		// The caller just confirmed, so suppress BlankRewritableDisk's second
		// confirmation prompt.
		if (!BlankRewritableDisk(speed, true, true)) {
			Console::Error("Failed to blank disc\n");
			return false;
		}
		blankedForThisWrite = true;

		if (!readDiscState(discStatus, isErasable) || discStatus != 0x00) {
			Console::Error("Disc did not report a blank state after erasing\n");
			return false;
		}
		Console::Success("Disc blanking verified\n");
		return true;
	};

	auto verifyCapacity = [&]() -> bool {
		if (!WriteDiscInternal::WaitForDriveReady(m_drive, 15)) {
			Console::Error("Drive did not become ready for capacity validation\n");
			return false;
		}

		// READ TRACK INFORMATION: type=1 (track number), track 0xFF (invisible/blank)
		BYTE trackInfoCmd[10] = { 0x52, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x24, 0x00 };
		BYTE trackInfoResp[36] = { 0 };
		BYTE sk = 0, asc = 0, ascq = 0;
		if (!m_drive.SendSCSIWithSense(trackInfoCmd, sizeof(trackInfoCmd),
			trackInfoResp, sizeof(trackInfoResp), &sk, &asc, &ascq, true)) {
			Console::Error("Could not determine writable disc capacity (");
			std::cout << m_drive.GetSenseDescription(sk, asc, ascq) << ")\n";
			return false;
		}

		const WORD responseLength =
			(static_cast<WORD>(trackInfoResp[0]) << 8) | trackInfoResp[1];
		if (responseLength < 26) {
			Console::Error("Drive returned incomplete track-capacity data\n");
			return false;
		}

		const DWORD freeBlocks =
			(static_cast<DWORD>(trackInfoResp[16]) << 24) |
			(static_cast<DWORD>(trackInfoResp[17]) << 16) |
			(static_cast<DWORD>(trackInfoResp[18]) << 8) |
			static_cast<DWORD>(trackInfoResp[19]);
		if (freeBlocks == 0) {
			Console::Error("Drive reports no writable program sectors on this disc\n");
			return false;
		}

		// Free Blocks is the writable program-area capacity beginning at LBA 0.
		// The pregap occupies negative LBAs and the recorder reserves/generates
		// the lead-out, so neither is charged against this program-area count.
		if (totalSectors > freeBlocks) {
			Console::Error("Image too large for disc (need ");
			std::cout << totalSectors << " program sectors, disc has "
				<< freeBlocks << " free)\n";
			Console::Info("Use a higher-capacity disc (e.g., 80-min or 90-min CD-R)\n");
			return false;
		}
		return true;
	};

	Console::Info("Checking disc media status...\n");
	if (!ensureBlankWritableMedia(!discAlreadyBlanked, true) ||
		!verifyCapacity()) {
		return false;
	}

	// ── Check CD-Text write capability early (before write mode setup) ──
	bool canWriteCDText = false;
	if (WriteDiscInternal::HasCDTextContent(discTitle, discPerformer, tracks)) {
		DriveCapabilities caps;
		canWriteCDText = m_drive.DetectCapabilities(caps) && caps.supportsWriteCDText;
		if (!canWriteCDText) {
			Console::Warning("Drive does not advertise CD-Text write support - skipping\n");
		}
	}

	// A rip immediately before this burn may have left a Pioneer drive in the
	// Quiet + Fragile/Rental-CD slow-spin read preset (ApplyAudioExtractionPreset).
	// Those read-optimized modes survive the eject/media-swap and stop the drive
	// from streaming a write -- the BDR-S13U aborts a few hundred sectors in with
	// COMMAND SEQUENCE ERROR (KEY=05 ASC=2C ASCQ=00). Restore the drive's default
	// write-capable state first. No-op on non-Pioneer drives.
	{
		PioneerVendor pioneer(m_drive);
		if (pioneer.IsPioneerDrive()) {
			// Detect whether the preset is actually engaged *before* clearing, so
			// the message reflects reality: ClearAudioExtractionPreset() returns
			// true whenever the vendor commands are accepted, not whether the modes
			// were on. fragileCdOn (cap byte 56) is the reliable signal; a non-
			// default Quiet level (byte 2) corroborates it. The reset itself runs
			// unconditionally -- it is idempotent, so the burn is always write-ready
			// even if the cap bits under-report -- but we only announce it when the
			// drive was genuinely left in the read preset.
			const PioneerCapabilities& pc = pioneer.Capabilities();
			bool inReadPreset = pc.valid &&
				(pc.fragileCdOn ||
					(pc.advancedQuietCurrent != 0xFF && pc.advancedQuietCurrent != 0));
			pioneer.ClearAudioExtractionPreset();
			if (inReadPreset) {
				Console::Info("Reset Pioneer read preset (Quiet/Fragile) left by the previous rip\n");
			}

			// Burn-after-copy failure: the rip leaves the drive's write channel /
			// buffer in a read-session state the BDR-S13U cannot stream a burn out
			// of. It accepts the cue sheet and starts writing, then aborts about
			// 567 sectors in (~ its write-buffer depth) with COMMAND SEQUENCE ERROR
			// (KEY=05 ASC=2C ASCQ=00). A CD-RW only recovers because BLANK cycles
			// the write subsystem first; a write-once CD-R has no blank pass. When
			// we are NOT coming off a blank, drop and re-Open the drive handle so
			// the burn runs on a clean handle, free of the per-handle driver state
			// the prior read session left behind. Non-destructive: no eject, no
			// erase, no marks. The media stays loaded across the re-Open.
			if (!blankedForThisWrite) {
				Console::Info("Resetting drive handle to clear read-session state before write...\n");
				if (!m_drive.Reopen()) {
					Console::Error("Failed to reopen drive after reset - aborting write\n");
					return false;
				}
				if (!ensureBlankWritableMedia(false, false) ||
					!verifyCapacity()) {
					Console::Error("Media validation failed after reopening the drive\n");
					return false;
				}
			}
		}
	}

	// Set drive write speed BEFORE power calibration (OPC is speed-dependent).
	// SET CD SPEED is only a request: modern media/firmware commonly clamp a
	// 1x request to 4x, 8x, or another supported write strategy. Read back the
	// selected value instead of unconditionally reporting the request as fact.
	WORD actualReadKB = 0;
	WORD actualWriteKB = 0;
	bool exactSpeed = m_drive.TrySetSpeedAndVerify(
		speed, speed, &actualReadKB, &actualWriteKB);
	if (actualWriteKB > 0) {
		int actualWriteX =
			(static_cast<int>(actualWriteKB) + CD_SPEED_1X / 2) / CD_SPEED_1X;
		if (actualWriteX < 1) actualWriteX = 1;

		if (exactSpeed || actualWriteX == speed) {
			Console::Success("Drive write speed confirmed at ");
		}
		else {
			Console::Info("Drive selected write speed ");
		}
		std::cout << actualWriteX << "x (" << actualWriteKB << " KB/s";
		if (actualWriteX != speed)
			std::cout << "; requested " << speed << "x";
		std::cout << ")\n";
	}
	else {
		Console::Warning("Could not verify the drive's selected write speed; ");
		std::cout << speed << "x was requested.\n";
	}

	// Plextor PoweRec: surface the drive's current state so the user knows
	// whether the smarter Plextor-only calibration path is in effect.
	// Toggling is a separate menu action; here we only report. Gate on a genuine
	// PLEXTOR vendor string -- IsPlextor() also matches LiteOn (shared chipset),
	// but PoweRec (0xED) is Plextor-only, so labeling a LiteOn "Plextor PoweRec"
	// would be misleading.
	{
		std::string poweRecVendor, poweRecModel;
		if (m_drive.GetDriveInfo(poweRecVendor, poweRecModel) &&
			poweRecVendor.find("PLEXTOR") != std::string::npos) {
			bool poweRecOn = false;
			if (m_drive.GetPoweRec(poweRecOn)) {
				Console::Info("Plextor PoweRec: ");
				std::cout << (poweRecOn ? "ENABLED" : "disabled") << "\n";
			}
		}
	}

	// Power calibration
	if (usePowerCalibration) {
		if (!PerformPowerCalibration()) {
			return false;
		}
	}

	// Audio cue-sheet writing uses SAO (write type 02h): 2352-byte blocks with
	// drive-generated subchannel.  Do not auto-promote metadata-bearing CUEs to
	// raw P-W; accepting the layout does not prove the drive can write that mode.
	int subchannelMode = 0;

	// ── CD-Text: choose a delivery method BEFORE sending the cue sheet ───
	// Two mechanisms:
	//   1. WRITE BUFFER (0x3B): the classic Plextor vendor path. The drive
	//      buffers the packs (independent of the cue sheet) and embeds them in
	//      the lead-in it generates during the SAO burn. The cue sheet stays plain.
	//   2. SAO host lead-in method (the portable path, per the libburn SAO
	//      cookbook): the cue sheet's lead-in DATA FORM is flagged for CD-Text
	//      (| 0x40) and the host writes the lead-in itself as 96-byte CD-Text
	//      blocks (ATIP lead-in start down to LBA -150) as the first phase of one
	//      continuous SAO write; WriteAudioSectors then continues from -150.
	// Deciding the method here -- before the cue sheet -- lets us send the cue
	// sheet exactly once with the correct flag (no reopen, no wasted send), so the
	// drive speed and power calibration set above stay in effect.
	std::vector<BYTE> cdTextPacks;
	bool cdTextViaWriteBuffer = false;
	bool cdTextViaLeadIn = false;
	if (canWriteCDText) {
		cdTextPacks = WriteDiscInternal::BuildCDTextPacks(discTitle, discPerformer, tracks);
		if (!cdTextPacks.empty()) {
			Console::Info("CD-Text: ");
			std::cout << (cdTextPacks.size() / 18) << " packs (";
			if (!discPerformer.empty()) std::cout << discPerformer;
			if (!discPerformer.empty() && !discTitle.empty()) std::cout << " - ";
			if (!discTitle.empty()) std::cout << discTitle;
			std::cout << ")\n";

			cdTextViaWriteBuffer = WriteDiscInternal::SendCDTextToDevice(m_drive, cdTextPacks);
			cdTextViaLeadIn = !cdTextViaWriteBuffer;
			if (cdTextViaWriteBuffer)
				Console::Success("CD-Text queued via WRITE BUFFER (drive embeds it in the lead-in)\n");
			else
				Console::Info("WRITE BUFFER unavailable -- writing CD-Text into the lead-in (SAO)\n");
		}
	}

	// ── Send the disc layout (cue sheet) once, flagged for a CD-Text lead-in
	// only when we will write that lead-in ourselves ───────────────────
	bool layoutAccepted = false;
	if (WriteDiscInternal::PrepareDriveForWrite(m_drive, subchannelMode)) {
		Console::Info("\nSending disc layout to drive...\n");
		if (WriteDiscInternal::BuildAndSendCueSheet(
				m_drive, tracks, totalSectors, subchannelMode, true, false, cdTextViaLeadIn)) {
			layoutAccepted = true;
			Console::Success(subchannelMode == 2
				? "Using raw P-W mode (preserved/synthesized subchannel)\n"
				: "Using SAO mode (drive-generated subchannel)\n");
		}
		else if (cdTextViaLeadIn) {
			// The CD-Text lead-in flag may be what the drive rejected; retry the
			// cue sheet plain and burn audio without CD-Text.
			Console::Warning("Cue sheet with CD-Text lead-in flag rejected - retrying without CD-Text\n");
			cdTextViaLeadIn = false;
			if (WriteDiscInternal::BuildAndSendCueSheet(m_drive, tracks, totalSectors, subchannelMode)) {
				layoutAccepted = true;
				Console::Success(subchannelMode == 2
					? "Using raw P-W mode (preserved/synthesized subchannel)\n"
					: "Using SAO mode (drive-generated subchannel)\n");
			}
		}
	}

	// Modern Pioneer BD writers (e.g. BDR-S13U) reject the legacy raw-SCSI
	// SEND CUE SHEET (0x5D) path for CD-DA in *every* mode -- a firmware
	// limitation, not a malformed cue sheet. When no raw layout is accepted,
	// fall back to the Windows IMAPI2 Disc-At-Once writer.
	if (!layoutAccepted) {
		Console::Warning("Drive rejected every raw-SCSI disc layout\n");
		std::string fallbackWarning =
			"The drive rejected exact raw-SCSI writing.\n\n"
			"The Windows IMAPI fallback is NOT a 1:1 copy: it normalizes "
			"inter-track pregaps and may fall back to Track-At-Once.";
		const bool hasMetadata =
			WriteDiscInternal::HasCDTextContent(discTitle, discPerformer, tracks) ||
			!discMCN.empty() ||
			std::any_of(tracks.begin(), tracks.end(),
				[](const TrackWriteInfo& track) { return !track.isrcCode.empty(); });
		if (hasMetadata) {
			fallbackWarning +=
				"\nCD-Text, catalog number, and ISRC metadata will not be preserved.";
		}
		fallbackWarning += "\n\nWrite a non-exact fallback disc anyway?";
		if (!GuiInput::PromptYesNo("Non-exact IMAPI fallback",
			fallbackWarning.c_str())) {
			Console::Info("Write cancelled; the destination disc was not written\n");
			return false;
		}
		Console::Warning("User accepted a non-exact IMAPI2 fallback write\n");
		return WriteDiscIMAPI(binFile, tracks, totalSectors, speed);
	}

	// ── Write CD-Text into the lead-in (host method), the first phase of the
	// continuous SAO write; WriteAudioSectors then continues from LBA -150 ──
	if (cdTextViaLeadIn) {
		bool touchedDisc = false;
		if (WriteDiscInternal::SendCDTextLeadInToDevice(m_drive, cdTextPacks, &touchedDisc)) {
			Console::Success("CD-Text written into the lead-in\n");
		}
		else if (touchedDisc) {
			Console::Error("CD-Text lead-in failed after writing began - aborting\n");
			Console::Info("The lead-in is partially written; not continuing the burn.\n");
			return false;
		}
		else {
			// Rejected before committing anything -- recover a clean audio-only
			// session (reopen for a fresh cue sheet) so the burn still succeeds.
			Console::Warning("CD-Text lead-in path failed - burning audio without CD-Text\n");
			if (!m_drive.Reopen()) {
				Console::Error("Failed to reopen drive after CD-Text fallback\n");
				return false;
			}
			if (!ensureBlankWritableMedia(false, false) ||
				!verifyCapacity()) {
				Console::Error("Media validation failed after CD-Text recovery\n");
				return false;
			}
			m_drive.SetSpeed(speed, speed);  // re-apply speed dropped with the handle
			if (!WriteDiscInternal::PrepareDriveForWrite(m_drive, subchannelMode) ||
				!WriteDiscInternal::BuildAndSendCueSheet(m_drive, tracks, totalSectors, subchannelMode)) {
				Console::Error("Failed to restore normal write layout\n");
				return false;
			}
		}
	}

	Console::Info("\nWriting sectors...\n");
	if (!WriteAudioSectors(binFile, subFile, tracks, totalSectors,
		hasSubchannel, needsDeinterleave, subchannelMode, discMCN)) {
		Console::Error("Failed to write sectors\n");
		return false;
	}

	// In simulate (Test Write) mode nothing is committed, so readback
	// verification would always fail against the still-blank disc. Skip it and
	// report the dry-run as a success instead of a spurious write failure.
	{
		char sim[8] = {};
		DWORD simLen = GetEnvironmentVariableA("OPTISCAN_SIMULATE_WRITE", sim, sizeof(sim));
		if (simLen > 0 && simLen < sizeof(sim) && sim[0] != '0') {
			Console::Success("SIMULATE complete -- pipeline ran; nothing committed, verification skipped\n");
			return true;
		}
	}

	return VerifyWriteCompletion(binFile);
}
