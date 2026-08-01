#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleColors.h"
#include "Drive.h"
#include "GuiInput.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <windows.h>

// ============================================================================
// Helper: Wait for drive to become ready (poll TEST UNIT READY)
// ============================================================================
static bool WaitForDriveReady(ScsiDrive& drive, int timeoutSeconds) {
	for (int i = 0; i < timeoutSeconds * 4; i++) {
		BYTE testCmd[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		BYTE sk = 0, asc = 0, ascq = 0;
		if (drive.SendSCSIWithSense(testCmd, sizeof(testCmd), nullptr, 0,
			&sk, &asc, &ascq, true)) {
			return true;
		}
		if (sk == 0x02 && asc == 0x04) {
			Sleep(250);
			continue;
		}
		return false;
	}
	return false;
}

// A successful TUR is not sufficient to prove that an asynchronous BLANK has
// finished: some drives remain command-ready while blanking in the background.
// READ DISC INFORMATION status 0 is the completion condition we need.
static bool DiscInformationReportsBlank(ScsiDrive& drive) {
	BYTE cmd[10] = { 0x51, 0, 0, 0, 0, 0, 0, 0, 0x22, 0 };
	BYTE response[34] = {};
	if (!drive.SendSCSI(cmd, sizeof(cmd), response, sizeof(response), true))
		return false;
	return (response[2] & 0x03) == 0x00;
}

// ============================================================================
// CheckRewritableDisk - Detect rewritable disc and capacity
// ============================================================================
bool OpticalDrive::CheckRewritableDisk(bool& isFull, bool& isRewritable, bool quiet,
	bool* outIsBlank) {
	isFull = false;
	isRewritable = false;
	if (outIsBlank) *outIsBlank = false;

	if (!quiet) Console::Info("Querying disc media type...\n");

	BYTE cmd[10] = { 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0x00 };
	BYTE response[252] = { 0 };
	BYTE senseKey = 0, asc = 0, ascq = 0;

	if (!m_drive.SendSCSIWithSense(cmd, sizeof(cmd), response, sizeof(response),
		&senseKey, &asc, &ascq, true)) {

		Console::Warning("Disc information query failed");
		std::string senseDesc = m_drive.GetSenseDescription(senseKey, asc, ascq);
		if (!senseDesc.empty()) {
			Console::Info(" (");
			std::cout << senseDesc << ")\n";
		}
		else {
			std::cout << "\n";
		}

		Console::Warning("Attempting fallback disc detection...\n");

		BYTE profileCmd[10] = { 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00 };
		BYTE profileResponse[8] = { 0 };

		if (m_drive.SendSCSI(profileCmd, sizeof(profileCmd), profileResponse, sizeof(profileResponse), true)) {
			WORD profile = (static_cast<WORD>(profileResponse[6]) << 8) | profileResponse[7];
			isRewritable = (profile == 0x0A);

			// Profile decode also sets the result flags, so run the switch even
			// when quiet; only the printed line is gated.
			if (!quiet) Console::Success("Media type detected: ");
			switch (profile) {
			case 0x08:
				if (!quiet) std::cout << "CD-ROM\n";
				isFull = true;
				return true;
			case 0x09: if (!quiet) std::cout << "CD-R (write-once)\n"; isRewritable = false; break;
			case 0x0A: if (!quiet) std::cout << "CD-RW (rewritable)\n"; isRewritable = true; break;
			default:
				if (!quiet) std::cout << "Unknown (0x" << std::hex << profile << std::dec << ")\n";
				return false;
			}
			// GET CONFIGURATION identifies the medium family, not whether a
			// writable disc is blank, appendable, or complete. Do not invent a
			// fullness result when READ DISC INFORMATION failed.
			Console::Warning("Writable media detected, but its blank/full status is unknown.\n");
			return false;
		}

		Console::Warning("Could not determine disc type\n");
		return false;
	}

	BYTE discStatus = response[2] & 0x03;
	isFull = (discStatus == 0x02);
	isRewritable = (response[2] & 0x10) != 0;
	if (outIsBlank) *outIsBlank = (discStatus == 0x00);  // 0x00 = Empty (blank)

	if (!quiet) {
		Console::Success("Disc media type: ");
		std::cout << (isRewritable ? "CD-RW (rewritable)\n" : "CD-R (write-once)\n");

		Console::Success("Disc status: ");
		switch (discStatus) {
		case 0x00: std::cout << "Empty\n"; break;
		case 0x01: std::cout << "Appendable\n"; break;
		case 0x02: std::cout << "Complete (full)\n"; break;
		default: std::cout << "Unknown\n"; break;
		}
	}

	return true;
}

// ============================================================================
// BlankRewritableDisk - Erase rewritable media (quick or full)
// ============================================================================
bool OpticalDrive::BlankRewritableDisk(int speed, bool quickBlank, bool skipConfirm) {
	Console::Warning(quickBlank ? "\nQuick blanking rewritable disc...\n"
		: "\nFull blanking rewritable disc...\n");
	Console::Info("[!] This operation will erase all data on the disc!\n");
	if (!skipConfirm && !GuiInput::PromptYesNo("Confirm blank",
		"This operation will erase all data on the disc. Proceed?")) {
		Console::Info("Blank operation cancelled\n");
		return false;
	}

	m_drive.SetSpeed(speed);

	BYTE standardType = (quickBlank ? 0x01 : 0x00) | 0x10;
	BYTE cmd[12] = { 0xA1, standardType, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	bool blankStarted = false;
	const char* usedMethod = quickBlank ? "quick blank" : "full blank";

	if (m_drive.SendSCSI(cmd, sizeof(cmd), nullptr, 0, false)) {
		blankStarted = true;
	}
	else {
		Console::Warning("Standard blank failed - trying erase session recovery...\n");

		BYTE recoveryCmd[12] = { 0xA1, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

		if (m_drive.SendSCSI(recoveryCmd, sizeof(recoveryCmd), nullptr, 0, false)) {
			Console::Info("Erase session in progress...\n");
			WaitForDriveReady(m_drive, 120);

			Console::Info("Retrying ");
			std::cout << usedMethod << "...\n";
			if (m_drive.SendSCSI(cmd, sizeof(cmd), nullptr, 0, false)) {
				blankStarted = true;
			}
			else {
				Console::Error("Retry after erase session also failed\n");
			}
		}
		else {
			Console::Warning("Erase session not supported by drive\n");
		}
	}

	if (!blankStarted) {
		Console::Error("Blank command failed\n");
		return false;
	}

	Console::Info("Blanking method: ");
	std::cout << usedMethod << "\n";

	int maxWait = quickBlank ? 120 : 600;
	int barWidth = 35;
	std::string label = quickBlank ? "Quick blank" : "Full blank";
	int lastPct = -1;
	int lastLineLen = 0;
	auto startTime = std::chrono::steady_clock::now();

	bool blankCompleted = false;
	for (int i = 0; i < maxWait; i++) {
		Sleep(1000);

		if (InterruptHandler::Instance().IsInterrupted()) {
			std::cout << "\n";
			Console::Error("Blank operation cancelled\n");
			return false;
		}

		BYTE sk = 0, asc = 0, ascq = 0;
		int drivePct = -1;
		m_drive.RequestSenseProgress(sk, asc, ascq, drivePct);

		// TUR alone cannot prove completion: some drives remain ready during a
		// background BLANK. Confirm the medium itself reports blank.
		BYTE testCmd[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		BYTE tsk = 0, tasc = 0, tascq = 0;
		const bool turReady = m_drive.SendSCSIWithSense(
			testCmd, sizeof(testCmd), nullptr, 0, &tsk, &tasc, &tascq, true);
		if (turReady && DiscInformationReportsBlank(m_drive))
			drivePct = 100;

		int pct;
		if (drivePct >= 0) {
			pct = std::min(drivePct, 100);
		}
		else {
			pct = std::min((i + 1) * 100 / maxWait, 99);
		}

		// Never allow progress to go backward
		if (lastPct >= 0 && pct < lastPct) {
			pct = lastPct;
		}

		if (pct != lastPct) {
			lastPct = pct;
			std::ostringstream ss;
			ss << "\r" << label << " [";
			int filled = pct * barWidth / 100;
			for (int j = 0; j < barWidth; j++) {
				ss << (j < filled ? "\xe2\x96\x88" : "\xe2\x96\x91");
			}
			ss << "] " << std::setw(3) << pct << "%";

			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now() - startTime).count();
			if (elapsed >= 60)
				ss << " " << elapsed / 60 << "m " << std::setfill('0') << std::setw(2) << elapsed % 60 << "s";
			else
				ss << " " << elapsed << "s";

			std::string line = ss.str();
			if (static_cast<int>(line.size()) < lastLineLen)
				line.append(lastLineLen - line.size(), ' ');
			std::cout << line << std::flush;
			lastLineLen = static_cast<int>(line.size());
		}

		if (drivePct >= 100) {
			blankCompleted = true;
			break;
		}
	}

	auto totalSec = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - startTime).count();
	std::cout << "\n";

	if (!blankCompleted) {
		Console::Error("Blank operation timed out - disc may not be fully erased.\n");
		return false;
	}

	std::cout << "  Done";
	if (totalSec > 0) {
		if (totalSec >= 60)
			std::cout << " in " << totalSec / 60 << "m "
			<< std::setfill('0') << std::setw(2) << totalSec % 60 << "s" << std::setfill(' ');
		else
			std::cout << " in " << totalSec << "s";
	}
	std::cout << "\n";

	Console::Success("Disc blanked successfully\n");
	return true;
}

// ============================================================================
// PerformPowerCalibration - Calibrate laser power for writing
// ============================================================================
bool OpticalDrive::PerformPowerCalibration() {
	Console::Info("Performing power calibration (OPC)...\n");

	// SEND OPC INFORMATION (0x54). Try IMMED first so the drive returns as soon
	// as the calibration has started; fall back to non-IMMED if the drive rejects
	// the IMMED bit (some older writers refuse it).
	BYTE sk = 0, asc = 0, ascq = 0;
	auto trySend = [&](BYTE immed) {
		BYTE cmd[10] = { 0x54, immed, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		sk = asc = ascq = 0;
		return m_drive.SendSCSIWithSense(cmd, sizeof(cmd), nullptr, 0, &sk, &asc, &ascq, false);
	};

	bool ok = trySend(0x01);
	if (!ok && sk == 0x05 /* ILLEGAL REQUEST */) {
		ok = trySend(0x00);  // retry without IMMED
	}

	if (!ok) {
		char msg[160];
		std::snprintf(msg, sizeof(msg),
			"Power calibration rejected by drive (sense %02X/%02X/%02X) - continuing without it\n",
			sk, asc, ascq);
		Console::Warning(msg);
		return true;
	}

	Console::Info("Waiting for power calibration to complete");
	bool ready = false;
	for (int i = 0; i < 60; i++) {
		if (InterruptHandler::Instance().IsInterrupted()) {
			Console::Error("\nPower calibration cancelled\n");
			return false;
		}
		if (m_drive.TestUnitReady()) {
			ready = true;
			break;
		}
		std::cout << "." << std::flush;
		Sleep(1000);
	}
	std::cout << "\n";
	if (!ready) {
		Console::Error("Power calibration did not complete within 60 seconds.\n");
		return false;
	}

	Console::Success("Power calibration complete\n");
	return true;
}

void BlankDiscStandalone(int speed, int eraseType) {
	OpticalDrive copier;

	Console::Info("Standalone disc blanking tool\n");

	// Discover CD/DVD drives and pick one
	std::vector<wchar_t> audioDrives;
	std::vector<wchar_t> cdDrives = ScanDrives(audioDrives);

	if (cdDrives.empty()) {
		Console::Error("No CD/DVD drives found\n");
		return;
	}

	wchar_t driveLetter = cdDrives[0];
	if (cdDrives.size() > 1) {
		Console::Info("Select drive:\n");
		std::string msg;  // plain-text mirror of the list for the modal box
		for (size_t i = 0; i < cdDrives.size(); i++) {
			std::cout << "  " << (i + 1) << ". " << static_cast<char>(cdDrives[i]) << ":\n";
			msg += std::to_string(i + 1) + ". ";
			msg += static_cast<char>(cdDrives[i]);
			msg += ":\n";
		}
		if (!msg.empty() && msg.back() == '\n') msg.pop_back();
		std::cout << "Choice: ";
		bool ok = false;
		int pick = GetMenuChoice("Select Drive to Blank", msg.c_str(),
			1, static_cast<int>(cdDrives.size()), 1, &ok);
		if (!ok) { Console::Info("Cancelled.\n"); return; }
		driveLetter = cdDrives[pick - 1];
	}

	if (!copier.Open(driveLetter)) {
		Console::Error("Failed to open drive\n");
		return;
	}
	PrintDriveIdentity(driveLetter);

	if (eraseType == 0) {
		Console::Error("Invalid erase type specified\n");
		return;
	}

	copier.BlankRewritableDisk(speed, eraseType == 1);
}
