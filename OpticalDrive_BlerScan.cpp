	#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include "PioneerVendor.h"
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstring>

// ============================================================================
// BLER Scanning - Sector Reading and Error Collection
// ============================================================================

bool OpticalDrive::RunBlerScan(const DiscInfo& disc, BlerResult& result, int scanSpeed) {
	// Lock the tray for the scan so an accidental eject can't abort it.
	DriveDoorLockGuard doorLock(m_drive);

	// Pioneer PureRead interpolates/re-reads to hide errors after retries, which
	// would mask the raw C1/C2 errors this scan measures. Force it Off for the whole
	// scan (previous mode restored on scope exit); no-ops on non-Pioneer drives.
	// Consistent with the Q-Check, C2, Disc-Rot and Disc-Balance scans. If the
	// per-sector path below hands off to the Pioneer vendor scan (RunQCheckScan),
	// that scan's own guard nests harmlessly — it sees PureRead already Off.
	PioneerVendor pioneerProbe(m_drive);
	PioneerPureReadOffGuard pioneerPureReadGuard(m_drive, pioneerProbe.IsPioneerDrive());

	// Pioneer BD burners don't populate the per-sector READ CD C2 error-pointer
	// bitmap (it reads all-zero regardless of disc condition), so the naive
	// per-sector BLER path below would miss real errors. The drive CAN still
	// measure C2 — via the vendor quality scan (0x3B/0x3C), which reads its CIRC
	// decoder's real C1/C2/CU. Reroute to that (same as menu option 6). The
	// per-sector code below is only reached on (older) drives without the vendor scan.
	if (RunPioneerVendorC2Fallback(disc, result, scanSpeed, "BLER Scan"))
		return true;

	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: C2 not supported.\n";
		return false;
	}

	// Read C1 capability AFTER CheckC2Support() refreshes detection state
	bool hasC1Support = m_drive.SupportsC1BlockErrors();

	if (hasC1Support)
		std::cout << "\n=== CD BLER Quality Scan ===\n";
	else
		std::cout << "\n=== CD Integrity Scan (C2 Only) ===\n";

	if (hasC1Support) {
		std::cout << "C1 block error reporting available - C1 and C2 errors will be reported.\n\n";
	}
	else {
		std::cout << "Note: C1 errors are not available on this drive.\n";
		std::cout << "      This scan verifies read integrity (C2) but cannot measure physical disc degradation.\n\n";
	}

	DWORD totalSectors = 0, firstLBA = 0, lastLBA = 0;
	bool first = true;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) {
			DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
			if (first) { firstLBA = start; first = false; }
			lastLBA = t.endLBA;
			totalSectors += t.endLBA - start + 1;
		}
	}

	if (totalSectors == 0) {
		std::cout << "No audio tracks.\n";
		return false;
	}

	result = BlerResult{};
	result.totalSectors = totalSectors;
	result.totalSeconds = (totalSectors + 74) / 75;
	result.perSecondC2.resize(result.totalSeconds + 1, { 0, 0 });
	result.hasC1Data = hasC1Support;
	if (hasC1Support) {
		result.perSecondC1.resize(result.totalSeconds + 1, { 0, 0 });
	}

	std::cout << "Scanning " << totalSectors << " sectors...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";
	m_drive.SetSpeed(scanSpeed);

	DWORD scannedSectors = 0;
	int currentErrorRun = 0;

	// Track error LBAs for cluster analysis
	std::vector<DWORD> errorLBAs;

	ProgressIndicator progress(40);
	progress.SetLabel("  Scanning");
	progress.Start();

	ScsiDrive::C2ReadOptions c2Opts;
	c2Opts.multiPass = false;
	c2Opts.countBytes = true;   // Byte counting — PlexTools-style C2 error interpretation
	c2Opts.defeatCache = true;

	std::vector<BYTE> c2Buffer(C2_ERROR_SIZE, 0);

	// Collects (LBA, C2 count) for all sectors with C2 errors — trimmed after the loop
	std::vector<std::pair<DWORD, int>> sectorErrors;

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		currentErrorRun = 0;  // reset at each track boundary

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				std::cout << "\n\n*** Scan cancelled by user ***\n";
				return false;
			}

			std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
			int c2Errors = 0;
			int c1BlockErrors = 0, c2BlockErrors = 0;
			BYTE senseKey = 0, asc = 0, ascq = 0;
			std::memset(c2Buffer.data(), 0, C2_ERROR_SIZE);

			// Use audio-relative sector count for indexing to handle
			// mixed-mode discs with non-audio gaps between tracks
			size_t secIdx = static_cast<size_t>(scannedSectors / 75);
			if (secIdx >= result.perSecondC2.size())
				secIdx = result.perSecondC2.size() - 1;

			// Record the starting LBA for each time bucket
			if (scannedSectors % 75 == 0) {
				result.perSecondC2[secIdx].first = lba;
				if (hasC1Support)
					result.perSecondC1[secIdx].first = lba;
			}

			bool readSuccess = m_drive.ReadSectorWithC2Ex(
				lba, buf.data(), nullptr, c2Errors, c2Buffer.data(), c2Opts,
				&senseKey, &asc, &ascq,
				hasC1Support ? &c1BlockErrors : nullptr,
				hasC1Support ? &c2BlockErrors : nullptr);

			int zoneError = 0;

			if (readSuccess) {
				bool recovered = (senseKey == 0x01);

				// Collect C1 block errors (when available)
				if (hasC1Support && c1BlockErrors > 0) {
					result.totalC1Errors += c1BlockErrors;
					result.totalC1Sectors++;
					result.perSecondC1[secIdx].second += c1BlockErrors;

					if (c1BlockErrors > result.maxC1InSingleSector) {
						result.maxC1InSingleSector = c1BlockErrors;
						result.worstC1SectorLBA = lba;
					}
				}

				int effectiveC2 = std::max(c2Errors, c2BlockErrors);

				if (effectiveC2 > 0) {
					if (!recovered) {
						result.totalC2Errors += effectiveC2;
						result.totalC2Sectors++;
						result.perSecondC2[secIdx].second += effectiveC2;

						if (effectiveC2 > result.maxC2InSingleSector) {
							result.maxC2InSingleSector = effectiveC2;
							result.worstSectorLBA = lba;
						}

						zoneError = 1;
						errorLBAs.push_back(lba);
						sectorErrors.push_back({ lba, effectiveC2 });

						currentErrorRun++;
						if (currentErrorRun > result.consecutiveErrorSectors) {
							result.consecutiveErrorSectors = currentErrorRun;
						}
					}
					else {
						currentErrorRun = 0;
						result.recoveredC2Errors += effectiveC2;
						result.recoveredC2Sectors++;
					}
				}
				else {
					currentErrorRun = 0;
				}
			}
			else {
				result.totalReadFailures++;
				result.perSecondC2[secIdx].second++;   // register in per-second data
				zoneError = 1;
				errorLBAs.push_back(lba);

				currentErrorRun++;
				if (currentErrorRun > result.consecutiveErrorSectors) {
					result.consecutiveErrorSectors = currentErrorRun;
				}
			}

			ClassifyZone(lba, firstLBA, lastLBA, zoneError, result.zoneStats);

			scannedSectors++;
			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	// Sort and keep top 10 worst C2 sectors by error count
	std::sort(sectorErrors.begin(), sectorErrors.end(),
		[](const auto& a, const auto& b) { return a.second > b.second; });
	if (sectorErrors.size() > 10) sectorErrors.resize(10);
	result.topWorstC2Sectors = std::move(sectorErrors);

	// Perform analysis
	AnalyzeBlerResults(result, errorLBAs, scanSpeed);

	// Print report
	PrintBlerReport(disc, result);

	if (result.totalC2Sectors == 0 && result.totalReadFailures == 0
		&& !m_drive.SupportsC1BlockErrors()) {
		result.c2Unverified = true;
	}

	return true;
}
