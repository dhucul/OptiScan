#include "CopyWorkflow.h"
#include "FileUtils.h"
#include "ConsoleColors.h"
#include "AccurateRip.h"
#include "Drive.h"
#include "DriveSelection.h"
#include "GuiInput.h"
#include "Progress.h"
#include "MenuHelpers.h"
#include "PioneerVendor.h"
#include <windows.h>
#include <iostream>

namespace {
// Best-effort: on Pioneer drives, enable PureRead + Quiet speed mode +
// Fragile/Rental CD slow-rotation mode before extracting audio. The user
// chooses the PureRead mode (Master interpolates unreadable samples for a
// gap-free rip; Perfect reports an error instead of fabricating data). These
// settings are session-only (eepSave=false) so the drive returns to its prior
// state on next power-cycle. Silent no-op for non-Pioneer drives.
void TryApplyPioneerAudioPreset(OpticalDrive& copier) {
    PioneerVendor pioneer(copier.GetDriveRef());
    if (!pioneer.IsPioneerDrive()) return;
    PioneerCapabilities pc;
    if (!pioneer.ReadCapabilities(pc) || !pc.valid) return;

    // Let the user pick how the drive should treat audio it cannot read
    // cleanly after exhausting retries. Only meaningful when PureRead is
    // supported; otherwise we skip straight to applying the rest of the preset.
    PureReadMode chosenMode = PureReadMode::Master;
    if (pc.pureReadSupport) {
        std::cout << "\n=== Pioneer PureRead Mode ===\n";
        std::cout << "Controls how the drive handles audio it cannot read cleanly\n";
        std::cout << "after exhausting retries.\n\n";
        std::cout << "1. Master (recommended) - interpolate unreadable samples so the\n";
        std::cout << "   rip stays gap-free; a near-perfect estimate fills the gap.\n";
        std::cout << "2. Perfect (strictest)   - report a read error instead of\n";
        std::cout << "   interpolating; unrecoverable samples are never faked.\n";
        std::cout << "Choice: ";
        int c = GetMenuChoice("Pioneer PureRead Mode",
            "Controls how the drive handles audio it cannot read cleanly\n"
            "after exhausting retries.\n\n"
            "1. Master (recommended) - interpolate unreadable samples so the\n"
            "   rip stays gap-free; a near-perfect estimate fills the gap.\n"
            "2. Perfect (strictest)   - report a read error instead of\n"
            "   interpolating; unrecoverable samples are never faked.",
            1, 2, 1);
        chosenMode = (c == 2) ? PureReadMode::Perfect : PureReadMode::Master;
    }

    const char* modeName = (chosenMode == PureReadMode::Perfect) ? "Perfect" : "Master";

    bool changed = pioneer.ApplyAudioExtractionPreset(/*persist=*/false, chosenMode);
    if (changed) {
        char info[176];
        std::snprintf(info, sizeof(info),
            "Pioneer drive detected - enabling PureRead %s + Quiet + Fragile CD mode for this session.\n",
            modeName);
        Console::Info(info);
    }

    // Confirm PureRead actually engaged by reading the mode back from the drive.
    // ApplyAudioExtractionPreset returns true if ANY preset setting took (Quiet or
    // Fragile can succeed while the PureRead write silently fails), so the "enabled"
    // message above is not proof. Verify PureRead specifically against the drive.
    if (!pc.pureReadSupport) {
        Console::Warning("This Pioneer drive does not support PureRead - extracting without it.\n");
        return;
    }

    PureReadMode mode = PureReadMode::Off;
    bool realTime = false;
    if (pioneer.GetPureReadMode(mode, realTime) && mode != PureReadMode::Off) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "PureRead confirmed engaged (mode=%s%s).\n",
            mode == PureReadMode::Perfect ? "Perfect" : "Master",
            realTime ? ", Real-Time" : "");
        Console::Success(msg);
    }
    else {
        Console::Warning("PureRead did NOT engage - the drive reports it is Off. "
            "Proceeding without PureRead error recovery.\n");
    }
}
}

bool RunCopyWorkflow(OpticalDrive& copier, DiscInfo& disc, const std::wstring& /*workDir*/) {
	Console::Info("\n(Enter 0 at any prompt to go back to menu)\n");

	{
		std::vector<std::string> preflightWarnings;
		if (!copier.RunPreflightChecks(disc, preflightWarnings)) {
			std::string msg = "Disc preflight reported issues:\n";
			for (const auto& w : preflightWarnings) {
				msg += "  - " + w + "\n";
			}
			msg += "\nContinue anyway?";
			if (!GuiInput::PromptYesNo("Disc preflight", msg.c_str())) {
				Console::Info("Copy cancelled.\n");
				return false;
			}
		}
	}

	if (disc.sessionCount > 1) {
		// Enhanced/multi-session CDs: auto-select session 1 (audio session)
		// since the data session cannot be ripped properly.
		disc.selectedSession = 1;
		Console::Info("Multi-session disc detected - automatically using session 1 (audio).\n");
	}

	int speed = copier.SelectSpeed();
	if (speed == -1) return false;

	int subch = copier.SelectSubchannel();
	if (subch == -1) return false;
	disc.includeSubchannel = (subch == 1);

	int pregapMode = copier.SelectPregapMode();
	if (pregapMode == -1) return false;
	disc.pregapMode = static_cast<PregapMode>(pregapMode);

	int errorMode = copier.SelectErrorHandling();
	if (errorMode == -1) return false;

	int secureMode = copier.SelectSecureRipMode(speed);
	if (secureMode == -1) return false;

	bool isBurstMode = (secureMode == -2);
	SecureRipConfig secureConfig{};
	if (!isBurstMode) {
		secureConfig = copier.GetSecureRipConfig(static_cast<SecureRipMode>(secureMode));
	}

	disc.loggingOutput = LogOutput::File;

	bool hasAccurateStream = false;

	// Detect capabilities for ALL modes (including burst)
	{
		std::cout << "\nDetecting drive capabilities..." << std::flush;
		DriveCapabilities caps;
		if (copier.DetectDriveCapabilities(caps)) {
			disc.enableC2Detection = isBurstMode ? false : caps.supportsC2ErrorReporting;
			hasAccurateStream = caps.supportsAccurateStream;
			std::cout << " done.\n";

			TryApplyPioneerAudioPreset(copier);

			// Pioneer C2-enabled READ CD returns audio shifted from the bare 0xF8
			// read path that the AccurateRip-published drive offset was calibrated
			// against. Force C2 off on Pioneer for non-burst rips so the audio
			// matches AccurateRip; multi-pass hashing still provides verification.
			if (!isBurstMode && disc.enableC2Detection) {
				PioneerVendor pioneer(copier.GetDriveRef());
				if (pioneer.IsPioneerDrive()) {
					disc.enableC2Detection = false;
					Console::Info("Pioneer drive - disabling C2 for AccurateRip-compatible reads.\n");
				}
			}

			if (hasAccurateStream && !isBurstMode) {
				Console::Info("Drive supports Accurate Stream (jitter-free reads).\n");

				if (secureConfig.mode == SecureRipMode::Fast) {
					secureConfig.minPasses = 1;
					secureConfig.cacheDefeat = false;
				}
				else if (secureConfig.mode == SecureRipMode::Standard) {
					secureConfig.minPasses = 2;
					secureConfig.cacheDefeat = false;
				}
				else if (secureConfig.mode == SecureRipMode::Paranoid) {
					secureConfig.cacheDefeat = false;
				}
			}
			// No separate message for burst — the later auto-disable block handles it
			else if (hasAccurateStream && isBurstMode) {
				Console::Info("Drive supports Accurate Stream - cache defeat unnecessary in burst mode.\n");
			}
		}
		else {
			disc.enableC2Detection = false;
			std::cout << " skipped (detection failed).\n";
		}
	}

	int offset = copier.SelectOffset();
	if (offset == -1) return false;
	disc.driveOffset = offset;

	if (hasAccurateStream) {
		// Both read paths (standard + secure + burst) have cache defeat disabled
		// so skip the prompt entirely
		disc.enableCacheDefeat = false;
		Console::Info("Cache defeat auto-disabled (Accurate Stream drive).\n");
	}
	else {
		int cacheDefeat = copier.SelectCacheDefeat();
		if (cacheDefeat == -1) return false;
		disc.enableCacheDefeat = (cacheDefeat == 1);

		// Sync user's choice into secureConfig so both paths are consistent
		if (secureConfig.mode != SecureRipMode::Disabled) {
			secureConfig.cacheDefeat = disc.enableCacheDefeat;
		}
	}

	// Hide CDR Media (Plextor only) — applied just before the read,
	// restored unconditionally after the read.
	bool hideCDR = false;
	{
		int hideChoice = copier.SelectHideCDRMedia();
		if (hideChoice == -1) return false;
		hideCDR = (hideChoice == 1);
	}

	// Silent Mode (Plextor only) — same lifetime as HideCDR.
	bool silentMode = false;
	{
		int silentChoice = copier.SelectSilentMode();
		if (silentChoice == -1) return false;
		silentMode = (silentChoice == 1);
	}

	// Output directory via native folder picker.
	Console::Info("\nChoose the output directory for the disc copy...\n");
	std::wstring outputDir = GuiInput::PromptForFolder(L"Choose output directory for disc copy");
	if (outputDir.empty()) {
		Console::Info("Copy cancelled (no output directory selected).\n");
		return false;
	}
	outputDir = NormalizePath(outputDir);
	if (!outputDir.empty() && outputDir.back() != L'\\' && outputDir.back() != L'/') {
		outputDir += L"\\";
	}
	if (!CreateDirectoryRecursive(outputDir)) {
		Console::Error("Failed to create directory: ");
		std::wcerr << outputDir << L"\n";
		return false;
	}

	// Writability probe — surface permission issues before the long read.
	{
		std::wstring testFile = outputDir + L".audiocopy_test_" + std::to_wstring(GetTickCount()) + L".tmp";
		HANDLE hTest = CreateFileW(testFile.c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
		if (hTest == INVALID_HANDLE_VALUE) {
			Console::Error("Cannot write to directory: ");
			std::wcerr << outputDir << L"\n";
			if (GetLastError() == ERROR_ACCESS_DENIED) {
				Console::Warning("Access denied. Check folder permissions.\n");
			}
			return false;
		}
		CloseHandle(hTest);
	}

	// Synthesize a basename for the .bin/.cue/.log set from CD-TEXT when available.
	std::wstring defaultName = L"AudioCD";
	if (!disc.cdText.albumTitle.empty()) {
		std::string title = disc.cdText.albumTitle;
		if (!disc.cdText.albumArtist.empty()) {
			title = disc.cdText.albumArtist + " - " + title;
		}
		int len = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
		if (len > 1) {
			std::wstring wideTitle(static_cast<size_t>(len - 1), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, &wideTitle[0], len);
			std::wstring sanitized = SanitizeFilename(wideTitle);
			if (!sanitized.empty()) defaultName = sanitized;
		}
	}

	std::wstring path = outputDir + defaultName;
	Console::Success("Output directory: ");
	std::wcout << outputDir << L"\n";
	std::wcout << L"Using filename: " << path << L"\n";

	if (hideCDR) {
		if (copier.GetDriveRef().SetHideCDRMedia(true)) {
			Console::Info("Hide CDR Media: applied to drive.\n");
		}
		else {
			Console::Warning("Hide CDR Media: drive rejected the request - continuing without it.\n");
			hideCDR = false;
		}
	}

	if (silentMode) {
		if (copier.GetDriveRef().SetSilentMode(true)) {
			Console::Info("Silent Mode: applied to drive.\n");
		}
		else {
			Console::Warning("Silent Mode: drive rejected the request - continuing without it.\n");
			silentMode = false;
		}
	}

	{
		DWORD estSectors = 0;
		for (const auto& t : disc.tracks) {
			if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;
			DWORD start = (disc.pregapMode == PregapMode::Skip) ? t.startLBA : t.pregapLBA;
			if (t.endLBA >= start) estSectors += (t.endLBA - start + 1);
		}
		if (estSectors > 0 && !copier.CheckDiskSpace(outputDir, estSectors)) {
			char buf[160];
			std::snprintf(buf, sizeof(buf),
				"Insufficient free space at output path (need ~%llu MB). Continue anyway?",
				static_cast<unsigned long long>(estSectors) * AUDIO_SECTOR_SIZE / (1024 * 1024));
			if (!GuiInput::PromptYesNo("Low disk space", buf)) {
				Console::Info("Copy cancelled.\n");
				return false;
			}
		}
	}

	Console::Info("\nReading disc...\n");
	ProgressIndicator prog;
	prog.SetLabel("Reading");
	prog.Start();

	bool readSuccess = false;
	SecureRipResult secureResult;

	if (isBurstMode) {
		readSuccess = copier.ReadDiscBurst(disc, MakeProgressCallback(&prog), speed, errorMode);
	}
	else {
		// Use secure path even for Standard (single-pass) to avoid abort-on-first-error
		readSuccess = copier.ReadDiscSecure(disc, secureConfig, secureResult,
			MakeProgressCallback(&prog));
	}

	// Restore drive's normal media reporting before returning, regardless of
	// read success.  Failure to restore is non-fatal — drive state resets on
	// power-cycle or media change.
	if (hideCDR) {
		copier.GetDriveRef().SetHideCDRMedia(false);
	}
	if (silentMode) {
		copier.GetDriveRef().SetSilentMode(false);
	}

	if (!readSuccess) {
		prog.Finish(false);
		return false;
	}
	prog.Finish(true);

	auto extractedAnyAudio = [](const DiscInfo& d) {
		for (size_t i = 0; i < d.rawSectors.size(); i++) {
			const auto& s = d.rawSectors[i];
			size_t n = (s.size() < static_cast<size_t>(AUDIO_SECTOR_SIZE))
				? s.size() : static_cast<size_t>(AUDIO_SECTOR_SIZE);
			for (size_t j = 0; j < n; j++) {
				if (s[j] != 0) return true;
			}
		}
		return false;
	};

	// ── Sanity guard: did we actually extract any audio? ─────────────
	// The per-sector read paths substitute a zero-filled sector whenever a
	// read fails, and both ReadDiscBurst/ReadDiscSecure return success even if
	// every sector failed. Two failure shapes both end up as an all-zero rip:
	//   (a) the drive rejects our CD-DA READ CD form outright (e.g. some
	//       Hitachi-LG HL-DT-ST units) — every read fails and is zero-filled;
	//   (b) the drive returns digital silence with GOOD status.
	// Without this check the workflow would save a large all-zero .bin and
	// report "Copy complete", and VerifyWrittenFile (memory-vs-file) would
	// "pass" because both sides are identical zeros. A real audio CD is never
	// entirely silent, so an all-zero buffer means extraction failed — this is
	// the authoritative signal (a failed read always leaves a zeroed sector).
	{
		bool anyAudio = extractedAnyAudio(disc);

		if (!disc.rawSectors.empty() && !anyAudio && disc.enableC2Detection && !isBurstMode) {
			Console::Warning("\nInitial read produced only silence while C2 reads were enabled.\n");
			Console::Info("Retrying extraction once with C2 disabled for this drive...\n");

			disc.rawSectors.clear();
			disc.badSectors.clear();
			disc.errorCount = 0;
			disc.enableC2Detection = false;
			secureConfig.useC2 = false;
			secureConfig.c2Guided = false;

			if (hideCDR) {
				copier.GetDriveRef().SetHideCDRMedia(true);
			}
			if (silentMode) {
				copier.GetDriveRef().SetSilentMode(true);
			}

			ProgressIndicator retryProg;
			retryProg.SetLabel("Reading");
			retryProg.Start();
			secureResult = SecureRipResult{};
			readSuccess = copier.ReadDiscSecure(disc, secureConfig, secureResult,
				MakeProgressCallback(&retryProg));
			retryProg.Finish(readSuccess);

			if (hideCDR) {
				copier.GetDriveRef().SetHideCDRMedia(false);
			}
			if (silentMode) {
				copier.GetDriveRef().SetSilentMode(false);
			}

			if (!readSuccess) {
				return false;
			}

			anyAudio = extractedAnyAudio(disc);
			if (anyAudio) {
				Console::Success("Retry succeeded with C2 disabled.\n");
			}
		}

		if (!disc.rawSectors.empty() && !anyAudio) {
			// badSectors only flavors the message — the trigger is the all-zero
			// buffer above, which is robust regardless of how the drive reported
			// the failures.
			bool mostlyRejected = (!disc.rawSectors.empty() &&
				disc.badSectors.size() * 2 >= disc.rawSectors.size());

			Console::Error("\nNo audio was extracted - the copy contains only digital silence.\n");
			if (mostlyRejected) {
				Console::Warning("The drive rejected the CD-DA read command for the whole disc. This\n"
					"drive may not support the read form OptiScan uses for extraction.\n");

				// Report the actual SCSI sense from the last failed read so the
				// cause is diagnosable (e.g. 05/64/00 = illegal mode for this
				// track; 05/20/00 = invalid command; 05/24/00 = invalid CDB field).
				BYTE sk = 0, asc = 0, ascq = 0;
				if (copier.GetDriveRef().GetLastReadSense(sk, asc, ascq)) {
					std::string desc = copier.GetDriveRef().GetSenseDescription(sk, asc, ascq);
					char senseMsg[256];
					std::snprintf(senseMsg, sizeof(senseMsg),
						"Drive reported: %s (sense KEY/ASC/ASCQ = %02X/%02X/%02X).\n",
						desc.c_str(), sk, asc, ascq);
					Console::Warning(senseMsg);
				}
			}
			else {
				Console::Warning("The drive returned silence for the entire disc. It may be refusing\n"
					"digital audio extraction, or the disc could not be read.\n");
			}
			if (errorMode == 1) {
				Console::Error("Aborting - error handling is set to \"Abort on error\" and no audio was extracted.\n");
				Console::Info("No file written. Re-run with a skip mode to save a best-effort copy.\n");
				return false;
			}
			if (!GuiInput::PromptYesNo("No audio extracted",
				"No audio data was extracted - the result is completely silent.\n\n"
				"Save the empty (all-zero) file anyway?")) {
				Console::Info("Copy cancelled - no file written.\n");
				return false;
			}
			Console::Warning("Saving silent copy at user request.\n");
		}
	}

	// Error-handling policy: "Abort on error" refuses to save a rip that still
	// contains unrecoverable sectors. Burst aborts mid-read (handled in the burst
	// engine); the secure path runs its full multi-pass recovery first, so the
	// abort decision is made here on whatever stayed unrecoverable. Skip modes
	// (2/3) fall through and save with the bad sectors zero-filled. errorCount
	// counts only hard-unrecoverable sectors; sectors that got best-effort data
	// but fell short of secure verification are reported in the secure log.
	if (errorMode == 1 && disc.errorCount > 0) {
		char abortMsg[176];
		std::snprintf(abortMsg, sizeof(abortMsg),
			"\nAborting: %d sector(s) could not be read and error handling is set to \"Abort on error\".\n",
			disc.errorCount);
		Console::Error(abortMsg);
		Console::Info("No file was written. Re-run with a skip mode to save a best-effort (zero-filled) copy.\n");
		return false;
	}

	// Ensure offset correction is applied before saving.
	if (disc.driveOffset != 0) {
		copier.ApplyOffsetCorrection(disc);
	}

	Console::Info("Saving files...\n");
	if (!copier.SaveToFile(disc, path)) {
		Console::Error("Failed to save!\n");
		return false;
	}

	if (disc.pregapMode == PregapMode::Include) {
		std::vector<DWORD> mismatched;
		if (!copier.VerifyWrittenFile(path + L".bin", disc, mismatched)) {
			Console::Warning("Written .bin file did not match in-memory disc data.\n");
		}
	}

	std::wstring logPath = path + L".log";
	if (copier.SaveReadLog(disc, logPath)) {
		Console::Success("Log saved to: ");
		std::wcout << logPath << L"\n";
	}

	if (secureConfig.mode != SecureRipMode::Disabled && secureConfig.mode != SecureRipMode::Burst) {
		std::wstring secureLogPath = path + L"_secure.log";
		if (copier.SaveSecureRipLog(secureResult, secureLogPath)) {
			Console::Success("Secure rip log saved to: ");
			std::wcout << secureLogPath << L"\n";
		}
	}

	copier.Eject();

	Console::Success("\nCopy complete. Files written to: ");
	std::wcout << outputDir << L"\n";
	Console::Info("Output basename: ");
	std::wcout << path << L"\n";
	return true;
}

void RunWriteDiscWorkflow(OpticalDrive& copier, const std::wstring& workDir,
	wchar_t& audioDrive) {
	// ── Burner drive selection ──────────────────────────────────────
	// The drive opened at startup is the audio-source drive — likely not the
	// burner. Let the user pick which CD/DVD drive to write with, defaulting
	// to whichever is already open. Scan quietly — SelectWriterDrive will print
	// the numbered list itself, so a verbose ScanDrives would duplicate it.
	{
		std::vector<wchar_t> audioDrives;
		std::vector<wchar_t> cdDrives = ScanDrives(audioDrives, /*verbose=*/false);
		if (cdDrives.empty()) {
			Console::Error("No CD/DVD drives detected.\n");
			return;
		}

		wchar_t pick = audioDrive;
		if (cdDrives.size() > 1) {
			pick = SelectWriterDrive(cdDrives, audioDrive);
			if (!pick) {
				Console::Info("Write cancelled.\n");
				return;
			}
		}
		else {
			pick = cdDrives.front();
		}

		if (pick != audioDrive) {
			copier.Close();
			if (!copier.Open(pick)) {
				Console::Error("Failed to open selected drive.\n");
				// Try to reopen the original audio drive so subsequent menu
				// actions still have something to work with.
				copier.Open(audioDrive);
				return;
			}
			audioDrive = pick;
			Console::Info("Using drive ");
			std::cout << static_cast<char>(audioDrive) << ":\n";
		}

		// Make sure media is loaded before going further — picking the burner
		// often means the user still needs to swap in a blank.
		if (!copier.GetDriveRef().TestUnitReady()) {
			char prompt[160];
			std::snprintf(prompt, sizeof(prompt),
				"Insert a blank CD-R or CD-RW into drive %c: and click OK.",
				static_cast<char>(audioDrive));
			if (!GuiInput::PromptYesNo("Insert disc", prompt)) {
				Console::Info("Write cancelled.\n");
				return;
			}
			if (!copier.GetDriveRef().WaitForDriveReady(30)) {
				Console::Error("Drive did not become ready with a disc.\n");
				return;
			}
		}
	}

	// ── FIX #4: Early check — verify drive supports writing ─────────
	DriveCapabilities caps;
	if (copier.DetectDriveCapabilities(caps)) {
		if (caps.maxWriteSpeedKB == 0) {
			Console::Error("Drive does not support disc writing\n");
			return;
		}
	}

	// Detect disc type and status. Quiet: WriteDisc re-reports the media type
	// just before the burn, so suppress the duplicate readout here.
	bool isFull, isRewritable;
	if (!copier.CheckRewritableDisk(isFull, isRewritable, /*quiet=*/true)) {
		Console::Error("Cannot determine disc type\n");
		return;
	}

	if (isFull && !isRewritable) {
		Console::Error("Disc is full and not rewritable - cannot write\n");
		return;
	}

	// Track speed selected during erase so we can reuse it for writing
	int eraseSpeed = -1;
	bool wasBlanked = false;

	// Offer to erase only if the disc is actually CD-RW
	if (isRewritable && isFull) {
		Console::Warning("CD-RW disc is full. Erase it first?\n");
		std::cout << "1. Quick erase (fast, recommended)\n";
		std::cout << "2. Full erase (thorough, slower)\n";
		std::cout << "3. Cancel\n";
		std::cout << "Choice: ";
		bool ok = false;
		int choice = GetMenuChoice("Erase full CD-RW before writing?",
			"1. Quick erase (fast, recommended)\n"
			"2. Full erase (thorough, slower)\n"
			"3. Cancel",
			1, 3, 1, &ok);

		if (!ok || choice == 3) {
			Console::Info("Write cancelled\n");
			return;
		}

		bool quickBlank = (choice == 1);
		eraseSpeed = copier.SelectWriteSpeed();
		if (eraseSpeed == -1) return;
		if (!copier.BlankRewritableDisk(eraseSpeed, quickBlank)) {
			return;
		}
		wasBlanked = true;

		if (!GuiInput::PromptYesNo("Continue?", "Continue with writing now?")) return;
	}
	else if (isRewritable && !isFull) {
		// CD-RW with space — optionally erase before writing
		Console::Info("CD-RW disc detected with available space.\n");
		std::cout << "1. Write directly\n";
		std::cout << "2. Quick erase first\n";
		std::cout << "3. Full erase first\n";
		std::cout << "4. Erase only (no write)\n";
		std::cout << "Choice: ";
		bool ok = false;
		int choice = GetMenuChoice("CD-RW with free space - what now?",
			"1. Write directly\n"
			"2. Quick erase first\n"
			"3. Full erase first\n"
			"4. Erase only (no write)",
			1, 4, 1, &ok);
		if (!ok) { Console::Info("Write cancelled\n"); return; }

		// ── FIX #1: Erase-only now offers quick vs full blank ───────
		if (choice == 4) {
			std::cout << "\nErase type:\n";
			std::cout << "1. Quick erase (fast)\n";
			std::cout << "2. Full erase (thorough)\n";
			std::cout << "Choice: ";
			bool eraseOk = false;
			int eraseType = GetMenuChoice("Erase type",
				"1. Quick erase (fast)\n"
				"2. Full erase (thorough)",
				1, 2, 1, &eraseOk);
			if (!eraseOk) { Console::Info("Cancelled\n"); return; }
				int speed = copier.SelectWriteSpeed();
			if (speed == -1) return;
			copier.BlankRewritableDisk(speed, eraseType == 1);
			return;
		}

		if (choice == 2 || choice == 3) {
			bool quickBlank = (choice == 2);
			eraseSpeed = copier.SelectWriteSpeed();
			if (eraseSpeed == -1) return;
			if (!copier.BlankRewritableDisk(eraseSpeed, quickBlank)) {
				return;
			}
			wasBlanked = true;

			if (!GuiInput::PromptYesNo("Continue?", "Continue with writing now?")) return;
		}
	}

	// Auto-detect .bin/.cue/.sub files from folder via native folder picker.
	Console::Info("\nChoose the folder containing .bin/.cue/.sub files...\n");
	std::wstring folder = GuiInput::PromptForFolder(
		L"Choose folder with .bin/.cue/.sub files", workDir);
	if (folder.empty()) {
		Console::Info("Cancelled (no source folder selected).\n");
		return;
	}
	folder = NormalizePath(folder);
	while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/'))
		folder.pop_back();

	// Scan folder for .bin, .cue, .sub files
	std::wstring binFile, cueFile, subFile;
	WIN32_FIND_DATAW fd;
	HANDLE hFind = FindFirstFileW((folder + L"\\*").c_str(), &fd);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			std::wstring name(fd.cFileName);
			std::wstring lower = name;
			for (auto& ch : lower) ch = towlower(ch);

			if (lower.size() > 4 && lower.substr(lower.size() - 4) == L".bin" && binFile.empty())
				binFile = folder + L"\\" + name;
			else if (lower.size() > 4 && lower.substr(lower.size() - 4) == L".cue" && cueFile.empty())
				cueFile = folder + L"\\" + name;
			else if (lower.size() > 4 && lower.substr(lower.size() - 4) == L".sub" && subFile.empty())
				subFile = folder + L"\\" + name;
		} while (FindNextFileW(hFind, &fd));
		FindClose(hFind);
	}

	// Validate required files
	if (binFile.empty()) {
		Console::Error("No .bin file found in folder\n");
		return;
	}
	if (cueFile.empty()) {
		Console::Error("No .cue file found in folder\n");
		return;
	}

	// Display detected files
	Console::Success("Detected files:\n");
	std::wcout << L"  BIN: " << binFile << L"\n";
	std::wcout << L"  CUE: " << cueFile << L"\n";
	// Subchannel writing always uses the SAO path: the drive generates the
	// subchannel from the cue sheet, which is reliable and reproduces pregaps
	// exactly. Raw P-W subchannel writing was removed because many drives accept
	// the mode but cannot actually perform it, producing a garbage burn.
	if (!subFile.empty()) {
		std::wcout << L"  SUB: " << subFile << L"\n";
		Console::Info("Using SAO path -- .sub will not be written, pregaps still exact.\n");
	}
	else {
		Console::Warning("No .sub file found - writing without subchannel data\n");
	}

	// ── FIX #2: Reuse erase speed if already selected, else prompt ──
	int speed;
	if (wasBlanked) {
		Console::Info("Using previously selected write speed (");
		std::cout << eraseSpeed << "x)\n";
		speed = eraseSpeed;
	}
	else {
		speed = copier.SelectWriteSpeed();
		if (speed == -1) return;
	}

	// Ask about power calibration
	Console::Info("\nUse power calibration?\n1. Yes (recommended)\n2. No\nChoice: ");
	int calibChoice = GetMenuChoice("Use power calibration?",
		"Optical Power Calibration tunes laser power to the loaded disc.\n\n"
		"1. Yes (recommended)\n"
		"2. No",
		1, 2, 1);
	bool useCal = (calibChoice == 1);

	// Plextor-only: optional test-write + VariRec tuning
	bool plxTestWrite = false;
	bool plxVariRecOn = false;
	int  plxVariRecOff = 0;
	if (copier.SelectPlextorWriteOptions(plxTestWrite, plxVariRecOn, plxVariRecOff) == -1) {
		return;
	}

	// Apply Plextor write-time vendor settings
	if (plxTestWrite) {
		if (copier.GetDriveRef().SetPlextorTestWrite(true)) {
			Console::Warning("TEST WRITE MODE - laser will stay at read power; nothing will be burned.\n");
		}
		else {
			Console::Warning("Test write: drive rejected the request - proceeding with a real burn.\n");
			plxTestWrite = false;
		}
	}
	if (plxVariRecOn) {
		if (copier.GetDriveRef().SetVariRecCD(true, plxVariRecOff)) {
			Console::Info("VariRec applied (offset ");
			std::cout << plxVariRecOff << ")\n";
		}
		else {
			Console::Warning("VariRec: drive rejected the request - using factory strategy.\n");
			plxVariRecOn = false;
		}
	}

	// Perform write. Subchannel is always drive-generated via the SAO path; the
	// .sub file is no longer written (raw P-W subchannel writing was removed).
	bool writeOk = copier.WriteDisc(binFile, cueFile, subFile, speed, useCal,
		wasBlanked);

	// Restore drive state regardless of outcome
	if (plxTestWrite) copier.GetDriveRef().SetPlextorTestWrite(false);
	if (plxVariRecOn) copier.GetDriveRef().SetVariRecCD(false, 0);

	if (writeOk) {
		Console::Success(plxTestWrite
			? "Test write completed successfully (no data burned)\n"
			: "Disc write completed successfully\n");
	}
	else {
		Console::Error("Disc write failed\n");
	}
}
