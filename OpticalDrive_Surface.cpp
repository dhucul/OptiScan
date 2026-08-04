#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>

// ============================================================================
// Surface Mapping & Lead Area Checks
// ============================================================================

bool OpticalDrive::CheckLeadAreas(DiscInfo& disc, int scanSpeed) {
	std::cout << "\n=== Lead-in/Lead-out Area Check ===\n";
	if (!m_drive.CheckC2Support()) return false;

	// Validate disc structure
	if (disc.leadOutLBA == 0) {
		std::cout << "ERROR: Invalid disc structure (no lead-out defined).\n";
		return false;
	}

	m_drive.SetSpeed(scanSpeed);

	int leadInErrors = 0, leadOutErrors = 0;
	int leadInScanned = 0, leadOutScanned = 0;
	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);

	// ── Lead-in check ───────────────────────────────────────────────────────
	// The true lead-in zone lives at negative LBAs (before LBA 0).  If the
	// drive supports lead-in overread we scan LBA −150 to −1.  Otherwise we
	// fall back to scanning the first 150 sectors of the program area
	// (LBA 0–149, i.e. the Track 1 pregap) which is the next-best indicator
	// of inner-edge disc health.
	bool canOverreadLeadIn = m_drive.TestOverread(/*leadIn=*/true);

	if (canOverreadLeadIn) {
		std::cout << "  Scanning true lead-in (LBA -150 to -1)...\n";
		for (int lba = -150; lba < 0; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				return false;
			}
			int c2 = 0;
			if (m_drive.ReadSectorWithC2(static_cast<DWORD>(lba), buf.data(), nullptr, c2)) {
				leadInScanned++;
				if (c2 > 0) leadInErrors++;
			}
		}
	}
	else {
		std::cout << "  Drive does not support lead-in overread.\n";
		std::cout << "  Scanning pregap area (LBA 0-149) as inner-edge proxy...\n";
		for (DWORD lba = 0; lba < 150; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				return false;
			}
			int c2 = 0;
			if (m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2)) {
				leadInScanned++;
				if (c2 > 0) leadInErrors++;
			}
		}
	}

	// ── Lead-out check ──────────────────────────────────────────────────────
	// Scan the last 150 sectors of the program area (just before the lead-out
	// marker).  Guard against overlap with the lead-in/pregap scan above.
	DWORD leadOutStart = disc.leadOutLBA;
	DWORD minLeadOut = canOverreadLeadIn ? 0 : 150;  // avoid re-scanning pregap
	leadOutStart = (leadOutStart > 150) ? (leadOutStart - 150) : minLeadOut;
	if (leadOutStart < minLeadOut) leadOutStart = minLeadOut;
	DWORD leadOutEnd = disc.leadOutLBA;

	std::cout << "  Scanning lead-out area (LBA " << leadOutStart << "-" << (leadOutEnd - 1) << ")...\n";
	for (DWORD lba = leadOutStart; lba < leadOutEnd; lba++) {
		if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
			m_drive.SetSpeed(0);
			return false;
		}
		int c2 = 0;
		if (m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2)) {
			leadOutScanned++;
			if (c2 > 0) leadOutErrors++;
		}
	}

	m_drive.SetSpeed(0);

	// ── Results ─────────────────────────────────────────────────────────────
	std::cout << "\n--- Lead Area Results ---\n";

	std::cout << "  Lead-in  ";
	if (canOverreadLeadIn)
		std::cout << "(LBA -150 to -1):        ";
	else
		std::cout << "(pregap LBA 0-149):      ";
	std::cout << leadInScanned << " sectors read, " << leadInErrors << " errors";
	if (leadInScanned == 0) std::cout << "  [SKIP - no sectors readable]";
	else if (leadInErrors == 0) std::cout << "  [OK]";
	else std::cout << "  [WARN - TOC area may be degraded]";
	std::cout << "\n";

	std::cout << "  Lead-out (last 150 sectors):   "
		<< leadOutScanned << " sectors read, " << leadOutErrors << " errors";
	if (leadOutScanned == 0) std::cout << "  [SKIP - no sectors readable]";
	else if (leadOutErrors == 0) std::cout << "  [OK]";
	else std::cout << "  [WARN - outer edge damage]";
	std::cout << "\n";

	if (leadInErrors > 0 || leadOutErrors > 0) {
		std::cout << "\n  Note: Lead area errors can cause disc recognition problems\n";
		std::cout << "        and indicate edge-region physical damage.\n";
		return true;
	}
	else {
		std::cout << "\n  Lead areas are intact. Disc structure is healthy.\n";
		return true;
	}
}

void OpticalDrive::GenerateSurfaceMap(DiscInfo& disc, const std::wstring& filename, int scanSpeed) {
	std::cout << "\n=== Generating Disc Surface Map ===\n";
	m_drive.SetSpeed(scanSpeed);

	DWORD totalSectors = CalculateTotalAudioSectors(disc);

	if (totalSectors == 0) {
		std::cout << "No audio tracks to scan.\n";
		m_drive.SetSpeed(0);
		return;
	}

	std::ofstream mapFile;
	if (!filename.empty()) {
		mapFile.open(filename);
		if (!mapFile) {
			std::cout << "ERROR: Cannot create map file.\n";
			m_drive.SetSpeed(0);
			return;
		}
	}

	ProgressIndicator progress(40);
	progress.SetLabel("  Surface Scan");
	progress.Start();

	std::vector<BYTE> buf;
	try {
		buf.resize(AUDIO_SECTOR_SIZE);
	}
	catch (const std::bad_alloc&) {
		std::cout << "ERROR: Failed to allocate sector buffer.\n";
		m_drive.SetSpeed(0);
		return;
	}

	DWORD scannedSectors = 0;
	int totalC2Errors = 0;
	int failedReads = 0;
	int errorSectors = 0;
	bool headerWritten = false;

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;

		// Calculate range: for Track 1, start at LBA 0; for others, include pregap
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		// Verify track boundaries are valid
		if (start > t.endLBA) {
			std::cout << "WARNING: Track " << t.trackNumber << " has invalid boundaries (start > end).\n";
			continue;
		}

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				if (mapFile.is_open()) mapFile.flush();
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return;
			}

			int c2Errors = 0;
			bool readOk = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2Errors);

			if (!readOk) failedReads++;
			if (c2Errors > 0) {
				errorSectors++;
				totalC2Errors += c2Errors;
			}

			// Only write rows for problem sectors
			if (mapFile.is_open() && (!readOk || c2Errors > 0)) {
				// Write header on first problem row
				if (!headerWritten) {
					mapFile << "LBA,Track,Region,Time_MMSSFF,C2_Errors,Read_Status,Severity\n";
					headerWritten = true;
				}

				const char* region = (lba < t.startLBA) ? "PREGAP" : "AUDIO";

				// Convert LBA to MM:SS:FF (75 frames per second)
				DWORD absLba = lba + 150; // adjust for 2-second offset
				int mm = absLba / (75 * 60);
				int ss = (absLba / 75) % 60;
				int ff = absLba % 75;

				// Classify severity
				const char* severity = "CRITICAL";
				if (readOk) {
					if (c2Errors > 50)      severity = "HIGH";
					else if (c2Errors > 10) severity = "MEDIUM";
					else                    severity = "LOW";
				}

				mapFile << lba << ","
					<< t.trackNumber << ","
					<< region << ","
					<< std::setfill('0') << std::setw(2) << mm << ":"
					<< std::setw(2) << ss << ":"
					<< std::setw(2) << ff << ","
					<< c2Errors << ","
					<< (readOk ? "OK" : "FAIL") << ","
					<< severity << "\n";
			}

			scannedSectors++;
			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}

	// If no problems were found, write a clean summary instead
	if (mapFile.is_open() && !headerWritten) {
		mapFile << "No errors detected. All " << scannedSectors
			<< " sectors read successfully with zero C2 errors.\n";
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              SURFACE MAP SUMMARY\n";
	std::cout << std::string(60, '=') << "\n";
	std::cout << "  Sectors scanned:  " << scannedSectors << "\n";
	std::cout << "  C2 error sectors: " << errorSectors;
	if (scannedSectors > 0)
		std::cout << " (" << std::fixed << std::setprecision(3)
		<< (errorSectors * 100.0 / scannedSectors) << "%)";
	std::cout << "\n";
	std::cout << "  Total C2 errors:  " << totalC2Errors << "\n";
	std::cout << "  Read failures:    " << failedReads << "\n";

	std::cout << "\n  Surface condition: ";
	if (failedReads > 0)
		std::cout << "DAMAGED - unreadable sectors present\n";
	else if (errorSectors == 0)
		std::cout << "CLEAN - no errors detected\n";
	else if (errorSectors * 100.0 / scannedSectors < 0.01)
		std::cout << "GOOD - very minor surface wear\n";
	else if (errorSectors * 100.0 / scannedSectors < 0.1)
		std::cout << "FAIR - some surface damage\n";
	else
		std::cout << "POOR - significant surface damage\n";

	if (!filename.empty()) {
		std::wcout << L"  Map saved to:     " << filename << L"\n";
		if (headerWritten)
			std::cout << "  (CSV contains only problem sectors)\n";
		else
			std::cout << "  (No errors - clean disc)\n";
	}
	std::cout << std::string(60, '=') << "\n";
}
