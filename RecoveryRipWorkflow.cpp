#include "RecoveryRipWorkflow.h"
#include "FileUtils.h"
#include "ConsoleColors.h"
#include "GuiInput.h"
#include "Progress.h"
#include "MenuHelpers.h"
#include "Preservation.h"
#include "RecoveryCheckpoint.h"
#include <windows.h>
#include <iostream>

bool RunRecoveryRipWorkflow(OpticalDrive& copier, DiscInfo& disc,
	const std::wstring& /*workDir*/) {
	struct RawSectorRelease {
		DiscInfo& value;
		~RawSectorRelease() { value.rawSectors.clear(); value.rawSectors.shrink_to_fit(); }
	} rawSectorRelease{ disc };

	Console::Heading("\n=== Recovery Rip (drive-independent) ===\n");
	Console::Info("Rebuilds hard sectors from cross-read consensus rather than\n");
	Console::Info("trusting the drive's C2. Slow but resilient on damaged discs.\n");
	Console::Info("(Enter 0 / Cancel at any prompt to go back to the menu.)\n");

	{
		std::vector<std::string> warnings;
		if (!copier.RunPreflightChecks(disc, warnings)) {
			std::string msg = "Disc preflight reported issues:\n";
			for (const auto& w : warnings) msg += "  - " + w + "\n";
			msg += "\nContinue anyway?";
			if (!GuiInput::PromptYesNo("Disc preflight", msg.c_str())) {
				Console::Info("Recovery rip cancelled.\n");
				return false;
			}
		}
	}

	if (disc.sessionCount > 1) {
		disc.selectedSession = 1;
		Console::Info("Multi-session disc — using session 1 (audio).\n");
	}

	// Speed: low speeds read marginal media more reliably. This doubles as the
	// engine's per-pass speed cap.
	int speed = copier.SelectScanSpeed();

	int subch = copier.SelectSubchannel();
	if (subch == -1) return false;
	disc.includeSubchannel = (subch == 1);

	int pregapMode = copier.SelectPregapMode();
	if (pregapMode == -1) return false;
	disc.pregapMode = static_cast<PregapMode>(pregapMode);

	bool offsetOk = false;
	int offset = copier.SelectOffset(&offsetOk);
	if (!offsetOk) return false;
	disc.driveOffset = offset;

	// The hybrid C2 tie-break can only engage if the drive reports C2 at all.
	DriveCapabilities driveCaps;
	bool haveDriveCaps = false;
	{
		std::cout << "\nDetecting drive capabilities..." << std::flush;
		if (copier.DetectDriveCapabilities(driveCaps)) {
			haveDriveCaps = true;
			disc.enableC2Detection = driveCaps.supportsC2ErrorReporting;
			std::cout << " done.\n";
		}
		else {
			disc.enableC2Detection = false;
			std::cout << " skipped (detection failed).\n";
		}
	}

	RecoveryRipConfig config;
	config.maxSpeed = speed;        // SelectScanSpeed returns 0 for "max"
	config.cacheDefeat = true;

	// Output directory via native folder picker.
	Console::Info("\nChoose the output directory for the recovered image...\n");
	std::wstring outputDir = GuiInput::PromptForFolder(L"Choose output directory for recovery rip");
	if (outputDir.empty()) {
		Console::Info("Recovery rip cancelled (no output directory selected).\n");
		return false;
	}
	outputDir = NormalizePath(outputDir);
	if (!outputDir.empty() && outputDir.back() != L'\\' && outputDir.back() != L'/')
		outputDir += L"\\";
	if (!CreateDirectoryRecursive(outputDir)) {
		Console::Error("Failed to create directory: ");
		std::wcerr << outputDir << L"\n";
		return false;
	}

	// Synthesize a basename from CD-TEXT when available.
	std::wstring defaultName = L"AudioCD_recovered";
	if (!disc.cdText.albumTitle.empty()) {
		std::string title = disc.cdText.albumTitle;
		if (!disc.cdText.albumArtist.empty())
			title = disc.cdText.albumArtist + " - " + title;
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
				Console::Info("Recovery rip cancelled.\n");
				return false;
			}
		}
	}

	Console::Info("\nReading disc (recovery engine)...\n");
	ProgressIndicator prog;
	prog.SetLabel("Recover");
	prog.Start();

	RecoveryRipResult result;
	bool ok = copier.ReadDiscRecovery(disc, config, result,
		MakeProgressCallback(&prog), path);
	prog.Finish(ok);
	if (!ok) {
		Console::Warning("Recovery stopped. Resume sidecars were kept beside the planned output.\n");
		return false;
	}

	if (disc.driveOffset != 0)
		copier.ApplyOffsetCorrection(disc);

	DataValidationSummary dataValidation;
	const bool hasDataTracks = ValidateDataTracks(disc, dataValidation);
	PreservationOffsetResult preservationOffset =
		AnalyzePreservationWriteOffset(disc);

	Console::Info("Saving files...\n");
	if (!copier.SaveToFile(disc, path)) {
		Console::Error("Failed to save!\n");
		return false;
	}

	std::vector<DWORD> mismatched;
	if (!copier.VerifyWrittenArtifacts(path, disc, mismatched)) {
		Console::Error("Written recovery artifacts did not match in-memory data.\n");
		return false;
	}

	std::wstring reportPath = path + L"_recovery.txt";
	if (copier.SaveRecoveryReport(result, reportPath)) {
		Console::Success("Recovery report saved to: ");
		std::wcout << reportPath << L"\n";
	}
	else {
		Console::Error("Failed to save the recovery report.\n");
		return false;
	}

	std::vector<std::wstring> artifacts =
		CollectPreservationArtifacts(path, disc);
	artifacts.push_back(reportPath);
	if (hasDataTracks) {
		std::wstring validationPath = path + L"_data_validation.txt";
		if (!WriteDataValidationReport(dataValidation, validationPath)) {
			Console::Error("Failed to save raw data-track validation report.\n");
			return false;
		}
		artifacts.push_back(validationPath);
	}

	PreservationManifestContext manifest;
	manifest.workflow = "Recovery rip";
	manifest.artifacts = artifacts;
	manifest.drive = haveDriveCaps ? &driveCaps : nullptr;
	manifest.recovery = &result;
	manifest.dataValidation = hasDataTracks ? &dataValidation : nullptr;
	manifest.writeOffset = &preservationOffset;
	std::wstring manifestPath = path + L".manifest.json";
	if (!WritePreservationManifest(disc, manifest, manifestPath)) {
		Console::Error("Failed to create preservation manifest.\n");
		return false;
	}
	Console::Success("Preservation manifest saved to: ");
	std::wcout << manifestPath << L"\n";
	if (preservationOffset.detected) {
		std::cout << "Preservation write-offset estimate: "
			<< preservationOffset.sampleOffset << " sample(s), "
			<< preservationOffset.confidencePercent << "% confidence (not applied).\n";
	}

	const bool recoveryComplete =
		result.partial == 0 &&
		result.unrecovered == 0 &&
		result.subchannelFailures == 0;
	if (recoveryComplete) {
		if (!RemoveRecoveryCheckpointFiles(path))
			Console::Warning("Output is complete, but recovery sidecars could not be removed.\n");
	}
	else {
		Console::Warning("Recovery sidecars were retained so another pass or drive "
			"can improve the incomplete sectors.\n");
	}

	if (result.unrecovered > 0) {
		Console::Warning("Some sectors were never readable — see the recovery report.\n");
	}
	if (result.partial > 0) {
		Console::Warning("Some bytes could not reach consensus — see the recovery report.\n");
	}
	if (result.subchannelFailures > 0) {
		Console::Warning("Some requested subchannel sectors were unreadable — "
			"see the recovery report.\n");
	}
	if (recoveryComplete) {
		Console::Success("All problem sectors rebuilt by consensus.\n");
	}

	if (result.c2DisputedBytes > 0) {
		char buf[200];
		std::snprintf(buf, sizeof(buf),
			"%lld byte(s) in %d sector(s) were accepted by consensus but flagged "
			"by the drive's C2 — see C2FlaggedBytes in the recovery report.\n",
			result.c2DisputedBytes, result.c2DisputedSectors);
		Console::Warning(buf);
	}

	copier.Eject();

	Console::Success("\nRecovery rip complete. Files written to: ");
	std::wcout << outputDir << L"\n";
	return true;
}
