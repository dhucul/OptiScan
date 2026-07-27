#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleColors.h"
#include "GuiInput.h"
#include "Progress.h"
#include "InterruptHandler.h"
#include "WriteDiscInternal.h"
#include "PioneerVendor.h"
#include <iostream>
#include <fstream>
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

	// ── Check disc is empty and writable ────────────────────────────
	if (!discAlreadyBlanked) {
		Console::Info("Checking disc media status...\n");

		WriteDiscInternal::WaitForDriveReady(m_drive, 10);

		BYTE discInfoCmd[10] = { 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0x00 };
		BYTE discInfoResp[252] = { 0 };
		BYTE sk = 0, asc = 0, ascq = 0;

		if (m_drive.SendSCSIWithSense(discInfoCmd, sizeof(discInfoCmd),
			discInfoResp, sizeof(discInfoResp), &sk, &asc, &ascq, true)) {

			BYTE discStatus = discInfoResp[2] & 0x03;
			bool isErasable = (discInfoResp[2] & 0x10) != 0;

			// Surface the media type up front so the user can confirm CD-R vs
			// CD-RW before the burn. The per-status messages below don't state
			// it in the common "empty disc and ready" case, where a
			// brand/dye-related write failure is most likely to surprise the user.
			Console::Success("Disc type: ");
			std::cout << (isErasable ? "CD-RW (rewritable)\n" : "CD-R (write-once)\n");

			switch (discStatus) {
			case 0x00:
				Console::Success("Disc is empty and ready for writing\n");
				break;

			case 0x01: // Appendable -- has an open/incomplete session
				if (isErasable) {
					Console::Warning("CD-RW disc has an incomplete session\n");
					Console::Info("The disc must be blanked before SAO writing\n");
					if (!BlankRewritableDisk(speed, true)) {
						Console::Error("Failed to blank disc -- write cancelled\n");
						return false;
					}
				}
				else {
					Console::Error("CD-R already has data and cannot be erased\n");
					Console::Info("Insert a blank CD-R disc and try again\n");
					return false;
				}
				break;

			case 0x02: // Complete -- fully written
				if (isErasable) {
					Console::Warning("CD-RW disc is not empty (fully written)\n");
					if (!GuiInput::PromptYesNo("Blank disc?",
						"The disc must be blanked before writing. Blank now?")) {
						Console::Info("Write operation cancelled\n");
						return false;
					}
					if (!BlankRewritableDisk(speed, true)) {
						Console::Error("Failed to blank disc\n");
						return false;
					}
				}
				else {
					Console::Error("CD-R is fully written and cannot be erased\n");
					Console::Info("Insert a blank CD-R disc and try again\n");
					return false;
				}
				break;

			default:
				Console::Warning("Unknown disc status (0x");
				std::cout << std::hex << static_cast<int>(discStatus)
					<< std::dec << ") -- attempting to write\n";
				break;
			}
		}
		else {
			// READ DISC INFORMATION failed -- try GET CONFIGURATION profile fallback
			BYTE profileCmd[10] = { 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00 };
			BYTE profileResp[8] = { 0 };

			if (m_drive.SendSCSI(profileCmd, sizeof(profileCmd),
				profileResp, sizeof(profileResp), true)) {
				WORD profile = (static_cast<WORD>(profileResp[6]) << 8) | profileResp[7];

				switch (profile) {
				case 0x08:
					Console::Error("Drive reports CD-ROM media (read-only)\n");
					return false;
				case 0x09:
					Console::Warning("CD-R detected but could not verify empty status\n");
					Console::Info("Proceeding -- write will fail if disc is not blank\n");
					break;
				case 0x0A:
					Console::Warning("CD-RW detected but could not verify empty status\n");
					Console::Info("Proceeding -- write will fail if disc is not blank\n");
					break;
				default:
					Console::Warning("Unknown media profile (0x");
					std::cout << std::hex << profile << std::dec
						<< ") -- attempting to write\n";
					break;
				}
			}
			else {
				Console::Warning("Could not determine disc status (");
				std::cout << m_drive.GetSenseDescription(sk, asc, ascq)
					<< ") -- attempting to write\n";
			}
		}
	}
	else {
		Console::Success("Disc was already blanked - skipping media check\n");
		WriteDiscInternal::WaitForDriveReady(m_drive, 10);
	}

	// Verify input files exist
	std::ifstream binStream(binFile, std::ios::binary);
	if (!binStream.is_open()) {
		Console::Error("Cannot open .bin file: ");
		std::wcout << binFile << L"\n";
		return false;
	}
	binStream.seekg(0, std::ios::end);
	long long fileSize = binStream.tellg();
	binStream.close();

	DWORD totalSectors = static_cast<DWORD>(fileSize / AUDIO_SECTOR_SIZE);

	// Subchannel is always drive-generated via the SAO path (reliable, and
	// pregaps are still reproduced exactly). Raw P-W subchannel writing from the
	// .sub file was removed, so hasSubchannel stays false here; we still validate
	// the .sub purely to give the user feedback about the file they supplied.
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
				Console::Info("Using SAO (drive-generated subchannel; pregaps still exact)\n");
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

	// Set last track endLBA now that we know totalSectors
	if (!tracks.empty()) {
		if (tracks.back().endLBA == 0) {
			// Last track in CUE — endLBA not set by parser, use BIN file size
			tracks.back().endLBA = totalSectors - 1;
		}
		else if (tracks.back().endLBA + 1 < totalSectors) {
			// Data tracks were filtered — BIN is larger than audio portion
			Console::Info("Trimming to audio content: ");
			std::cout << (tracks.back().endLBA + 1) << " of " << totalSectors << " sectors\n";
			totalSectors = tracks.back().endLBA + 1;
		}
	}

	// ── Verify disc has enough capacity for the image ───────────────
	// (after CUE parse so totalSectors reflects any trim)
	{
		// READ TRACK INFORMATION: type=1 (track number), track 0xFF (invisible/blank)
		BYTE trackInfoCmd[10] = { 0x52, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x24, 0x00 };
		BYTE trackInfoResp[36] = { 0 };
		if (m_drive.SendSCSI(trackInfoCmd, sizeof(trackInfoCmd),
			trackInfoResp, sizeof(trackInfoResp), true)) {
			DWORD freeBlocks = (static_cast<DWORD>(trackInfoResp[24]) << 24) |
				(static_cast<DWORD>(trackInfoResp[25]) << 16) |
				(static_cast<DWORD>(trackInfoResp[26]) << 8) |
				static_cast<DWORD>(trackInfoResp[27]);

			constexpr DWORD LEADOUT_OVERHEAD = 6750;
			constexpr DWORD PREGAP_OVERHEAD = 150;
			DWORD sectorsNeeded = totalSectors + PREGAP_OVERHEAD + LEADOUT_OVERHEAD;

			if (freeBlocks > 0 && sectorsNeeded > freeBlocks) {
				Console::Error("Image too large for disc (need ");
				std::cout << sectorsNeeded << " sectors, disc has "
					<< freeBlocks << " free)\n";
				Console::Info("Use a higher-capacity disc (e.g., 80-min or 90-min CD-R)\n");
				return false;
			}
		}
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
			if (!discAlreadyBlanked) {
				Console::Info("Resetting drive handle to clear read-session state before write...\n");
				if (!m_drive.Reopen()) {
					Console::Error("Failed to reopen drive after reset - aborting write\n");
					return false;
				}
				WriteDiscInternal::WaitForDriveReady(m_drive, 15);
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

	// Audio cue-sheet writing always uses SAO (write type 02h): 2352-byte blocks
	// with drive-generated subchannel. This is the only supported path -- raw P-W
	// subchannel writing was removed (drives that "accept" it often cannot perform
	// it). subchannelMode is therefore always 0; hasSubchannel/needsDeinterleave
	// stay false and are passed through to WriteAudioSectors only for signature
	// compatibility.
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
	if (WriteDiscInternal::PrepareDriveForWrite(m_drive, 0)) {
		Console::Info("\nSending disc layout to drive...\n");
		if (WriteDiscInternal::BuildAndSendCueSheet(
				m_drive, tracks, totalSectors, 0, true, false, cdTextViaLeadIn)) {
			layoutAccepted = true;
			Console::Success("Using SAO mode (drive-generated subchannel)\n");
		}
		else if (cdTextViaLeadIn) {
			// The CD-Text lead-in flag may be what the drive rejected; retry the
			// cue sheet plain and burn audio without CD-Text.
			Console::Warning("Cue sheet with CD-Text lead-in flag rejected - retrying without CD-Text\n");
			cdTextViaLeadIn = false;
			if (WriteDiscInternal::BuildAndSendCueSheet(m_drive, tracks, totalSectors, 0)) {
				layoutAccepted = true;
				Console::Success("Using SAO mode (drive-generated subchannel)\n");
			}
		}
	}

	// Modern Pioneer BD writers (e.g. BDR-S13U) reject the legacy raw-SCSI
	// SEND CUE SHEET (0x5D) path for CD-DA in *every* mode -- a firmware
	// limitation, not a malformed cue sheet. When no raw layout is accepted,
	// fall back to the Windows IMAPI2 Disc-At-Once writer.
	if (!layoutAccepted) {
		Console::Warning("Drive rejected every raw-SCSI disc layout\n");
		Console::Info("Switching to IMAPI2 Disc-At-Once fallback...\n");
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
			WriteDiscInternal::WaitForDriveReady(m_drive, 15);
			m_drive.SetSpeed(speed, speed);  // re-apply speed dropped with the handle
			if (!WriteDiscInternal::PrepareDriveForWrite(m_drive, 0) ||
				!WriteDiscInternal::BuildAndSendCueSheet(m_drive, tracks, totalSectors, 0)) {
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
