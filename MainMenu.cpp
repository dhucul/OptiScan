#include "MainMenu.h"
#include "AccurateRip.h"
#include "CopyWorkflow.h"
#include "Drive.h"
#include "DriveOffsetDatabase.h"
#include "DriveSelection.h"
#include "GuiInput.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include "MenuUI.h"
#include "PioneerVendor.h"
#include "Progress.h"
#include "ProtectionCheck.h"
#include "RecoveryRipWorkflow.h"
#include "TrackRipWorkflow.h"
#include "UpdateChecker.h"
#include "WriteTracksWorkflow.h"
#include <algorithm>
#include <iostream>
#include <thread>
#include <vector>

namespace {

// Pioneer CD Check standalone UI. Measurement mechanics live in the shared
// OpticalDrive engine so every workflow uses identical inspection-state,
// PureRead, retry, cancellation, cleanup, and validity semantics.
void RunPioneerCdCheck(OpticalDrive& copier, DiscInfo& disc) {
    PioneerVendor pioneer(copier.GetDriveRef());
    if (!pioneer.IsPioneerDrive()) {
        Console::Warning("Pioneer CD Check requires a Pioneer drive.\n");
        return;
    }

    PioneerCapabilities capabilities;
    if (pioneer.ReadCapabilities(capabilities) && capabilities.valid &&
        !capabilities.cdCheckSupport) {
        char message[160];
        std::snprintf(message, sizeof(message),
            "Pioneer CD Check support flag (0xF4 byte 44) reads 0x%02X. "
            "Trying the command because some firmware reports this flag incorrectly.\n",
            capabilities.raw[44]);
        Console::Warning(message);
    }

    bool choiceAccepted = false;
    const int choice = GetMenuChoice("Pioneer CD Check",
        "1. Quick radial scan (recommended)\n"
        "   Samples every 0.05 mm, matching Pioneer BD Drive Utility.\n\n"
        "2. Full scan\n"
        "   Measures the complete audio range; can take up to disc duration.",
        1, 2, 1, &choiceAccepted);
    if (!choiceAccepted) return;

    const PioneerCdCheckScanMode mode = choice == 1
        ? PioneerCdCheckScanMode::Quick : PioneerCdCheckScanMode::Full;

    Console::Heading("\n=== Pioneer CD Check (audio quality) ===\n");
    Console::Info("PureRead is temporarily disabled so errors remain visible.\n");

    PioneerCdCheckSummary summary;
    copier.RunPioneerCdCheckMeasurement(disc, mode, summary, "  CD Check");
    if (!summary.completed || summary.validSamples == 0)
        return;

    const PioneerCdCheckGrade grade = summary.sawInvalidMeasurement
        ? PioneerCdCheckGrade::D : summary.worstGrade;
    const PioneerCdCheckResult& report = summary.worstSample.valid
        ? summary.worstSample : summary.lastValid;

    std::cout << "\n  Mode:                     "
        << (mode == PioneerCdCheckScanMode::Quick
            ? "Quick (0.05 mm radial samples)" : "Full range") << "\n";
    std::cout << "  Valid measurements:       " << summary.validSamples;
    if (mode == PioneerCdCheckScanMode::Quick)
        std::cout << " / " << summary.plannedSamples;
    std::cout << "\n";
    std::cout << "  Worst C1 uncorr. frames:  " << summary.worstC1Frames << "\n";
    std::cout << "  Worst C2 uncorr. bytes:   " << summary.worstC2Bytes << "\n";
    if (report.teDataValid) {
        std::cout << "  Tracking-Error peak:      " << report.tePeak << "\n";
        std::cout << "  Tracking-Error integ.:    " << report.teIntegrationMax << "\n";
    }
    else {
        std::cout << "  Tracking-Error:           unavailable\n";
    }
    if (summary.sawInvalidMeasurement)
        Console::Warning("  One or more measurement windows were invalid.\n");

    std::cout << "  Pioneer grade:            Level "
        << PioneerCdCheckGradeName(grade) << "\n";

    switch (grade) {
    case PioneerCdCheckGrade::A:
        Console::Success("\nCondition: GOOD\n");
        Console::Info("  Good condition; no CD Check uncorrectable bytes were reported.\n");
        break;
    case PioneerCdCheckGrade::B:
        Console::Success("\nCondition: NORMAL\n");
        Console::Info("  Some areas may not read smoothly, but remain correctable.\n");
        break;
    case PioneerCdCheckGrade::C:
        Console::Warning("\nCondition: LOW\n");
        Console::Info("  Uncorrectable bytes were detected. Use secure extraction and verify\n");
        Console::Info("  the copy independently; PureRead may assist recovery during ripping.\n");
        break;
    case PioneerCdCheckGrade::D:
        Console::Error("\nCondition: BAD\n");
        Console::Info("  Severe uncorrectable or tracking errors were detected.\n");
        break;
    }
}

}  // namespace

int DispatchMenuChoice(OpticalDrive& copier, DiscInfo& disc,
                       const std::wstring& workDir, wchar_t& audioDrive,
                       bool& hasTOC, int choice) {
	{
		switch (choice) {

			// ════════════════════════════════════════════════════════════
			//  Ripping
			// ════════════════════════════════════════════════════════════

			// ── 1. Copy disc ────────────────────────────────────────────
		case 1:
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			RunCopyWorkflow(copier, disc, workDir);
			break;

			// ── 2. Rip tracks (WAV/FLAC) ────────────────────────────────
		case 2:
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			RunTrackRipWorkflow(copier, disc, workDir);
			break;

			// ── 3. Write disc ───────────────────────────────────────────
		case 3:
			RunWriteDiscWorkflow(copier, workDir, audioDrive);
			break;

			// ── 4. Write tracks to disc using current disc's pregaps ────
		case 4:
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			RunWriteTracksWorkflow(copier, disc, workDir, audioDrive);
			break;

			// ════════════════════════════════════════════════════════════
			//  Disc Quality
			// ════════════════════════════════════════════════════════════

			// ── 5. Hardware quality scan ─────────────────────────────────
		case 5: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			QCheckResult qcheckResult;
			if (copier.RunQCheckScan(disc, qcheckResult, speed)) {
				std::wstring logPath = workDir + L"\\qcheck_scan.csv";
				if (copier.SaveQCheckLog(qcheckResult, logPath)) {
					Console::Success("Q-Check scan log saved to: ");
					std::wcout << logPath << L"\n";
				}
			}
			else {
				if (!qcheckResult.supported) {
					Console::Warning("A hardware quality scan is not available on this drive.\n");
					Console::Info("Supported backends are Plextor Q-Check, Pioneer 0x3B/0x3C,\n");
					Console::Info("and LiteOn/MediaTek vendor quality scans.\n");
					Console::Info("\nOptions 7/8 can use READ CD C2 only when the drive reports a\n");
					Console::Info("functional C2 error-pointer bitmap; a zero-only unsupported\n");
					Console::Info("bitmap must not be interpreted as a clean disc.\n");
				}
				else {
					Console::Error("Q-Check scan failed.\n");
				}
			}
			break;
		}

			  // ── 6. C2 error scan ──────────────────────────────────────
		case 6: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			BlerResult c2Result;
			if (copier.RunC2Scan(disc, c2Result, speed)) {
				std::wstring logPath = workDir + L"\\c2_scan.csv";
				if (copier.SaveBlerLog(c2Result, logPath)) {
					Console::Success("C2 scan log saved to: ");
					std::wcout << logPath << L"\n";
				}
			}
			else {
				Console::Error("C2 scan failed.\n");
			}
			break;
		}

			  // ── 7. BLER scan (detailed) ─────────────────────────────────
		case 7: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			BlerResult result;
			if (copier.RunBlerScan(disc, result, speed)) {
				std::wstring logPath = workDir + L"\\bler_scan.csv";
				if (copier.SaveBlerLog(result, logPath)) {
					Console::Success("BLER log saved to: ");
					std::wcout << logPath << L"\n";
				}
			}
			else {
				Console::Error("BLER scan failed.\n");
			}
			break;
		}

			  // ── 8. Disc rot detection ───────────────────────────────────
		case 8: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			DiscRotAnalysis result;
			if (copier.RunDiscRotScan(disc, result, speed)) {
				std::wstring logPath = workDir + L"\\discrot_report.txt";
				if (copier.SaveDiscRotLog(result, logPath)) {
					Console::Success("Disc rot report saved to: ");
					std::wcout << logPath << L"\n";
				}
			}
			else {
				Console::Error("Disc rot scan failed.\n");
			}
			break;
		}

			  // ── 9. Generate surface map ─────────────────────────────────
		case 9: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			std::wstring mapFile = workDir + L"\\surface_map.csv";
			copier.GenerateSurfaceMap(disc, mapFile, speed);
			break;
		}

			  // ── 10. Multi-pass verification ─────────────────────────────
		case 10: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			std::cout << "\n=== Multi-Pass Verification ===\n";
			std::cout << "Select number of passes (2-10, recommended: 3): ";
			int passes = GetMenuChoice("Multi-Pass Verification",
				"Number of full read passes to compare for consistency.\n\n"
				"Enter a value from 2 to 10 (recommended: 3).",
				2, 10, 3);
			std::vector<MultiPassResult> results;
			copier.RunMultiPassVerification(disc, results, passes, speed);
			break;
		}

			  // ── 11. Compare disc CRCs (original vs. copy) ──────────────
		case 11: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }

			bool hasAudio = false;
			for (const auto& t : disc.tracks) { if (t.isAudio) { hasAudio = true; break; } }
			if (!hasAudio) {
				Console::Warning("No audio tracks found on this disc.\n");
				break;
			}

			Console::Heading("\n=== Compare Disc CRCs (Original vs. Copy) ===\n");
			Console::Info("Reads both discs and compares track CRCs.\n");
			Console::Info("Make sure the ORIGINAL disc is currently inserted.\n\n");

			int speed = copier.SelectSpeed();
			if (speed == -1) break;

			// ── Read original disc ─────────────────────────────────────
			Console::Info("Step 1/2: Reading original disc...\n");
			DiscInfo originalDisc = disc;
			originalDisc.rawSectors.clear();
			originalDisc.selectedSession = 0;
			originalDisc.enableCacheDefeat = false;
			originalDisc.pregapMode = PregapMode::Skip;
			originalDisc.enableC2Detection = false;
			originalDisc.includeSubchannel = false;

			// Reset endLBAs to canonical TOC boundaries (nextTrack.startLBA - 1)
			// so CRC windows match the copy disc, which has no pregap trimming.
			for (size_t i = 0; i + 1 < originalDisc.tracks.size(); i++) {
				originalDisc.tracks[i].endLBA = originalDisc.tracks[i + 1].startLBA - 1;
			}

			ProgressIndicator origProgress(40);
			origProgress.SetLabel("  Original");
			origProgress.Start();

			bool origOk = copier.ReadDiscBurst(originalDisc, [&origProgress](int cur, int tot) {
				origProgress.Update(cur, tot);
				}, speed);

			origProgress.Finish(origOk);

			if (!origOk) {
				Console::Error("Failed to read original disc.\n");
				break;
			}

			// Keep probe sectors for offset detection before freeing
			std::vector<std::vector<BYTE>> origProbe;
			{
				size_t total = originalDisc.rawSectors.size();
				size_t probeSize = std::min<size_t>(100, total);
				size_t midStart = (total > probeSize) ? (total - probeSize) / 2 : 0;
				origProbe.assign(originalDisc.rawSectors.begin() + midStart,
					originalDisc.rawSectors.begin() + midStart + probeSize);
			}

			// Compute original CRCs, then free the bulk data
			std::vector<std::pair<int, uint32_t>> originalCRCs;
			for (int i = 0; i < static_cast<int>(originalDisc.tracks.size()); i++) {
				if (originalDisc.tracks[i].isAudio) {
					originalCRCs.push_back({
						originalDisc.tracks[i].trackNumber,
						copier.CalculateTrackCRC(originalDisc, i)
						});
				}
			}
			originalDisc.rawSectors.clear();
			originalDisc.rawSectors.shrink_to_fit();

			Console::Success("Original disc read complete.\n\n");

			// ── Swap discs ─────────────────────────────────────────────
			copier.Eject();
			GuiInput::WaitForKey("Please insert the COPIED disc, then click OK.");

			copier.Close();
			Sleep(3000);

			if (!copier.Open(audioDrive)) {
				Console::Error("Failed to reopen drive.\n");
				break;
			}

			DiscInfo copyDisc;
			bool copyTOC = false;
			for (int attempt = 0; attempt < 10; attempt++) {
				if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
					Console::Warning("\n*** Cancelled by user ***\n");
					break;
				}
				if (copier.ReadTOC(copyDisc, true)) {  // skip pregap scan for CRC compare
					copyTOC = true;
					break;
				}
				Console::Info("Waiting for disc to become ready...\n");
				Sleep(2000);
			}

			if (!copyTOC) {
				Console::Error("Failed to read TOC of copied disc.\n");
				break;
			}

			// Validate the copy's own TOC against the original
			// (don't overwrite — the copy may have different LBA boundaries)
			{
				int origAudioCount = 0, copyAudioCount = 0;
				for (const auto& t : originalDisc.tracks) { if (t.isAudio) origAudioCount++; }
				for (const auto& t : copyDisc.tracks)     { if (t.isAudio) copyAudioCount++; }
				if (origAudioCount != copyAudioCount) {
					Console::Warning(("Audio track count differs (original: "
    + std::to_string(origAudioCount) + ", copy: "
    + std::to_string(copyAudioCount) + ")\n").c_str());
				}
			}

			copyDisc.pregapMode = PregapMode::Skip;
			copyDisc.enableC2Detection = false;
			copyDisc.enableCacheDefeat = false;
			copyDisc.includeSubchannel = false;

			ProgressIndicator copyProgress(40);
			copyProgress.SetLabel("  Copy");
			copyProgress.Start();

			bool copyOk = copier.ReadDiscBurst(copyDisc, [&copyProgress](int cur, int tot) {
				copyProgress.Update(cur, tot);
				}, speed);

			copyProgress.Finish(copyOk);

			if (!copyOk) {
				Console::Error("Failed to read copied disc.\n");
				break;
			}

			Console::Success("Copied disc read complete.\n");

			// ── Detect and compensate for write offset ─────────────────
			int detectedOffset = 0;
			if (!origProbe.empty() && !copyDisc.rawSectors.empty()) {
				// Build a matching mid-disc probe from the copy
				size_t total = copyDisc.rawSectors.size();
				size_t probeSize = std::min<size_t>(100, total);
				size_t midStart = (total > probeSize) ? (total - probeSize) / 2 : 0;
				std::vector<std::vector<BYTE>> copyProbe(
					copyDisc.rawSectors.begin() + midStart,
					copyDisc.rawSectors.begin() + midStart + probeSize);
				detectedOffset = copier.DetectSampleOffset(origProbe, copyProbe);
			}
			origProbe.clear();

			if (detectedOffset != 0) {
				Console::Info("\nWrite offset detected: ");
				std::cout << detectedOffset << " samples ("
					<< (detectedOffset * 4) << " bytes)\n";
				Console::Info("Compensating before CRC comparison...\n\n");
				copier.ApplySampleOffset(copyDisc.rawSectors, detectedOffset);
			}
			else {
				Console::Info("\nNo write offset detected (discs appear sample-aligned).\n\n");
			}

			// ── Compute copy CRCs (from offset-compensated data) ───────
			std::vector<std::pair<int, uint32_t>> copyCRCs;
			for (int i = 0; i < static_cast<int>(copyDisc.tracks.size()); i++) {
				if (copyDisc.tracks[i].isAudio) {
					copyCRCs.push_back({
						copyDisc.tracks[i].trackNumber,
						copier.CalculateTrackCRC(copyDisc, i)
						});
				}
			}
			copyDisc.rawSectors.clear();
			copyDisc.rawSectors.shrink_to_fit();

			// ── Compare ────────────────────────────────────────────────
			copier.CompareDiscCRCs(originalCRCs, copyCRCs);

			disc = copyDisc;
			hasTOC = true;
			break;
		}

			   // ════════════════════════════════════════════════════════════
			   //  Disc Information
			   // ════════════════════════════════════════════════════════════

			   // ── 12. Audio content analysis ─────────────────────────────
		case 12: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			AudioAnalysisResult result;
			copier.AnalyzeAudioContent(disc, result, speed);
			break;
		}

			   // ── 13. Disc fingerprint ──────────────────────────────────
		case 13: {
			if (!hasTOC) {
				Console::Error("This operation requires a disc with a valid TOC.\n"); break;
			}
			DiscFingerprint fingerprint;
			if (copier.GenerateDiscFingerprint(disc, fingerprint)) {
				copier.PrintDiscFingerprint(fingerprint);
				std::wstring fpPath = workDir + L"\\disc_fingerprint.txt";
				if (copier.SaveDiscFingerprint(fingerprint, fpPath)) {
					Console::Success("Fingerprint saved to: ");
					std::wcout << fpPath << L"\n";
				}
			}
			else {
				Console::Error("Failed to generate disc fingerprint.\n");
			}
			break;
		}

			   // ── 14. Lead area check ───────────────────────────────────
		case 14: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			copier.CheckLeadAreas(disc, speed);
			break;
		}

			   // ── 15. Subchannel integrity check ────────────────────────
		case 15: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			int errorCount = 0;
			Console::Info("\nChecking subchannel integrity...\n");
			if (copier.VerifySubchannelIntegrity(disc, errorCount, speed)) {
				if (errorCount == 0) {
					Console::Success("Subchannel data integrity verified - no errors found.\n");
				}
				else {
					Console::Warning("Subchannel errors detected: ");
					std::cout << errorCount << " issues found.\n";
				}
			}
			else {
				Console::Error("Failed to verify subchannel integrity.\n");
			}
			break;
		}

			   // ── 16. Verify subchannel burn status ─────────────────────
		case 16: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			SubchannelBurnResult burnResult;
			Console::Info("\nVerifying subchannel burn status...\n");
			if (!copier.VerifySubchannelBurnStatus(disc, burnResult, speed)) {
				Console::Error("Failed to verify subchannel burn status.\n");
			}
			break;
		}

			   // ── 17. Copy-protection check ─────────────────────────────
		case 17: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			RunProtectionCheck(copier, disc, workDir, speed);
			break;
		}

			   // ════════════════════════════════════════════════════════════
			   //  Drive Diagnostics
			   // ════════════════════════════════════════════════════════════

			   // ── 18. Drive capabilities ────────────────────────────────
		case 18: {
			DriveCapabilities caps;
			if (copier.DetectDriveCapabilities(caps)) {
				copier.PrintDriveCapabilities(caps);
				std::cout << "\n";
				copier.ShowDriveRecommendations();
			}
			else {
				Console::Error("Failed to query drive capabilities.\n");
			}
			break;
		}

			   // ── 19. Drive offset detection ────────────────────────────
		case 19: {
			OffsetDetectionResult offsetResult;
			Console::Info("\nDetecting drive read offset...\n");

			if (copier.DetectDriveOffset(offsetResult)) {
				Console::Success("Offset detected: ");
				std::cout << offsetResult.offset << " samples";
				std::cout << " (confidence: " << offsetResult.confidence << "%)\n";
				std::cout << "Method: " << offsetResult.details << "\n";
			}
			else {
				Console::Warning("Could not auto-detect offset.\n");
				Console::Info("Recommendation: Use a test disc or lookup at accuraterip.com/driveoffsets.htm\n");
			}
			break;
		}

			   // ── 20. C2 validation test ────────────────────────────────
		case 20: {
			Console::Info("\n=== C2 Validation Test ===\n");
			Console::Info("This test reads sectors at different speeds to verify C2 accuracy.\n");
			Console::Info("Inconsistent C2 results may indicate unreliable C2 reporting.\n\n");

			std::vector<DWORD> testLBAs;
			for (const auto& t : disc.tracks) {
				if (!t.isAudio) continue;
				DWORD mid = t.startLBA + (t.endLBA - t.startLBA) / 2;
				testLBAs.push_back(mid);
				if (testLBAs.size() >= 3) break;
			}
			if (testLBAs.empty()) {
				Console::Warning("No audio tracks found.\n");
				break;
			}

			ProgressIndicator prog;
			prog.SetLabel("Validating C2");
			prog.Start();

			bool cancelled = false;
			int passed = 0;
			int total = static_cast<int>(testLBAs.size());
			for (int idx = 0; idx < total; idx++) {
				if (InterruptHandler::Instance().IsInterrupted() || InterruptHandler::Instance().CheckEscapeKey()) {
					Console::Warning("\n*** C2 validation cancelled by user ***\n");
					cancelled = true;
					break;
				}
				DWORD lba = testLBAs[idx];
				Console::Info("Testing LBA: ");
				std::cout << lba << "\n";
				if (copier.ValidateC2Accuracy(lba))
					passed++;
				prog.Update(idx + 1, total);
			}

			prog.Finish(!cancelled && passed == total);

			if (!cancelled) {
				if (passed == total) {
					Console::Success("\nC2 Validation: PASSED\n");
					Console::Success("Your drive's C2 error reporting appears reliable.\n");
					std::string infoMsg = "C2 pointers were consistent across " + std::to_string(testLBAs.size()) + " sectors and multiple speeds.\n";
					Console::Info(infoMsg.c_str());
				}
				else {
					std::string failMsg = "\nC2 Validation: FAILED (" + std::to_string(passed) + "/" + std::to_string(testLBAs.size()) + " sectors passed)\n";
					Console::Warning(failMsg.c_str());
					Console::Warning("Your drive's C2 error reporting may be unreliable.\n");
					Console::Warning("Consider using BLER scan instead for quality checks.\n");
				}
			}
			break;
		}

			   // ── 21. Speed comparison test ─────────────────────────────
		case 21: {
			std::vector<SpeedComparisonResult> results;
			copier.RunSpeedComparisonTest(disc, results);
			break;
		}

			   // ── 22. Seek time analysis ────────────────────────────────
		case 22: {
			std::vector<SeekTimeResult> results;
			Console::Info("\nRunning seek time analysis...\n");
			if (copier.RunSeekTimeAnalysis(disc, results)) {
				Console::Success("Seek time analysis complete.\n");
			}
			else {
				Console::Error("Seek time analysis failed.\n");
			}
			break;
		}

			   // ── 23. Chipset identification ────────────────────────────
		case 23: {
			Console::Info("\nIdentifying drive chipset / controller...\n");
			ChipsetInfo chipsetInfo;
			if (copier.DetectChipset(chipsetInfo)) {
				copier.PrintChipsetInfo(chipsetInfo);
			}
			else {
				Console::Error("Failed to identify drive chipset.\n");
			}
			break;
		}

			   // ── 24. Disc balance check ────────────────────────────────
		case 24: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int balanceScore = 0;
			Console::Info("\nRunning disc balance check...\n");
			if (copier.CheckDiscBalance(disc, balanceScore)) {
				Console::Success("Disc balance check complete.\n");
			}
			else {
				Console::Error("Disc balance check failed.\n");
			}
			break;
		}

			   // ════════════════════════════════════════════════════════════
			   //  Utility
			   // ════════════════════════════════════════════════════════════

			   // ── 25. Rescan disc ───────────────────────────────────────
		case 25: {
			Console::Info("\nScanning drives...\n");
			wchar_t newAudioDrive = 0;
			std::vector<wchar_t> newAudioDrives;
			std::vector<wchar_t> newCdDrives = ScanDrives(newAudioDrives);

			if (newCdDrives.empty()) {
				Console::Error("No CD/DVD drives found!\n");
				break;
			}

			if (newAudioDrives.size() == 1) {
				newAudioDrive = newAudioDrives[0];
			}
			else if (newAudioDrives.size() > 1) {
				newAudioDrive = SelectAudioDrive(newAudioDrives);
			}

			if (!newAudioDrive) {
				newAudioDrive = WaitForDisc(newCdDrives, 0);
				if (!newAudioDrive) {
					Console::Error("No disc selected.\n");
					break;
				}
			}

			if (newAudioDrive != audioDrive) {
				copier.Close();
				if (!copier.Open(newAudioDrive)) {
					Console::Error("Failed to open drive\n");
					if (!copier.Open(audioDrive)) {
						Console::Error("Failed to reopen original drive\n");
						return 1;
					}
					break;
				}
				audioDrive = newAudioDrive;
				std::cout << "\nSwitched to drive ";
				Console::SetColor(Console::Color::Yellow);
				std::cout << static_cast<char>(audioDrive) << ":";
				Console::Reset();
				std::cout << "\n";
			}

			Console::Info("Rescanning disc...\n");
			disc = DiscInfo{};
			hasTOC = copier.ReadTOC(disc);
			bool didTOCScan = false;

			if (!hasTOC) {
				Console::Warning("No TOC found.\n");
				Console::Info("Attempting TOC-less disc scan from LBA 0...\n");
				hasTOC = copier.ScanDiscWithoutTOC(disc);
				didTOCScan = hasTOC;
				if (!hasTOC) {
					Console::Warning("Disc scan failed. Disc-dependent features will be unavailable.\n");
					break;
				}
			}

			// Bad TOC — rescan ONLY if ReadTOC was the source
			// AND the LBAs could not be recovered from Full TOC data.
			if (hasTOC && disc.tocRepaired && !disc.tocLBAsRecovered && !didTOCScan) {
				Console::Warning("TOC had out-of-range entries that were clamped.\n");
				Console::Info("Re-scanning disc without TOC for accurate boundaries...\n");
				DiscInfo rescanned;
				if (copier.ScanDiscWithoutTOC(rescanned)) {
					disc = rescanned;
				}
				else {
					Console::Warning("TOC-less scan failed - using clamped TOC instead.\n");
				}
			}

			if (hasTOC) {
				copier.ReadCDText(disc);
				copier.ReadISRC(disc);
				copier.ReadMCN(disc);
				std::vector<std::vector<uint32_t>> pressingCRCs;
				AccurateRip::Lookup(disc, pressingCRCs);
				PrintDiscInfo(disc);
				Console::Success("Disc rescan complete.\n");
			}
			break;
		}

			   // ── 26. Check for updates ─────────────────────────────────
		case 26: {
			CheckForUpdates(APP_VERSION);
			break;
		}

			   // ── 27. Help ──────────────────────────────────────────────
		case 27:
			PrintHelpMenu();
			break;

			// ── 28. Pioneer CD Check ────────────────────────────────
		case 28:
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			RunPioneerCdCheck(copier, disc);
			break;

			// ── 29. Jitter / beta scan (LiteOn) ─────────────────────
		case 29: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			JitterResult jr;
			if (copier.RunJitterScan(disc, jr, speed)) {
				std::wstring logPath = workDir + L"\\jitter_scan.csv";
				if (copier.SaveJitterLog(jr, logPath)) {
					Console::Success("Jitter log saved to: ");
					std::wcout << logPath << L"\n";
				}
			}
			else if (!jr.supported) {
				Console::Warning("Jitter scan requires legacy LiteOn 0xDF/0x1B jitter support.\n");
			}
			else {
				Console::Error("Jitter scan failed.\n");
			}
			break;
		}

			// ── 32. FE/TE servo scan (LiteOn, experimental) ─────────
		case 32: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			FeTeResult fr;
			if (copier.RunFeTeScan(disc, fr, speed)) {
				std::wstring logPath = workDir + L"\\fete_scan.csv";
				if (copier.SaveFeTeLog(fr, logPath)) {
					Console::Success("FE/TE log saved to: ");
					std::wcout << logPath << L"\n";
				}
			}
			else if (!fr.supported) {
				Console::Warning("FE/TE scan requires LiteOn/MediaTek 0xDF/0x08 servo support.\n");
			}
			else {
				Console::Error("FE/TE scan failed.\n");
			}
			break;
		}

			// ── 30. Recovery rip (drive-independent) ────────────────
			// Stable op id 30; surfaced as GUI button 5 (Ripping section) via
			// ButtonToMenuChoice. Rebuilds hard sectors from cross-read
			// consensus instead of trusting the drive's C2.
		case 30:
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			RunRecoveryRipWorkflow(copier, disc, workDir);
			break;

			// ── 31. Erase CD-RW (rewritable) ────────────────────────
			// Erases a rewritable disc, with a Quick/Full choice. A disc that already
			// reports blank is skipped (nothing to do). Crucially, when the disc type
			// can't be read at all -- a botched/unreadable burn -- it offers to force a
			// blank anyway: BLANK does not need to read the disc, so a full erase can
			// often recover a CD-RW, and on a write-once CD-R it just fails harmlessly.
			// No TOC pre-scan: contents are about to be wiped. Surfaced as GUI button
			// 31; dispatcher op id 31 (mapped from button index 30 by ButtonToMenuChoice).
		case 31: {
			Console::Heading("\n=== Erase CD-RW ===\n");

			// Erase is destructive and, unlike every other write path, used to run
			// against whatever drive happened to be open — the audio-source drive
			// after any earlier scan, or an arbitrary one on a fresh launch. Pick
			// the target the way Copy disc / Write tracks do, defaulting to the
			// drive already open.
			{
				std::vector<wchar_t> eraseAudioDrives;
				// Quiet scan — SelectWriterDrive prints the numbered list itself.
				std::vector<wchar_t> eraseCdDrives =
					ScanDrives(eraseAudioDrives, /*verbose=*/false);
				if (eraseCdDrives.empty()) {
					Console::Error("No CD/DVD drives detected.\n");
					break;
				}

				wchar_t pick = eraseCdDrives.front();
				if (eraseCdDrives.size() > 1) {
					Console::Info("\nPick the drive holding the disc to erase.\n");
					pick = SelectWriterDrive(eraseCdDrives, audioDrive);
					if (!pick) { Console::Info("Erase cancelled.\n"); break; }
				}

				if (pick != audioDrive) {
					copier.Close();
					if (!copier.Open(pick)) {
						Console::Error("Failed to open the selected drive.\n");
						copier.Open(audioDrive);   // leave the session usable
						break;
					}
					audioDrive = pick;
					// The cached TOC describes the disc in the drive we just left.
					disc = DiscInfo{};
					hasTOC = false;
				}

				std::string target = "Erasing in drive ";
				target += static_cast<char>(audioDrive);
				target += ":\n";
				Console::Info(target.c_str());
			}

			bool isFull = false, isRewritable = false, isBlank = false;
			bool detected = copier.CheckRewritableDisk(isFull, isRewritable, false, &isBlank);

			bool forceUnreadable = false;
			if (!detected) {
				// Bad / unreadable disc -- the type probe failed. A mis-burned CD-RW can
				// land here. Offer to blank it anyway; a full erase can often recover it.
				Console::Warning("Could not read the disc type - it may be a bad/unreadable copy.\n");
				if (!GuiInput::PromptYesNo("Force erase unreadable disc?",
					"The disc type couldn't be read (e.g. a bad burn).\n\n"
					"Attempt to blank it anyway?  This recovers a CD-RW; on a write-once\n"
					"CD-R it will simply fail without doing any harm.")) {
					Console::Info("Erase cancelled.\n");
					break;
				}
				forceUnreadable = true;
			}
			else if (!isRewritable) {
				Console::Error("The inserted disc is not rewritable - it cannot be erased.\n");
				Console::Info("Only CD-RW media can be blanked. CD-R is write-once.\n");
				break;
			}
			else if (isBlank) {
				Console::Success("CD-RW is already blank - nothing to erase.\n");
				break;
			}

			// Quick vs full. Quick clears the TOC/PMA (fast, makes the disc writable
			// again); full erases the entire recorded surface (slow, and the more
			// reliable choice for recovering a bad/unreadable disc).
			int mode = GuiInput::PromptYesNoCancel("Erase CD-RW",
				"Quick erase is fast and makes the disc writable again.\n"
				"Full erase wipes the entire surface and takes much longer\n"
				"(more reliable for recovering a bad/unreadable disc).\n\n"
				"Yes = Quick erase    No = Full erase    Cancel = abort");
			if (mode == -1) { Console::Info("Erase cancelled.\n"); break; }
			bool quickBlank = (mode == 1);

			int speed = copier.SelectWriteSpeed();
			if (speed == -1) { Console::Info("Erase cancelled.\n"); break; }

			if (forceUnreadable)
				Console::Info("Forcing blank on an unreadable disc...\n");

			// skipConfirm=true: the Quick/Full choice above (and the force prompt for
			// an unreadable disc) already confirm this destructive action.
			copier.BlankRewritableDisk(speed, quickBlank, /*skipConfirm=*/true);

			// The disc's contents are gone, so any cached TOC for it is now a
			// lie. Drop it rather than let a later operation act on a layout
			// that no longer exists on the medium.
			disc = DiscInfo{};
			hasTOC = false;
			break;
		}

		default:
			Console::Warning("Unknown option.\n");
			break;
		}
	}

	return 0;
}
