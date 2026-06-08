#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleGraph.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cstring>

// ============================================================================
// C2 Error Scanning - Quick disc health check
// ============================================================================

// Maximum bad-sector entries kept in memory / displayed in the chart.
// Prevents runaway memory usage and console flooding on heavily damaged discs.
static constexpr size_t MAX_BAD_SECTOR_ENTRIES = 500;

// Helper function to calculate total audio sectors
DWORD OpticalDrive::CalculateTotalAudioSectors(const DiscInfo& disc) const {
	DWORD totalSectors = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) {
			DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
			totalSectors += t.endLBA - start + 1;
		}
	}
	return totalSectors;
}

// C2 accuracy validation
bool OpticalDrive::ValidateC2Accuracy(DWORD testLBA) {
	return m_drive.ValidateC2Accuracy(testLBA);
}

// Helper to get human-readable sense key description
static const char* GetSenseKeyDescription(BYTE senseKey) {
	switch (senseKey) {
	case 0x00: return "No Error";
	case 0x01: return "Recovered Error";
	case 0x02: return "Not Ready";
	case 0x03: return "Medium Error";
	case 0x04: return "Hardware Error";
	case 0x05: return "Illegal Request";
	case 0x06: return "Unit Attention";
	case 0x07: return "Data Protect";
	case 0x08: return "Blank Check";
	case 0x0B: return "Aborted Command";
	default: return "Unknown";
	}
}

// Helper to get ASC/ASCQ description
static std::string GetASCDescription(BYTE asc, BYTE ascq) {
	if (asc == 0x11) {
		if (ascq == 0x00) return "Unrecovered read error";
		if (ascq == 0x05) return "L-EC uncorrectable error";
		if (ascq == 0x06) return "CIRC unrecovered error";
	}
	if (asc == 0x15 && ascq == 0x01) return "Mechanical positioning error";
	if (asc == 0x21 && ascq == 0x00) return "Logical block address out of range";
	if (asc == 0x30 && ascq == 0x00) return "Incompatible medium";
	if (asc == 0x64 && ascq == 0x00) return "Illegal mode for this track";
	if (asc == 0x00 && ascq == 0x00) return "No error";

	// Generic description using stringstream
	std::ostringstream oss;
	oss << "ASC=" << std::hex << std::uppercase << std::setfill('0')
		<< std::setw(2) << static_cast<int>(asc) << " ASCQ="
		<< std::setw(2) << static_cast<int>(ascq);
	return oss.str();
}

// Main C2 scanning function
bool OpticalDrive::RunC2Scan(const DiscInfo& disc, BlerResult& result, int scanSpeed) {
	std::cout << "\n=== C2 Error Scan (Quick) ===\n";
	std::cout << "Quick disc health check using C2 error reporting.\n";
	std::cout << "C2 errors indicate uncorrectable data corruption.\n\n";

	// On Pioneer BD burners the per-sector READ CD C2 area reads all-zero, so the
	// scan below would call the disc clean no matter its condition. Route to the
	// Pioneer vendor quality scan (option 6 path) for real C2 on those drives.
	if (RunPioneerVendorC2Fallback(disc, result, scanSpeed, "C2 Error Scan"))
		return true;

	// Check drive support
	if (!m_drive.CheckC2Support()) {
		Console::Error("ERROR: Drive does not support C2 error reporting.\n");
		return false;
	}

	// Calculate total audio sectors and overall LBA range for zone classification
	DWORD totalSectors = 0;
	DWORD globalFirstLBA = 0, globalLastLBA = 0;
	bool foundFirst = false;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) {
			DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
			if (!foundFirst) {
				globalFirstLBA = start;
				foundFirst = true;
			}
			else if (start < globalFirstLBA) {
				globalFirstLBA = start;
			}
			if (t.endLBA > globalLastLBA) globalLastLBA = t.endLBA;
			totalSectors += t.endLBA - start + 1;
		}
	}

	if (totalSectors == 0) {
		Console::Error("No audio tracks found.\n");
		return false;
	}

	// Initialize result structure
	result = BlerResult{};
	result.totalSectors = totalSectors;
	result.totalSeconds = (totalSectors + 74) / 75;
	result.perSecondC2.resize(result.totalSeconds + 1, { 0, 0 });

	// Track bad sectors with sense codes
	std::vector<C2SectorError> badSectors;
	int totalBadSectorCount = 0;   // True total before capping

	std::cout << "Scanning " << totalSectors << " sectors ("
		<< (result.totalSeconds / 60) << ":"
		<< std::setfill('0') << std::setw(2) << (result.totalSeconds % 60)
		<< std::setfill(' ') << " mm:ss)...\n";
	std::cout << "Speed: " << (scanSpeed == 0 ? "Max" : std::to_string(scanSpeed) + "x") << "\n";
	std::cout << "Press ESC or Ctrl+C to cancel\n\n";

	m_drive.SetSpeed(scanSpeed);

	DWORD scannedSectors = 0;
	int currentErrorRun = 0;

	// Progress indicator
	ProgressIndicator progress(40);
	progress.SetLabel("  C2 Scan");
	progress.Start();

	// Configure C2 reading options
	ScsiDrive::C2ReadOptions c2Opts;
	c2Opts.multiPass = false;
	c2Opts.countBytes = true;   // Byte counting — PlexTools-style C2 error interpretation
	c2Opts.defeatCache = true;  // Defeat drive cache for accurate reads

	// Allocate C2 buffer ONCE, outside the loop
	std::vector<BYTE> c2Buffer(C2_ERROR_SIZE, 0);

	// Collect error LBAs for cluster detection (separate from capped display list)
	std::vector<DWORD> errorLBAs;

	// Scan all audio tracks
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		currentErrorRun = 0;  // reset at each track boundary

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			// Check for user interrupt
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				Console::Warning("\n\n*** Scan cancelled by user ***\n");
				return false;
			}

			std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
			int c2Errors = 0;
			BYTE senseKey = 0, asc = 0, ascq = 0;

			// Zero out C2 buffer before each read (reuse same buffer)
			std::memset(c2Buffer.data(), 0, C2_ERROR_SIZE);

			// Calculate per-second index
			size_t secIdx = static_cast<size_t>(scannedSectors / 75);
			if (secIdx >= result.perSecondC2.size())
				secIdx = result.perSecondC2.size() - 1;

			// Record starting LBA for each time bucket
			if (scannedSectors % 75 == 0)
				result.perSecondC2[secIdx].first = lba;

			// Read sector with C2 error detection
			bool readSuccess = m_drive.ReadSectorWithC2Ex(lba, buf.data(), nullptr, c2Errors,
				c2Buffer.data(), c2Opts, &senseKey, &asc, &ascq);

			if (readSuccess) {
				// CRITICAL: Check if errors were recovered by the drive
				// Sense key 0x01 means "Recovered Error" - drive fixed it internally
				bool recovered = (senseKey == 0x01);

				if (c2Errors > 0) {
					// Only count as actual errors if NOT recovered
					if (!recovered) {
						result.totalC2Errors += c2Errors;
						result.totalC2Sectors++;
						result.perSecondC2[secIdx].second += c2Errors;

						// Track worst sector
						if (c2Errors > result.maxC2InSingleSector) {
							result.maxC2InSingleSector = c2Errors;
							result.worstSectorLBA = lba;
						}

						// Track consecutive errors
						currentErrorRun++;
						if (currentErrorRun > result.consecutiveErrorSectors) {
							result.consecutiveErrorSectors = currentErrorRun;
						}

						// Always record LBA for cluster analysis (uncapped)
						errorLBAs.push_back(lba);

						// Record in display list (capped to prevent runaway growth)
						totalBadSectorCount++;
						if (badSectors.size() < MAX_BAD_SECTOR_ENTRIES) {
							C2SectorError errorEntry;
							errorEntry.lba = lba;
							errorEntry.c2Errors = c2Errors;
							errorEntry.senseKey = senseKey;
							errorEntry.asc = asc;
							errorEntry.ascq = ascq;
							badSectors.push_back(errorEntry);
						}

						// Classify into inner/middle/outer zone
						ClassifyZone(lba, globalFirstLBA, globalLastLBA, 1, result.zoneStats);
					}
					else {
						// Recovered error - reset consecutive error counter
						currentErrorRun = 0;
						result.recoveredC2Errors += c2Errors;
						result.recoveredC2Sectors++;

						// Record recovered errors in display list (capped)
						totalBadSectorCount++;
						if (badSectors.size() < MAX_BAD_SECTOR_ENTRIES) {
							C2SectorError errorEntry;
							errorEntry.lba = lba;
							errorEntry.c2Errors = c2Errors;
							errorEntry.senseKey = senseKey;
							errorEntry.asc = asc;
							errorEntry.ascq = ascq;
							badSectors.push_back(errorEntry);
						}

						// Classify zone (no error contribution for recovered sectors)
						ClassifyZone(lba, globalFirstLBA, globalLastLBA, 0, result.zoneStats);
					}
				}
				else {
					// Clean sector - reset error run
					currentErrorRun = 0;

					// Classify zone (clean sector)
					ClassifyZone(lba, globalFirstLBA, globalLastLBA, 0, result.zoneStats);
				}
			}
			else {
				// SCSI command failed completely - this is a read failure.
				// Not counted in totalC2Sectors — read failures are tracked separately
				// via totalReadFailures so the two fields remain mutually exclusive.
				result.totalReadFailures++;
				result.perSecondC2[secIdx].second++;              // register in per-second data
				currentErrorRun++;
				if (currentErrorRun > result.consecutiveErrorSectors) {
					result.consecutiveErrorSectors = currentErrorRun;
				}

				// Always record LBA for cluster analysis (uncapped)
				errorLBAs.push_back(lba);

				// Record read failure in display list (capped)
				totalBadSectorCount++;
				if (badSectors.size() < MAX_BAD_SECTOR_ENTRIES) {
					C2SectorError errorEntry;
					errorEntry.lba = lba;
					errorEntry.c2Errors = -1;  // -1 indicates total read failure
					errorEntry.senseKey = senseKey;
					errorEntry.asc = asc;
					errorEntry.ascq = ascq;
					badSectors.push_back(errorEntry);
				}

				// Classify zone (error)
				ClassifyZone(lba, globalFirstLBA, globalLastLBA, 1, result.zoneStats);
			}

			scannedSectors++;
			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	// Calculate statistics
	result.avgC2PerSecond = result.totalSeconds > 0
		? static_cast<double>(result.totalC2Errors) / result.totalSeconds : 0;

	for (size_t i = 0; i < result.perSecondC2.size(); i++) {
		if (result.perSecondC2[i].second > result.maxC2PerSecond) {
			result.maxC2PerSecond = result.perSecondC2[i].second;
			result.worstSecondLBA = result.perSecondC2[i].first;
		}
	}

	// Build error cluster list from collected error LBAs
	std::sort(errorLBAs.begin(), errorLBAs.end());
	DetectErrorClusters(errorLBAs, result.errorClusters, scanSpeed);
	for (const auto& c : result.errorClusters) {
		int clusterSize = static_cast<int>(c.size());
		if (clusterSize > result.largestClusterSize)
			result.largestClusterSize = clusterSize;
	}

	// Detect edge concentration: significantly higher error rate in inner or outer zone
	double innerRate = result.zoneStats.InnerErrorRate();
	double middleRate = result.zoneStats.MiddleErrorRate();
	double outerRate = result.zoneStats.OuterErrorRate();
	result.hasEdgeConcentration =
		(outerRate > innerRate * 2.0 && outerRate > 1.0) ||
		(innerRate > outerRate * 2.0 && innerRate > 1.0);

	// Detect progressive pattern: error rate increases monotonically inner → outer
	result.hasProgressivePattern =
		innerRate < middleRate &&
		middleRate < outerRate &&
		outerRate > 0.5;

	// Quality rating
	if (result.totalReadFailures > 0)
		result.qualityRating = "BAD";
	else if (result.totalC2Sectors == 0)
		result.qualityRating = "EXCELLENT";
	else if (result.avgC2PerSecond < 1.0 && result.consecutiveErrorSectors < 3
		&& result.maxC2InSingleSector < 50)
		result.qualityRating = "GOOD";
	else if (result.avgC2PerSecond < 10.0 && result.consecutiveErrorSectors < 10
		&& result.maxC2InSingleSector < 100)
		result.qualityRating = "ACCEPTABLE";
	else if (result.avgC2PerSecond < 50.0)
		result.qualityRating = "FAIR";
	else
		result.qualityRating = "POOR";

	// Flag potentially non-functional C2 reporting.
	// If the entire disc scanned with zero C2 errors AND the drive doesn't
	// populate C1 block error stats (bytes 294-295), the C2 pointer bitmap
	// may also be non-functional.  Suggest vendor-command-based scanning.
	if (result.totalC2Sectors == 0 && result.totalReadFailures == 0
		&& !m_drive.SupportsC1BlockErrors()) {
		result.c2Unverified = true;
	}

	// Print report
	PrintC2ScanReport(result, disc, scanSpeed);

	if (result.c2Unverified) {
		std::cout << "\n  ** NOTE: Zero C2 errors across entire disc, but this drive\n"
			<< "     does not populate C2 block error stats (bytes 294-295).\n"
			<< "     The C2 pointer bitmap may not be functional.\n"
			<< "     Run Q-Check (quality scan) for hardware-level C1/C2/CU\n"
			<< "     measurement using the drive's internal ECC decoder. **\n";
	}

	return true;
}

// ============================================================================
// Pioneer vendor-scan fallback (shared by options 7 "C2 Error Scan" and
// 8 "BLER Scan"). See the declaration in OpticalDrive.h for rationale.
// ============================================================================
bool OpticalDrive::RunPioneerVendorC2Fallback(const DiscInfo& disc, BlerResult& result,
	int scanSpeed, const char* featureLabel) {
	// Only relevant on Pioneer drives that expose the 0x3B/0x3C vendor scan.
	// SupportsPioneerScan() caches its probe, so the repeat call inside
	// RunQCheckScan below is cheap.
	if (!m_drive.IsPioneer() || !m_drive.SupportsPioneerScan())
		return false;

	Console::Warning("  [Pioneer] ");
	std::cout << (featureLabel ? featureLabel : "This scan")
		<< ": the per-sector READ CD C2 area reads all-zero on Pioneer BD\n"
		<< "            burners, so it cannot see real C2 on this drive. Running the\n"
		<< "            Pioneer vendor quality scan (option 6 path) instead.\n";

	QCheckResult qc;
	if (!RunQCheckScan(disc, qc, scanSpeed)) {
		// Vendor scan turned out to be unavailable after all - let the caller
		// fall through to the per-sector path so the user still gets a result.
		Console::Info("  [Pioneer] Vendor quality scan unavailable; using per-sector C2 scan.\n");
		return false;
	}

	// Translate the QCheck (per-time-slice C1/C2/CU) result into the BlerResult
	// the option-7/8 CSV writer expects. Counts are per ~75-sector slice rather
	// than per-sector, since the vendor scan reports aggregated slices.
	DWORD firstLBA = 0, lastLBA = 0;
	bool first = true;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		if (first) { firstLBA = start; first = false; }
		lastLBA = t.endLBA;
	}

	result = BlerResult{};
	result.totalSectors = qc.totalSectors;
	result.totalSeconds = static_cast<int>(qc.totalSeconds);
	result.hasC1Data = true;
	result.totalC1Errors = qc.totalC1;
	result.avgC1PerSecond = qc.avgC1PerSecond;
	result.maxC1PerSecond = qc.maxC1PerSecond;
	result.totalC2Errors = qc.totalC2;
	result.avgC2PerSecond = qc.avgC2PerSecond;
	result.maxC2PerSecond = qc.maxC2PerSecond;
	result.qualityRating = qc.qualityRating.empty() ? "UNKNOWN" : qc.qualityRating;

	result.perSecondC1.reserve(qc.samples.size());
	result.perSecondC2.reserve(qc.samples.size());
	for (const auto& s : qc.samples) {
		result.perSecondC1.push_back({ s.lba, s.c1 });
		result.perSecondC2.push_back({ s.lba, s.c2 });
		if (s.c1 > 0) result.totalC1Sectors++;
		if (s.c2 > 0) {
			result.totalC2Sectors++;
			if (s.c2 > result.maxC2InSingleSector) {
				result.maxC2InSingleSector = s.c2;
				result.worstSectorLBA = s.lba;
			}
		}
		ClassifyZone(s.lba, firstLBA, lastLBA, s.c2 > 0 ? 1 : 0, result.zoneStats);
	}

	return true;
}

void OpticalDrive::PrintC2SenseCodeChart(const std::vector<C2SectorError>& badSectors, const DiscInfo& disc, const BlerResult& result) {
	if (badSectors.empty()) return;

	std::cout << "\n" << std::string(80, '=') << "\n";
	std::cout << "                    BAD SECTORS DETECTED\n";
	std::cout << std::string(80, '=') << "\n\n";

	std::cout << "  LBA      Track  Time      C2 Errs  Sense  ASC/ASCQ  Description\n";
	std::cout << "  " << std::string(78, '-') << "\n";

	// Sort by severity: failures first (-1), then by C2 error count descending
	auto sortedSectors = badSectors;
	std::sort(sortedSectors.begin(), sortedSectors.end(),
		[](const C2SectorError& a, const C2SectorError& b) {
			// Failures (c2Errors == -1) come first
			if (a.c2Errors < 0 && b.c2Errors >= 0) return true;
			if (a.c2Errors >= 0 && b.c2Errors < 0) return false;
			// Then sort by error count descending
			return a.c2Errors > b.c2Errors;
		});

	// Display bad sectors (may be capped by caller)
	for (size_t i = 0; i < sortedSectors.size(); i++) {
		const auto& err = sortedSectors[i];

		// Find which track this LBA belongs to
		int trackNum = 0;
		for (const auto& t : disc.tracks) {
			if (t.isAudio) {
				DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
				if (err.lba >= start && err.lba <= t.endLBA) {
					trackNum = t.trackNumber;
					break;
				}
			}
		}

		// Calculate time position (MM:SS.FF format)
		DWORD sectorOffset = static_cast<DWORD>(err.lba);
		int minutes = (sectorOffset / 75) / 60;
		int seconds = (sectorOffset / 75) % 60;
		int frames = sectorOffset % 75;

		// Print row with color coding based on severity
		std::cout << "  ";

		// LBA
		std::cout << std::setw(8) << err.lba << " ";

		// Track
		std::cout << std::setw(5) << trackNum << "  ";

		// Time
		std::cout << std::setfill('0')
			<< std::setw(2) << minutes << ":"
			<< std::setw(2) << seconds << "."
			<< std::setw(2) << frames
			<< std::setfill(' ') << "  ";

		// C2 Errors - color coded
		if (err.c2Errors < 0) {
			Console::SetColor(Console::Color::Red);
			std::cout << "  FAILED";
			Console::Reset();
			std::cout << "  ";
		}
		else {
			if (err.c2Errors > 100) Console::SetColor(Console::Color::Red);
			else if (err.c2Errors > 10) Console::SetColor(Console::Color::Yellow);
			else Console::SetColor(Console::Color::Green);

			std::cout << std::setw(7) << err.c2Errors << "  ";
			Console::Reset();
		}

		// Sense Key
		std::cout << " 0x" << std::hex << std::uppercase << std::setfill('0')
			<< std::setw(2) << static_cast<int>(err.senseKey) << std::dec << "  ";

		// ASC/ASCQ
		std::cout << std::hex << std::uppercase
			<< std::setw(2) << static_cast<int>(err.asc) << "/"
			<< std::setw(2) << static_cast<int>(err.ascq) << std::dec << std::nouppercase
			<< std::setfill(' ') << "  ";

		// Description - Enhanced to explain sense codes
		if (err.c2Errors < 0) {
			// Read failure
			Console::SetColor(Console::Color::Red);
			std::cout << GetSenseKeyDescription(err.senseKey) << " - "
				<< GetASCDescription(err.asc, err.ascq);
		}
		else if (err.senseKey == 0x01) {
			// Recovered error - drive fixed it
			Console::SetColor(Console::Color::Yellow);
			std::cout << "Recovered (" << GetASCDescription(err.asc, err.ascq) << ")";
		}
		else if (err.senseKey == 0x00 && err.c2Errors > 0) {
			// C2 errors detected but SCSI command succeeded
			Console::SetColor(Console::Color::Red);
			std::cout << "Uncorrectable data corruption";
		}
		else if (err.senseKey == 0x03) {
			// Medium error
			Console::SetColor(Console::Color::Red);
			std::cout << GetSenseKeyDescription(err.senseKey) << " - "
				<< GetASCDescription(err.asc, err.ascq);
		}
		else {
			// Other sense codes
			if (err.IsUnrecoverable()) Console::SetColor(Console::Color::Red);
			else Console::SetColor(Console::Color::Yellow);
			std::cout << GetSenseKeyDescription(err.senseKey) << " - "
				<< GetASCDescription(err.asc, err.ascq);
		}

		Console::Reset();
		std::cout << "\n";
	}

	std::cout << "\n  " << std::string(78, '-') << "\n";
	// totalC2Sectors (unrecovered C2) + recoveredC2Sectors + totalReadFailures are
	// mutually exclusive — sum gives the true bad-sector count with no double-counting.
	std::cout << "  Total bad sectors: " << (result.totalReadFailures + result.totalC2Sectors + result.recoveredC2Sectors) << "\n";

	// Summary by error type (counted from display list for per-category breakdown)
	int readFailures = 0, unrecoverable = 0, dataCorruption = 0;
	for (const auto& err : badSectors) {
		if (err.c2Errors < 0) readFailures++;
		else if (err.senseKey == 0x00 && err.c2Errors > 0) dataCorruption++;
		else if (err.IsUnrecoverable()) unrecoverable++;
	}

	// true totals for read failures and recovered errors; display-list counts
	// (with overflow marker) for the sub-categories derived from the capped list.
	const size_t trueTotalBad = static_cast<size_t>(
		result.totalReadFailures + result.totalC2Sectors + result.recoveredC2Sectors);
	const bool capped = badSectors.size() < trueTotalBad;

	std::cout << "  Read failures:     " << result.totalReadFailures << " (SCSI command failed)\n";
	std::cout << "  Recovered errors:  " << result.recoveredC2Sectors << " (drive fixed internally)\n";
	std::cout << "  Data corruption:   " << dataCorruption << (capped ? "+" : "")
		<< " (C2 errors, sense=0x00)\n";
	std::cout << "  Unrecoverable:     " << unrecoverable << (capped ? "+" : "")
		<< " (other SCSI errors)\n";

	std::cout << "\n  Note: Sense code 0x00/00/00 with C2 errors means:\n";
	std::cout << "        - SCSI read command succeeded\n";
	std::cout << "        - BUT data has uncorrectable corruption (C2 errors)\n";
	std::cout << "        - This is the most common error type on degraded discs\n";

	std::cout << "\n  Legend:\n";
	Console::SetColor(Console::Color::Green);
	std::cout << "    GREEN";
	Console::Reset();
	std::cout << "  - Minor C2 error count (1-10)\n";
	Console::SetColor(Console::Color::Yellow);
	std::cout << "    YELLOW";
	Console::Reset();
	std::cout << " - Moderate C2 error count (11-100)";
	if (result.recoveredC2Sectors > 0)
		std::cout << " or drive-recovered error";
	std::cout << "\n";
	Console::SetColor(Console::Color::Red);
	std::cout << "    RED";
	Console::Reset();
	std::cout << "    - Critical C2 count (>100), data corruption, or unrecoverable error\n";
}

void OpticalDrive::PrintC2ScanReport(const BlerResult& result, const DiscInfo& disc, int scanSpeed) {
	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "                C2 ERROR SCAN REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	// Scan summary
	std::cout << "\n--- Scan Summary ---\n";
	std::cout << "  Sectors scanned:   " << result.totalSectors << "\n";
	std::cout << "  Disc length:       "
		<< (result.totalSeconds / 60) << ":";
	std::cout << std::setfill('0') << std::setw(2) << (result.totalSeconds % 60);
	std::cout << std::setfill(' ') << " (mm:ss)\n";
	std::cout << "  Scan speed:        "
		<< (scanSpeed == 0 ? "Max" : std::to_string(scanSpeed) + "x") << "\n";

	// C2 Error statistics
	std::cout << "\n--- C2 Error Statistics ---\n";
	std::cout << "  Total C2 errors:       " << result.totalC2Errors << "\n";
	std::cout << "  Sectors with errors:   " << result.totalC2Sectors;
	if (result.totalSectors > 0) {
		double pct = (result.totalC2Sectors * 100.0 / result.totalSectors);
		std::cout << " (" << std::fixed << std::setprecision(3) << pct << "%)";
	}
	std::cout << "\n";

	std::cout << "  Read failures:         " << result.totalReadFailures << "\n";
	std::cout << "  Recovered errors:      " << result.recoveredC2Sectors << " sectors, "
		<< result.recoveredC2Errors << " C2 errors (drive-corrected)\n";
	std::cout << "  Avg C2 errors/second:  " << std::fixed << std::setprecision(2)
		<< result.avgC2PerSecond << "\n";
	std::cout << "  Max C2 errors/second:  " << result.maxC2PerSecond;
	if (result.maxC2PerSecond > 0) {
		int worstMin = (result.worstSecondLBA / 75) / 60;
		int worstSec = (result.worstSecondLBA / 75) % 60;
		std::cout << " (at " << worstMin << ":"
			<< std::setfill('0') << std::setw(2) << worstSec << std::setfill(' ') << ")";
	}
	std::cout << "\n";

	std::cout << "  Max errors in 1 sector: " << result.maxC2InSingleSector;
	if (result.maxC2InSingleSector > 0)
		std::cout << " (LBA " << result.worstSectorLBA << ")";
	std::cout << "\n";

	std::cout << "  Longest error run:      " << result.consecutiveErrorSectors << " sectors\n";

	// Zone error distribution
	std::cout << "\n--- Zone Error Rates ---\n";
	std::cout << "  Inner  (0-33%):    " << std::fixed << std::setprecision(2)
		<< result.zoneStats.InnerErrorRate() << "% ("
		<< result.zoneStats.innerErrors << "/" << result.zoneStats.innerSectors << ")\n";
	std::cout << "  Middle (33-66%):   " << std::fixed << std::setprecision(2)
		<< result.zoneStats.MiddleErrorRate() << "% ("
		<< result.zoneStats.middleErrors << "/" << result.zoneStats.middleSectors << ")\n";
	std::cout << "  Outer  (66-100%):  " << std::fixed << std::setprecision(2)
		<< result.zoneStats.OuterErrorRate() << "% ("
		<< result.zoneStats.outerErrors << "/" << result.zoneStats.outerSectors << ")\n";

	// Error cluster summary (if any)
	if (!result.errorClusters.empty()) {
		std::cout << "\n--- Error Clusters ---\n";
		std::cout << "  Clusters found:        " << result.errorClusters.size() << "\n";
		std::cout << "  Largest cluster:       " << result.largestClusterSize << " sectors\n";
		std::cout << "  Edge concentration:    " << (result.hasEdgeConcentration ? "YES" : "NO") << "\n";
		std::cout << "  Progressive pattern:   " << (result.hasProgressivePattern ? "YES" : "NO") << "\n";
	}

	// Print error distribution chart
	PrintC2Chart(result);

	// Quality assessment
	std::cout << "\n" << std::string(60, '-') << "\n";
	Console::SetColor(Console::Color::Cyan);
	std::cout << "  QUALITY RATING: ";
	Console::Reset();

	if (result.qualityRating == "EXCELLENT") {
		Console::SetColor(Console::Color::Green);
		std::cout << "EXCELLENT";
		Console::Reset();
		std::cout << "\n  No C2 errors detected. Disc is in perfect condition.\n";
	}
	else if (result.qualityRating == "GOOD") {
		Console::SetColor(Console::Color::Green);
		std::cout << "GOOD";
		Console::Reset();
		std::cout << "\n  Minor errors within acceptable limits.\n";
		std::cout << "  Disc is safe to rip with standard settings.\n";
	}
	else if (result.qualityRating == "ACCEPTABLE") {
		Console::SetColor(Console::Color::Yellow);
		std::cout << "ACCEPTABLE";
		Console::Reset();
		std::cout << "\n  Moderate error rate detected.\n";
		std::cout << "  Recommend using Secure rip mode for best results.\n";
	}
	else if (result.qualityRating == "POOR") {
		Console::SetColor(Console::Color::Red);
		std::cout << "POOR";
		Console::Reset();
		std::cout << "\n  High error rate detected.\n";
		std::cout << "  Recommend:\n";
		std::cout << "    - Clean the disc surface\n";
		std::cout << "    - Use Paranoid rip mode\n";
		std::cout << "    - Enable C2 error detection\n";
		std::cout << "    - Use low read speed (4x or lower)\n";
	}
	else {
		Console::SetColor(Console::Color::Red);
		std::cout << "BAD";
		Console::Reset();
		std::cout << "\n  CRITICAL: Read failures detected.\n";
		std::cout << "  Some data may be unrecoverable.\n";
		std::cout << "  Disc may be severely damaged or suffering from disc rot.\n";
	}

	std::cout << std::string(60, '=') << "\n";
}

void OpticalDrive::PrintC2Chart(const BlerResult& result, int width, int height) {
	if (result.perSecondC2.empty()) return;

	// No-error fast path: a short note rather than an empty plot.
	if (result.totalC2Errors == 0 && result.totalReadFailures == 0) {
		Console::SetColorRGB(Console::Theme::WhiteR,
			Console::Theme::WhiteG, Console::Theme::WhiteB);
		std::cout << "\n  C2 Error Distribution\n";
		Console::SetColorRGB(Console::Theme::GreenR,
			Console::Theme::GreenG, Console::Theme::GreenB);
		std::cout << "  " << Console::Sym::Check
			<< " No C2 errors - chart omitted.\n";
		Console::Reset();
		return;
	}

	int peak = 1;
	std::vector<int> raw;
	raw.reserve(result.perSecondC2.size());
	for (const auto& p : result.perSecondC2) {
		raw.push_back(p.second);
		if (p.second > peak) peak = p.second;
	}

	Console::GraphOptions opts;
	opts.title = "C2 Error Distribution";
	opts.subtitle = "Peak C2 errors per second across the disc";
	opts.width = width;
	opts.height = height;
	auto buckets = Console::BucketData(raw, width);
	Console::DrawBarGraph(buckets, peak, opts, result.totalSeconds);
}
