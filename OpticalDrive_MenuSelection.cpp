#define NOMINMAX
#include "OpticalDrive.h"
#include "AccurateRip.h"
#include "GuiInput.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstdlib>


// ============================================================================
// Menu Selection Methods
// ============================================================================

int OpticalDrive::SelectSpeed() {
	// Speed is no longer user-selectable. Many drives ignore the requested
	// multiplier and clamp to their own floor, so OptiScan now always uses the
	// lowest speed the drive will actually honor — best for accuracy on damaged
	// or marginal discs. Probe it, apply it, and report what we got.
	int lowest = m_drive.GetLowestHonoredSpeed(/*apply=*/true);
	std::cout << "\n=== Speed ===\n";
	std::cout << "  Using lowest speed the drive honors: " << lowest << "x\n";
	return lowest;
}

int OpticalDrive::SelectSubchannel() {
	std::cout << "\n=== Subchannel Data ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Include subchannel (for accurate rip, creates .sub file)\n";
	std::cout << "2. Audio only (smaller output, no .sub file)\n";
	std::cout << "Choice: ";
	int c = GetMenuChoice("Subchannel Data",
		"0. Back to menu\n"
		"1. Include subchannel (for accurate rip, creates .sub file)\n"
		"2. Audio only (smaller output, no .sub file)",
		0, 2, 1);
	if (c == 0) return -1;
	bool include = (c != 2);
	std::cout << (include ? "Including subchannel data\n" : "Audio only mode\n");
	return include ? 1 : 0;
}

int OpticalDrive::SelectErrorHandling() {
	std::cout << "\n=== Error Handling ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Abort on error (safest)\n";
	std::cout << "2. Skip bad sectors (fill with silence)\n";
	std::cout << "3. Skip bad sectors (keep reading)\nChoice: ";
	int c = GetMenuChoice("Error Handling",
		"0. Back to menu\n"
		"1. Abort on error (safest)\n"
		"2. Skip bad sectors (fill with silence)\n"
		"3. Skip bad sectors (keep reading)",
		0, 3, 1);
	if (c == 0) return -1;
	if (c == 2) { std::cout << "Will fill bad sectors with silence\n"; return 2; }
	if (c == 3) { std::cout << "Will skip bad sectors\n"; return 3; }
	std::cout << "Will abort on error\n";
	return 1;
}

LogOutput OpticalDrive::SelectLogging() {
	std::cout << "\n=== Logging Output ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Log to file\n";
	std::cout << "2. Log to console\n";
	std::cout << "3. Log to both\n";
	std::cout << "4. No logging\n";
	std::cout << "Choice: ";
	int c = GetMenuChoice("Logging Output",
		"0. Back to menu\n"
		"1. Log to file\n"
		"2. Log to console\n"
		"3. Log to both\n"
		"4. No logging",
		0, 4, 1);
	if (c == 0) return LogOutput::None;  // Sentinel for "back"
	switch (c) {
	case 1: return LogOutput::File;
	case 2: return LogOutput::Console;
	case 3: return LogOutput::Both;
	default: return LogOutput::None;
	}
}

int OpticalDrive::SelectC2Detection() {
	std::cout << "\n=== C2 Error Detection ===\n";
	std::cout << "C2 errors indicate uncorrectable read errors (data corruption).\n";
	std::cout << "Checking drive C2 support... ";
	if (!m_drive.CheckC2Support()) {
		std::cout << "NOT SUPPORTED\n";
		return 0;
	}
	std::cout << "OK\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Enable C2 error detection (recommended)\n";
	std::cout << "2. Disable C2 detection (faster)\nChoice: ";
	int c = GetMenuChoice("C2 Error Detection",
		"0. Back to menu\n"
		"1. Enable C2 error detection (recommended)\n"
		"2. Disable C2 detection (faster)",
		0, 2, 1);
	if (c == 0) return -1;
	bool enable = (c == 1);
	std::cout << (enable ? "C2 error detection enabled\n" : "C2 detection disabled\n");
	return enable ? 1 : 0;
}

int OpticalDrive::SelectOffset() {
	std::cout << "\n=== Drive Offset ===\n0. Back to menu\n1. Auto-detect\n2. Enter manually\n3. No correction\nChoice: ";
	int c = GetMenuChoice("Drive Offset",
		"0. Back to menu\n"
		"1. Auto-detect\n"
		"2. Enter manually\n"
		"3. No correction",
		0, 3, 1);
	if (c == 0) return -1;  // ✓ Changed from MENU_BACK to -1 for consistency
	if (c == 1) {
		int off = DetectDriveOffset();
		std::cout << "Using offset: " << off << "\n";
		return off;
	}
	if (c == 2) {
		return GuiInput::PromptInt("Drive offset",
			"Enter drive offset in samples (positive or negative):",
			-10000, 10000, 0);
	}
	return 0;
}

int OpticalDrive::SelectScanSpeed() {
	// Scan speed is no longer user-selectable — always use the lowest speed the
	// drive will honor, which is the most accurate for damaged discs. Drives
	// commonly ignore higher-precision requests anyway, so there is nothing to
	// choose between.
	int lowest = m_drive.GetLowestHonoredSpeed(/*apply=*/false);
	std::cout << "\n=== Scan Speed ===\n";
	std::cout << "  Using lowest speed the drive honors: " << lowest << "x\n";
	return lowest;
}

int OpticalDrive::SelectSecureRipMode(int selectedSpeed) {
	std::string speedLabel = (selectedSpeed == 0) ? "maximum speed" : (std::to_string(selectedSpeed) + "x");
	std::cout << "\n=== Rip Mode ===\n";
	std::cout << "Choose verification level for accuracy vs speed.\n\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Standard (single pass with retry on error)\n";
	std::cout << "2. Secure - Fast (2 passes, light verification)\n";
	std::cout << "3. Secure - Standard (3-6 passes, balanced accuracy)\n";
	std::cout << "4. Secure - Paranoid (4-8 passes, maximum accuracy)\n";
	std::cout << "5. Burst (" << speedLabel << ", no verification)\n";

	std::string ripModeMsg =
		"Choose verification level for accuracy vs speed.\n\n"
		"0. Back to menu\n"
		"1. Standard (single pass with retry on error)\n"
		"2. Secure - Fast (2 passes, light verification)\n"
		"3. Secure - Standard (3-6 passes, balanced accuracy)\n"
		"4. Secure - Paranoid (4-8 passes, maximum accuracy)\n"
		"5. Burst (" + speedLabel + ", no verification)";
	int c = GetMenuChoice("Rip Mode", ripModeMsg.c_str(), 0, 5, 1);

	switch (c) {
	case 0: return -1;
	case 1: std::cout << "Standard mode - Single pass\n"; return 0;
	case 2: std::cout << "Secure Fast mode - Light verification\n"; return 1;
	case 3: std::cout << "Secure Standard mode - Balanced accuracy\n"; return 2;
	case 4: std::cout << "Secure Paranoid mode - Maximum accuracy\n"; return 3;
	case 5: std::cout << "Burst mode - " << speedLabel << "\n"; return -2;
	default: return 0;
	}
}

int OpticalDrive::SelectPregapMode() {
	std::cout << "\n=== Pre-gap Extraction ===\n";
	std::cout << "Pre-gaps are the silent/hidden sections before each track's INDEX 01.\n";
	std::cout << "Track 1 may contain hidden audio (HTOA) before INDEX 01.\n\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Include pre-gaps in image (recommended for accurate backup)\n";
	std::cout << "2. Skip pre-gaps (audio starts at INDEX 01)\n";
	std::cout << "3. Extract pre-gaps as separate files\n";
	std::cout << "Choice: ";

	int c = GetMenuChoice("Pre-gap Extraction",
		"0. Back to menu\n"
		"1. Include pre-gaps in image (recommended for accurate backup)\n"
		"2. Skip pre-gaps (audio starts at INDEX 01)\n"
		"3. Extract pre-gaps as separate files",
		0, 3, 1);

	if (c == 0) return -1;

	const char* modes[] = { "", "Including pre-gaps in image", "Skipping pre-gaps", "Extracting pre-gaps separately" };
	std::cout << modes[c] << "\n";

	return c - 1;
}

int OpticalDrive::SelectCacheDefeat() {
	std::cout << "\n=== Drive Cache Defeat ===\n";
	std::cout << "CD drives cache recently read sectors in memory. When re-reading\n";
	std::cout << "the same sector, the drive may return cached data instead of\n";
	std::cout << "actually reading from the disc again.\n\n";
	std::cout << "Cache defeat forces a seek to a distant location between reads,\n";
	std::cout << "ensuring each read comes from the actual disc surface.\n\n";
	std::cout << "  - ENABLED:  More accurate, detects intermittent read errors\n";
	std::cout << "              Slower due to extra seeks (~2-3x slower)\n";
	std::cout << "  - DISABLED: Faster, but may miss marginal/unstable sectors\n\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Enable cache defeat (recommended for damaged discs)\n";
	std::cout << "2. Disable cache defeat (faster)\n";
	std::cout << "Choice: ";

	int c = GetMenuChoice("Drive Cache Defeat",
		"0. Back to menu\n"
		"1. Enable cache defeat (recommended for damaged discs)\n"
		"2. Disable cache defeat (faster)",
		0, 2, 2);

	if (c == 0) return -1;
	bool enable = (c == 1);
	std::cout << (enable ? "Cache defeat ENABLED - seeks between reads\n"
		: "Cache defeat DISABLED - maximum speed\n");
	return enable ? 1 : 0;
}

int OpticalDrive::SelectHideCDRMedia() {
	if (!m_drive.SupportsHideCDRMedia()) {
		Console::Info("Hide CDR Media: not supported on this drive (Plextor only) - skipping.\n");
		return 0;
	}

	std::cout << "\n=== Hide CDR Media ===\n";
	std::cout << "Plextor vendor feature: makes the drive treat inserted CD-R /\n";
	std::cout << "CD-RW discs as pressed CD on disc-info queries.\n\n";
	std::cout << "  - ENABLED:  Helps when re-ripping CD-R backups whose source\n";
	std::cout << "              protection alters drive behaviour for CDR media.\n";
	std::cout << "  - DISABLED: Normal media reporting (recommended for\n";
	std::cout << "              pressed audio CDs).\n\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Enable Hide CDR Media\n";
	std::cout << "2. Disable Hide CDR Media (default)\n";
	std::cout << "Choice: ";

	int c = GetMenuChoice("Hide CDR Media",
		"0. Back to menu\n"
		"1. Enable Hide CDR Media\n"
		"2. Disable Hide CDR Media (default)",
		0, 2, 2);

	if (c == 0) return -1;
	bool enable = (c == 1);
	std::cout << (enable ? "Hide CDR Media ENABLED\n"
		: "Hide CDR Media DISABLED\n");
	return enable ? 1 : 0;
}

int OpticalDrive::SelectSilentMode() {
	if (!m_drive.IsPlextor()) {
		// No prompt on non-Plextor drives — silently skip.
		return 0;
	}

	std::cout << "\n=== Silent Mode (Plextor) ===\n";
	std::cout << "Caps spin-up speed and softens seek motion to reduce drive noise.\n";
	std::cout << "Useful for long unattended secure / paranoid rip sessions.\n\n";
	std::cout << "  - ENABLED:  Quieter rip, may slightly increase total time.\n";
	std::cout << "  - DISABLED: Normal performance (default).\n\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Enable Silent Mode\n";
	std::cout << "2. Disable Silent Mode (default)\n";
	std::cout << "Choice: ";

	int c = GetMenuChoice("Silent Mode (Plextor)",
		"0. Back to menu\n"
		"1. Enable Silent Mode\n"
		"2. Disable Silent Mode (default)",
		0, 2, 2);

	if (c == 0) return -1;
	bool enable = (c == 1);
	std::cout << (enable ? "Silent Mode ENABLED\n" : "Silent Mode DISABLED\n");
	return enable ? 1 : 0;
}

int OpticalDrive::SelectPlextorWriteOptions(bool& outTestWrite,
	bool& outVariRecEnable, int& outVariRecOffset) {
	outTestWrite = false;
	outVariRecEnable = false;
	outVariRecOffset = 0;

	if (!m_drive.IsPlextor()) return 0;

	std::cout << "\n=== Plextor Write Options ===\n";

	// ── Test write ──
	std::cout << "Test write?  Simulates the burn end-to-end without consuming\n";
	std::cout << "the blank (laser stays at read power). Recommended for the\n";
	std::cout << "first burn from a new .cue/.bin set.\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Yes, test write only (no actual burn)\n";
	std::cout << "2. No, perform a real burn (default)\n";
	std::cout << "Choice: ";
	int t = GetMenuChoice("Plextor Test Write",
		"Simulates the burn end-to-end without consuming the blank\n"
		"(laser stays at read power).\n\n"
		"0. Back to menu\n"
		"1. Yes, test write only (no actual burn)\n"
		"2. No, perform a real burn (default)",
		0, 2, 2);
	if (t == 0) return -1;
	outTestWrite = (t == 1);

	// ── VariRec ──
	std::cout << "\nVariRec laser-power tuning?\n";
	std::cout << "Nudges the firmware's default write power for the inserted CD-R.\n";
	std::cout << "Use only when the stock strategy mis-burns borderline media.\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Enable VariRec\n";
	std::cout << "2. Skip (factory strategy, default)\n";
	std::cout << "Choice: ";
	int v = GetMenuChoice("Plextor VariRec",
		"Nudges the firmware's default write power for the inserted CD-R.\n"
		"Use only when the stock strategy mis-burns borderline media.\n\n"
		"0. Back to menu\n"
		"1. Enable VariRec\n"
		"2. Skip (factory strategy, default)",
		0, 2, 2);
	if (v == 0) return -1;

	if (v == 1) {
		outVariRecEnable = true;
		outVariRecOffset = GuiInput::PromptInt("VariRec offset",
			"Laser-power offset (-4 to +4, 0 = no change):",
			-4, 4, 0);
		std::cout << "VariRec ENABLED with offset " << outVariRecOffset << "\n";
	}

	return 1;
}

SecureRipConfig OpticalDrive::GetSecureRipConfig(SecureRipMode mode) {
	SecureRipConfig config;
	config.mode = mode;

	switch (mode) {
	case SecureRipMode::Disabled:
		config.minPasses = 1;
		config.maxPasses = 1;
		config.requiredMatches = 1;
		config.useC2 = true;
		config.c2Guided = true;
		config.cacheDefeat = false;
		config.maxSpeed = 0;
		break;
	case SecureRipMode::Fast:
		config.minPasses = 2;
		config.maxPasses = 3;
		config.requiredMatches = 2;
		config.useC2 = true;
		config.c2Guided = true;
		config.cacheDefeat = true;
		config.maxSpeed = 4;
		break;
	case SecureRipMode::Standard:
		config.minPasses = 3;
		config.maxPasses = 6;
		config.requiredMatches = 2;
		config.useC2 = true;
		config.c2Guided = true;
		config.cacheDefeat = true;
		config.maxSpeed = 4;
		break;
	case SecureRipMode::Paranoid:
		config.minPasses = 4;
		config.maxPasses = 8;
		config.requiredMatches = 3;
		config.useC2 = true;
		config.c2Guided = false;
		config.cacheDefeat = true;
		config.maxSpeed = 2;
		break;
	}

	// EXPERIMENTAL opt-in: OPTISCAN_DRIVE_READ_RETRY=<n> caps the drive's internal
	// Read Error Recovery retry count (mode page 0x01) during the secure rip so the
	// host's own multi-pass/consensus engine controls recovery. Off unless set.
	// Only meaningful for the secure modes (they use ReadDiscSecure).
	if (mode == SecureRipMode::Fast || mode == SecureRipMode::Standard ||
		mode == SecureRipMode::Paranoid) {
		char buf[16] = {};
		size_t len = 0;
		if (getenv_s(&len, buf, sizeof(buf), "OPTISCAN_DRIVE_READ_RETRY") == 0
			&& len > 1) {
			int v = std::atoi(buf);
			if (v >= 0 && v <= 255) {
				config.driveReadRetryOverride = v;
				std::cout << "  [experimental] OPTISCAN_DRIVE_READ_RETRY=" << v
					<< " -> will cap the drive's internal read-retry count for this rip.\n";
			}
			else {
				std::cout << "  [experimental] Ignoring OPTISCAN_DRIVE_READ_RETRY='"
					<< buf << "' (expected an integer 0-255).\n";
			}
		}
	}

	return config;
}

// ============================================================================
// Offset Detection
// ============================================================================

int OpticalDrive::DetectDriveOffset() {
	std::string vendor, model;
	if (m_drive.GetDriveInfo(vendor, model)) {
		std::cout << "Drive: " << vendor << " " << model << "\n";
	}

	// Try the enhanced detection method first
	OffsetDetectionResult result;
	if (m_drive.DetectDriveOffset(result)) {
		std::cout << "Offset detected: " << result.offset << " samples\n";
		std::cout << "Confidence: " << result.confidence << "%\n";
		std::cout << "Method: " << result.details << "\n";
		return result.offset;
	}

	std::cout << "Could not auto-detect offset from disc.\n";
	std::cout << "Recommendation: Use a known test disc or enter offset manually.\n";
	return 0;
}

bool OpticalDrive::DetectDriveOffset(OffsetDetectionResult& result) {
	return m_drive.DetectDriveOffset(result);
}

int OpticalDrive::SelectWriteSpeed() {
	// Write speed is no longer user-selectable. Lower speeds produce more
	// reliable burns, and drives clamp the request to a supported step anyway,
	// so OptiScan always requests the lowest (1x) and lets the burner settle on
	// the lowest write speed it actually supports for the loaded blank.
	std::cout << "\n=== Write Speed ===\n";
	std::cout << "  Requesting 1x; the burner may select a higher supported minimum.\n";
	std::cout << "  OptiScan will read back and report the selected speed before writing.\n";
	return 1;
}
