#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include "ConsoleColor.h"
#include "ConsoleGraph.h"
#include "ConsoleFormat.h"
#include "PioneerVendor.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdint>

// ============================================================================
// Disc Rot Detection
// ============================================================================

namespace {
// PioneerPureReadOffGuard and PioneerPerformanceModeGuard are now in
// PioneerVendor.h (shared with OpticalDrive_QCheck.cpp).

void RecalculateQCheckTotals(QCheckResult& result) {
	result.totalC1 = 0;
	result.totalC2 = 0;
	result.totalCU = 0;
	result.totalPioneerE22 = 0;
	result.maxC1PerSecond = 0;
	result.maxC1SecondIndex = -1;
	result.maxC2PerSecond = 0;
	result.maxC2SecondIndex = -1;
	result.maxCUPerSecond = 0;
	result.maxPioneerE22PerSecond = 0;
	result.maxPioneerE22SecondIndex = -1;

	for (int i = 0; i < static_cast<int>(result.samples.size()); i++) {
		const auto& s = result.samples[i];
		result.totalC1 += s.c1;
		result.totalC2 += s.c2;
		result.totalCU += s.cu;
		result.totalPioneerE22 += s.pioneerE22;
		if (s.c1 > result.maxC1PerSecond) {
			result.maxC1PerSecond = s.c1;
			result.maxC1SecondIndex = i;
		}
		if (s.c2 > result.maxC2PerSecond) {
			result.maxC2PerSecond = s.c2;
			result.maxC2SecondIndex = i;
		}
		if (s.cu > result.maxCUPerSecond)
			result.maxCUPerSecond = s.cu;
		if (s.pioneerE22 > result.maxPioneerE22PerSecond) {
			result.maxPioneerE22PerSecond = s.pioneerE22;
			result.maxPioneerE22SecondIndex = i;
		}
	}

	DWORD sampleCount = static_cast<DWORD>(result.samples.size());
	result.avgC1PerSecond = sampleCount > 0
		? static_cast<double>(result.totalC1) / sampleCount : 0.0;
	result.avgC2PerSecond = sampleCount > 0
		? static_cast<double>(result.totalC2) / sampleCount : 0.0;
	result.avgPioneerE22PerSecond = sampleCount > 0
		? static_cast<double>(result.totalPioneerE22) / sampleCount : 0.0;
}

const char* PioneerE22Rating(int total, double avg, int peak) {
	if (total == 0) return "Ideal";
	if (avg < 0.25 && peak < 25) return "Good";
	if (avg < 1.0 && peak < 100) return "Acceptable";
	return "Concerning";
}
}  // namespace

bool OpticalDrive::RunDiscRotScan(DiscInfo& disc, DiscRotAnalysis& result, int scanSpeed) {
	// Lock the tray for the multi-phase scan so an accidental eject can't abort it.
	DriveDoorLockGuard doorLock(m_drive);
	std::cout << "\n=== Disc Rot Detection Scan ===\n";
	std::cout << "This scan checks for physical disc degradation patterns.\n\n";

	EnsureCapabilitiesDetected();

	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: C2 error detection required but not supported.\n";
		return false;
	}

	DWORD firstLBA = 0, lastLBA = 0;
	DWORD totalSectors = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) {
			DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
			if (totalSectors == 0) firstLBA = start;
			lastLBA = t.endLBA;
			totalSectors += t.endLBA - start + 1;
		}
	}

	if (totalSectors == 0) {
		std::cout << "No audio tracks to scan.\n";
		return false;
	}

	result = DiscRotAnalysis{};
	std::vector<DWORD> errorLBAs;
	std::vector<DWORD> inconsistentLBAs;

	// ── Phase 0 (optional): C1 quality scan for early degradation ────
	// C1 errors are correctable — they reveal disc stress BEFORE C2
	// errors appear.  Available on Plextor, Pioneer, and LiteOn/MediaTek
	// drives that expose hardware quality-scan commands.
	QCheckResult c1Result;
	bool hasC1 = false;

	PioneerVendor pioneerProbe(m_drive);
	bool isPioneerDrive = pioneerProbe.IsPioneerDrive();
	result.pioneerDrive = isPioneerDrive;
	PioneerPureReadOffGuard pioneerPureReadGuard(m_drive, isPioneerDrive);
	PioneerPerformanceModeGuard pioneerPerfGuard(m_drive, isPioneerDrive);

	bool usePlextor = m_drive.SupportsQCheck();
	bool usePioneer = false;
	bool useLiteOn = false;

	if (!usePlextor) {
		usePioneer = m_drive.SupportsPioneerScan();
		if (!usePioneer)
			useLiteOn = m_drive.SupportsLiteOnScan();
	}

	if (usePlextor || usePioneer || useLiteOn) {
		c1Result.supported = true;
		c1Result.scanMethod = usePlextor
			? "Plextor Q-Check (0xE9/0xEB)"
			: usePioneer ? "Pioneer (0x3B/0x3C)"
			: "LiteOn/MediaTek";
		c1Result.totalSectors = lastLBA - firstLBA + 1;
		c1Result.totalSeconds = (c1Result.totalSectors + 74) / 75;

		std::cout << "Phase 0: C1 quality scan (early degradation detection)...\n";
		std::cout << "  Method: " << c1Result.scanMethod << "\n";

		m_drive.SetSpeed(scanSpeed);

		bool started = usePlextor
			? m_drive.PlextorQCheckStart(firstLBA, lastLBA)
			: usePioneer ? m_drive.PioneerScanStart(firstLBA, lastLBA)
			: m_drive.LiteOnScanStart(firstLBA, lastLBA);

		if (started) {
			bool scanDone = false;
			bool c1Cancelled = false;
			bool c1Failed = false;
			std::string c1FailureReason;
			int sampleIndex = 0;
			DWORD lastReportedLBA = DWORD(-1);
			auto lastLBAProgress = std::chrono::steady_clock::now();
			DWORD progressLBA = DWORD(-1);
			constexpr auto QCHECK_STALL_TIMEOUT = std::chrono::seconds(30);

			ProgressIndicator c1Progress(40);
			c1Progress.SetLabel("  C1 Scan");
			c1Progress.Start();

			while (!scanDone) {
				if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
					c1Cancelled = true;
					break;
				}

				if (usePlextor)
					std::this_thread::sleep_for(std::chrono::milliseconds(500));

				int c1 = 0, c2 = 0, cu = 0;
				DWORD currentLBA = 0;

				bool pollOk = usePlextor
					? m_drive.PlextorQCheckPoll(c1, c2, cu, currentLBA, scanDone)
					: usePioneer
					? m_drive.PioneerScanPoll(c1, c2, cu, currentLBA, scanDone)
					: m_drive.LiteOnScanPoll(c1, c2, cu, currentLBA, scanDone);

				if (!pollOk && (usePlextor || usePioneer)) {
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
					pollOk = usePlextor
						? m_drive.PlextorQCheckPoll(c1, c2, cu, currentLBA, scanDone)
						: m_drive.PioneerScanPoll(c1, c2, cu, currentLBA, scanDone);
				}
				if (!pollOk) {
					c1Failed = true;
					std::ostringstream reason;
					reason << "lost communication";
					if (lastReportedLBA != DWORD(-1)) {
						const double coverage = c1Result.totalSectors > 0 &&
							lastReportedLBA >= firstLBA
							? std::min(100.0,
								static_cast<double>(lastReportedLBA - firstLBA + 1) *
								100.0 / c1Result.totalSectors)
							: 0.0;
						reason << " at LBA " << lastReportedLBA << " ("
							<< std::fixed << std::setprecision(1) << coverage
							<< "% coverage)";
					}
					c1FailureReason = reason.str();
					break;
				}

				// Match Q-Check's completion ordering: detect LiteOn's positional
				// end marker before filtering empty, duplicate, or startup samples.
				if (useLiteOn && currentLBA >= lastLBA)
					scanDone = true;

				auto pollTime = std::chrono::steady_clock::now();
				if (scanDone || currentLBA != progressLBA) {
					progressLBA = currentLBA;
					lastLBAProgress = pollTime;
				}
				else if (pollTime - lastLBAProgress >= QCHECK_STALL_TIMEOUT) {
					c1Failed = true;
					c1FailureReason = "stalled for 30 seconds at LBA " +
						std::to_string(currentLBA);
					break;
				}
				if (!usePioneer && currentLBA == 0 && c1 == 0 && c2 == 0 && cu == 0 && !scanDone) continue;
				if (currentLBA == lastReportedLBA && !scanDone) continue;
				lastReportedLBA = currentLBA;

				// Match Q-Check: discard the first 3 startup/seek-settle samples.
				if (sampleIndex < 3 && !scanDone) { sampleIndex++; continue; }

				QCheckSample sample;
				sample.lba = currentLBA;
				sample.c1 = c1;
				// Pioneer reports E22 here, not verified C2/E32. Keep it in the
				// diagnostic field so it cannot masquerade as a copy error.
				if (usePioneer)
					sample.pioneerE22 = c2;
				else
					sample.c2 = c2;
				sample.cu = cu;
				c1Result.samples.push_back(sample);
				c1Result.totalC1 += sample.c1;
				c1Result.totalC2 += sample.c2;
				c1Result.totalCU += sample.cu;
				c1Result.totalPioneerE22 += sample.pioneerE22;
				int idx = static_cast<int>(c1Result.samples.size()) - 1;
				if (c1 > c1Result.maxC1PerSecond) {
					c1Result.maxC1PerSecond = c1;
					c1Result.maxC1SecondIndex = idx;
				}
				if (sample.c2 > c1Result.maxC2PerSecond) {
					c1Result.maxC2PerSecond = sample.c2;
					c1Result.maxC2SecondIndex = idx;
				}
				if (cu > c1Result.maxCUPerSecond)
					c1Result.maxCUPerSecond = cu;
				if (sample.pioneerE22 > c1Result.maxPioneerE22PerSecond) {
					c1Result.maxPioneerE22PerSecond = sample.pioneerE22;
					c1Result.maxPioneerE22SecondIndex = idx;
				}
				sampleIndex++;

				if (currentLBA >= firstLBA) {
					int done = static_cast<int>(std::min<DWORD>(
						currentLBA - firstLBA + 1, c1Result.totalSectors));
					c1Progress.Update(done,
						static_cast<int>(c1Result.totalSectors));
				}
			}

			c1Progress.Finish(!c1Cancelled && !c1Failed,
				static_cast<int>(c1Result.totalSectors));

			if (usePlextor) m_drive.PlextorQCheckStop();
			else if (usePioneer) m_drive.PioneerScanStop();
			else m_drive.LiteOnScanStop();

			if (c1Cancelled) {
				m_drive.SetSpeed(0);
				std::cout << "*** Disc rot scan cancelled ***\n";
				return false;
			}
			if (c1Failed) {
				std::cout << "  C1 quality scan " << c1FailureReason
					<< "; partial samples discarded.\n";
				c1Result.samples.clear();
			}

			bool spikesTrimmed = false;
			if (c1Result.samples.size() > 50) {
				std::vector<int> allErrs;
				allErrs.reserve(c1Result.samples.size());
				for (const auto& s : c1Result.samples)
					allErrs.push_back(s.c1 + s.c2 + s.cu + s.pioneerE22);
				std::sort(allErrs.begin(), allErrs.end());
				int median = allErrs[allErrs.size() / 2];

				size_t checkEnd = std::min<size_t>(30, c1Result.samples.size() / 2);
				for (int i = static_cast<int>(checkEnd) - 1; i >= 0; i--) {
					int err = c1Result.samples[i].c1 + c1Result.samples[i].c2
						+ c1Result.samples[i].cu + c1Result.samples[i].pioneerE22;
					if ((median == 0 && err > 10) || (median > 0 && err > median * 10)) {
						c1Result.samples.erase(c1Result.samples.begin() + i);
						spikesTrimmed = true;
					}
				}
			}

			RecalculateQCheckTotals(c1Result);
			if (spikesTrimmed)
				std::cout << "  Startup spike(s) trimmed from quality scan.\n";

			hasC1 = !c1Result.samples.empty();
			if (hasC1)
				std::cout << "\r  C1 scan complete: " << c1Result.samples.size()
				<< " samples, avg C1=" << std::fixed << std::setprecision(1)
				<< c1Result.avgC1PerSecond << "/sec\n\n";
			// Pioneer E22 remains a separate early-warning diagnostic. Verified
			// C2/E32 and CU are not inferred from it.
			if (usePioneer && hasC1) {
				result.pioneerQualityScanRun = true;
				result.pioneerE22Total = c1Result.totalPioneerE22;
				result.pioneerE22AvgPerSecond = c1Result.avgPioneerE22PerSecond;
				result.pioneerE22Peak = c1Result.maxPioneerE22PerSecond;
				result.pioneerE22Rating = PioneerE22Rating(
					result.pioneerE22Total,
					result.pioneerE22AvgPerSecond,
					result.pioneerE22Peak);
			}
		}
	}
	else {
		std::cout << "  (C1 scan not available - drive lacks quality scan support)\n";
		std::cout << "  (Disc rot detection limited to C2 errors only)\n\n";
	}

	// ── Phase 0b: Pioneer uncorrectable cross-check (CD Check 0xE6) ───
	// On Pioneer the vendor scan (Phase 0) has no CU counter and the per-sector
	// READ CD C2 area (Phase 1 below) reads all-zero, so uncorrectable data would
	// otherwise be invisible here. The CD Check protocol measures it directly.
	// Data loss is the strongest rot signal, so a non-zero result escalates the
	// verdict after the pattern analysis. Fast no-op on firmware that dropped the
	// protocol (e.g. BDR-S13U).
	//
	// E22 and E32 are distinct outcomes, so a clean E22/C1 profile cannot prove
	// that CU is absent. Always attempt the cross-check; unsupported firmware
	// such as BDR-S13U rejects the start command quickly and remains unmeasured.
	if (usePioneer && !g_interrupt.IsInterrupted()) {
		if (RunPioneerCdCheckCrosscheck(disc, c1Result)) {
			result.pioneerCdCheckRun = true;
			result.pioneerCdCheckC1Frames = c1Result.pioneerCdCheckC1Frames;
			result.pioneerCdCheckC2Bytes = c1Result.pioneerCdCheckC2Bytes;
		}
		if (g_interrupt.IsInterrupted()) {
			m_drive.SetSpeed(0);
			std::cout << "\n*** Disc rot scan cancelled during CD Check cross-check ***\n";
			return false;
		}
		std::cout << "\n";
	}

	std::cout << "Phase 1: C2 error distribution scan...\n";
	m_drive.SetSpeed(scanSpeed);

	ProgressIndicator progress(40);
	progress.SetLabel("  C2 Scan");
	progress.Start();

	ScsiDrive::C2ReadOptions c2Opts;
	c2Opts.countBytes = true;

	DWORD scannedSectors = 0;
	int maxC2InSector = 0;
	const int c2BucketCount = std::max(1, static_cast<int>((totalSectors + 74) / 75));
	std::vector<int> discRotC2PerSecond(c2BucketCount, 0);
	std::vector<int> discRotReadFailuresPerSecond(c2BucketCount, 0);
	int pioneerTransientC2 = 0;
	int pioneerRecoveredReadFailures = 0;
	if (isPioneerDrive) {
		std::cout << "  [Pioneer] C2-positive sectors will be verified with a second read.\n";
	}
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				return false;
			}

			std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
			size_t secIdx = std::min<size_t>(
				static_cast<size_t>(scannedSectors / 75), discRotC2PerSecond.size() - 1);
			int c2Errors = 0;
			bool readOk = m_drive.ReadSectorWithC2Ex(lba, buf.data(), nullptr, c2Errors, nullptr, c2Opts);
			if (!readOk && isPioneerDrive) {
				DefeatDriveCache(lba, lastLBA);
				readOk = m_drive.ReadSectorWithC2Ex(lba, buf.data(), nullptr, c2Errors, nullptr, c2Opts);
				if (readOk)
					pioneerRecoveredReadFailures++;
			}
			if (readOk) {
				if (isPioneerDrive && c2Errors > 0) {
					DefeatDriveCache(lba, lastLBA);
					std::vector<BYTE> verifyBuf(AUDIO_SECTOR_SIZE);
					int verifyC2 = 0;
					if (m_drive.ReadSectorWithC2Ex(lba, verifyBuf.data(), nullptr, verifyC2, nullptr, c2Opts)) {
						if (verifyC2 == 0) {
							c2Errors = 0;
							pioneerTransientC2++;
						}
						else {
							c2Errors = std::max(c2Errors, verifyC2);
						}
					}
				}
				ClassifyZone(lba, firstLBA, lastLBA, c2Errors > 0 ? 1 : 0, result.zones);
				if (c2Errors > 0) {
					discRotC2PerSecond[secIdx] += c2Errors;
					errorLBAs.push_back(lba);
					if (c2Errors > maxC2InSector)
						maxC2InSector = c2Errors;
				}
			}
			else {
				discRotReadFailuresPerSecond[secIdx]++;
				ClassifyZone(lba, firstLBA, lastLBA, 1, result.zones);
				errorLBAs.push_back(lba);
			}

			scannedSectors++;
			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}
	progress.Finish(true);
	if (pioneerTransientC2 > 0) {
		std::cout << "  [Pioneer] Ignored " << pioneerTransientC2
			<< " transient C2 sector" << (pioneerTransientC2 == 1 ? "" : "s")
			<< " not reproduced on verification read.\n";
	}
	if (pioneerRecoveredReadFailures > 0) {
		std::cout << "  [Pioneer] Recovered " << pioneerRecoveredReadFailures
			<< " transient read failure" << (pioneerRecoveredReadFailures == 1 ? "" : "s")
			<< " on verification read.\n";
	}

	std::sort(errorLBAs.begin(), errorLBAs.end());
	DetectErrorClusters(errorLBAs, result.clusters, scanSpeed);
	result.maxC2InSingleSector = maxC2InSector;

	// Adaptive Zone-Based Sampling
	std::cout << "\nPhase 2: Adaptive read consistency check...\n";

	double innerRate = result.zones.InnerErrorRate();
	double middleRate = result.zones.MiddleErrorRate();
	double outerRate = result.zones.OuterErrorRate();

	auto calcSampleInterval = [](double errorRate) -> int {
		if (errorRate > 2.0) return 20;
		if (errorRate > 0.5) return 50;
		if (errorRate > 0.1) return 100;
		return 200;
		};

	int innerInterval = calcSampleInterval(innerRate);
	int middleInterval = calcSampleInterval(middleRate);
	int outerInterval = calcSampleInterval(outerRate);

	int expectedSamples = 0;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			DWORD range = lastLBA - firstLBA;
			DWORD pos = lba - firstLBA;
			double pct = range > 0 ? static_cast<double>(pos) / range : 0;
			int sampleInterval = 200;
			if (pct < 0.33) sampleInterval = innerInterval;
			else if (pct < 0.66) sampleInterval = middleInterval;
			else sampleInterval = outerInterval;
			if ((lba - start) % sampleInterval == 0) expectedSamples++;
		}
	}

	int samplesChecked = 0;
	int inconsistentSamples = 0;

	progress.SetLabel("  Adaptive Check");
	progress.Start();

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				break;
			}

			DWORD range = lastLBA - firstLBA;
			DWORD pos = lba - firstLBA;
			double pct = range > 0 ? static_cast<double>(pos) / range : 0;
			int sampleInterval = 200;

			if (pct < 0.33) sampleInterval = innerInterval;
			else if (pct < 0.66) sampleInterval = middleInterval;
			else sampleInterval = outerInterval;

			if ((lba - start) % sampleInterval != 0) continue;

			int inconsistent = 0;
			if (TestReadConsistency(lba, 3, inconsistent, scanSpeed)) {
				samplesChecked++;
				if (inconsistent > 0) {
					inconsistentSamples++;
					inconsistentLBAs.push_back(lba);
				}
			}

			progress.Update(samplesChecked, expectedSamples);
		}
	}
	progress.Finish(true);

	result.totalRereadTests = samplesChecked;
	result.inconsistentSectors = inconsistentSamples;
	result.inconsistencyRate = samplesChecked > 0
		? static_cast<double>(inconsistentSamples) / samplesChecked * 100.0 : 0;

	m_drive.SetSpeed(0);

	AnalyzeErrorPatterns(errorLBAs, result);

	// ── Factor C1 data into rot assessment ───────────────────────────
	if (hasC1) {
		AnalyzeC1RotPatterns(c1Result, firstLBA, lastLBA, result);
	}

	// ── Factor the Pioneer CD Check cross-check into the verdict ──────
	// Uncorrectable bytes are actual data loss — the strongest rot signal — so
	// force the risk to at least HIGH (never downgrade an existing CRITICAL).
	if (result.pioneerCdCheckRun && result.pioneerCdCheckC2Bytes > 0
		&& result.rotRiskLevel != "CRITICAL") {
		result.rotRiskLevel = "HIGH";
	}

	if (result.rotRiskLevel == "NONE") {
		result.recommendation = "Disc appears healthy. Store properly to prevent future damage.";
	}
	else if (result.rotRiskLevel == "LOW") {
		result.recommendation = "Minor issues detected. Consider backing up soon.";
	}
	else if (result.rotRiskLevel == "MODERATE") {
		result.recommendation = "Disc showing early degradation signs. Back up immediately.";
	}
	else if (result.rotRiskLevel == "HIGH") {
		result.recommendation = "Significant degradation detected. Back up NOW - data loss likely.";
	}
	else {
		result.recommendation = "CRITICAL damage! Extract whatever data possible immediately.";
	}
	if (result.pioneerDrive && !result.pioneerCdCheckRun) {
		result.recommendation += " Pioneer CU/E32 was not measured on this firmware; verify any rip independently.";
	}

	PrintDiscRotReport(result);

	// Print C1 graph if available
	if (hasC1 && !c1Result.samples.empty()) {
		// Convert samples to a flat int vector for BucketData
		std::vector<int> c1Values;
		c1Values.reserve(c1Result.samples.size());
		for (const auto& s : c1Result.samples)
			c1Values.push_back(s.c1);

		int maxC1 = 1;
		for (int v : c1Values)
			if (v > maxC1) maxC1 = v;

		// Ensure the chart is tall enough to show the Red Book reference line
		if (maxC1 < 250) maxC1 = 250;

		Console::GraphOptions c1Opts;
		c1Opts.title = "C1 Quality Profile";
		c1Opts.subtitle = "C1 = corrected errors - early warning for degradation";
		c1Opts.width = 60;
		c1Opts.height = 10;
		c1Opts.refLine = 220;
		c1Opts.refLabel = "Red Book limit (220/sec)";
		c1Opts.colorize = true;

		auto buckets = Console::BucketData(c1Values, c1Opts.width);
		Console::DrawBarGraph(buckets, maxC1, c1Opts,
			static_cast<DWORD>(c1Result.samples.size()));

		// Pioneer E22 uses the same source samples as the numeric summary. Show
		// it separately from C1 and C2 because it is diagnostic-only.
		if (usePioneer) {
			std::vector<int> e22Values;
			e22Values.reserve(c1Result.samples.size());
			int peakE22 = 0;
			for (const auto& s : c1Result.samples) {
				e22Values.push_back(s.pioneerE22);
				peakE22 = std::max(peakE22, s.pioneerE22);
			}
			if (peakE22 > 0) {
				Console::GraphOptions e22Opts;
				e22Opts.title = "Disc Rot Pioneer E22 Profile (Diagnostic Only)";
				e22Opts.subtitle = "E22 distribution used for early-warning pattern analysis; not C2/CU";
				e22Opts.width = 60;
				e22Opts.height = 10;
				e22Opts.unitSuffix = "/sec";
				e22Opts.severityLowThreshold = 25;
				e22Opts.severityHighThreshold = 100;
				e22Opts.severityLowLabel = "1-24/sec low";
				e22Opts.severityModerateLabel = "25-99/sec elevated";
				e22Opts.severityHighLabel = "100+/sec heavy";
				auto e22Buckets = Console::BucketData(e22Values, e22Opts.width);
				Console::DrawBarGraph(e22Buckets, std::max(peakE22, 100), e22Opts,
					static_cast<DWORD>(c1Result.samples.size()));
			}
		}
		else {
			std::cout << "  C1 quality scan could not start; continuing with "
				"the independent C2 and consistency phases.\n";
		}
	}

	// Phase 1 distribution: show retained positive C2 activity (Pioneer
	// positives are re-read when possible). A zero-only
	// Pioneer bitmap is deliberately not rendered as a green clean graph.
	int peakDiscRotC2 = *std::max_element(discRotC2PerSecond.begin(), discRotC2PerSecond.end());
	if (peakDiscRotC2 > 0) {
		Console::GraphOptions c2Opts;
		c2Opts.title = "Disc Rot C2 Distribution";
		c2Opts.subtitle = "Retained C2 error-pointer activity per second during Phase 1";
		c2Opts.width = 60;
		c2Opts.height = 10;
		c2Opts.unitSuffix = "/sec";
		c2Opts.severityLowThreshold = 5;
		c2Opts.severityHighThreshold = 20;
		c2Opts.severityLowLabel = "1-4/sec low";
		c2Opts.severityModerateLabel = "5-19/sec moderate";
		c2Opts.severityHighLabel = "20+/sec high";
		auto c2Buckets = Console::BucketData(discRotC2PerSecond, c2Opts.width);
		Console::DrawBarGraph(c2Buckets, peakDiscRotC2, c2Opts,
			static_cast<DWORD>(discRotC2PerSecond.size()));
	}
	else if (isPioneerDrive) {
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
		std::cout << "\n  Disc Rot C2 graph omitted: Pioneer returned a zero-only READ CD C2 series;\n"
			<< "  that bitmap is not trusted as proof of a clean disc.\n";
		Console::Reset();
	}
	else {
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
		std::cout << "\n  " << Console::Sym::Check << " No C2 activity during Disc Rot Phase 1; graph omitted.\n";
		Console::Reset();
	}

	int peakReadFailures = *std::max_element(
		discRotReadFailuresPerSecond.begin(), discRotReadFailuresPerSecond.end());
	if (peakReadFailures > 0) {
		Console::GraphOptions failOpts;
		failOpts.title = "Disc Rot Read-Failure Distribution";
		failOpts.subtitle = "Failed sector reads per second; kept separate from measured C2";
		failOpts.width = 60;
		failOpts.height = 8;
		failOpts.unitSuffix = "/sec";
		auto failBuckets = Console::BucketData(discRotReadFailuresPerSecond, failOpts.width);
		Console::DrawBarGraph(failBuckets, peakReadFailures, failOpts,
			static_cast<DWORD>(discRotReadFailuresPerSecond.size()));
	}

	return true;
}

// ============================================================================
// Helper Functions
// ============================================================================

bool OpticalDrive::TestReadConsistency(DWORD lba, int passes, int& mismatchCount, int readSpeed) {
	mismatchCount = 0;
	if (passes < 2) return true;

	// Set consistent read speed for all consistency checks to ensure
	// reproducible results; speed variations can affect read stability.
	m_drive.SetSpeed(readSpeed);

	std::vector<BYTE> reference(AUDIO_SECTOR_SIZE);
	if (!m_drive.ReadSectorAudioOnly(lba, reference.data()))
		return false;

	std::vector<BYTE> compare(AUDIO_SECTOR_SIZE);
	for (int i = 1; i < passes; i++) {
		// Without Accurate Stream, the drive may return cached data instead
		// of re-reading from the disc, hiding genuine read inconsistencies
		if (!m_hasAccurateStream) {
			DefeatDriveCache(lba, 0);
		}

		if (!m_drive.ReadSectorAudioOnly(lba, compare.data()))
			return false;
		if (memcmp(reference.data(), compare.data(), AUDIO_SECTOR_SIZE) != 0)
			mismatchCount++;
	}
	return true;
}

void OpticalDrive::ClassifyZone(DWORD lba, DWORD totalStart, ULONG totalEnd,
	int hasError, DiscZoneStats& zones) {
	DWORD range = totalEnd - totalStart;
	if (range == 0) return;

	DWORD relative = lba - totalStart;
	double position = static_cast<double>(relative) / range;

	if (position < 0.33) {
		zones.innerSectors++;
		zones.innerErrors += hasError;
	}
	else if (position < 0.66) {
		zones.middleSectors++;
		zones.middleErrors += hasError;
	}
	else {
		zones.outerSectors++;
		zones.outerErrors += hasError;
	}
}

int OpticalDrive::CalculateClusterTolerance(int scanSpeed) {
	// scanSpeed typically ranges from 1-48x
	// Map to tolerance: slower speeds = tighter clusters (lower tolerance)
	//                   faster speeds = scattered sectors (higher tolerance)
	if (scanSpeed >= 40) return 8;      // Very fast: 8-sector window
	if (scanSpeed >= 24) return 7;      // Fast: 7-sector window
	if (scanSpeed >= 16) return 6;      // Medium-fast: 6-sector window
	if (scanSpeed >= 8) return 5;       // Medium: 5-sector window
	if (scanSpeed >= 4) return 4;       // Medium-slow: 4-sector window
	return 3;                           // Slow: 3-sector window (tight clustering)
}

void OpticalDrive::DetectErrorClusters(const std::vector<DWORD>& errorLBAs,
	std::vector<ErrorCluster>& clusters, int scanSpeed) {
	clusters.clear();
	if (errorLBAs.empty()) return;

	// Defensive: callers should pass sorted LBAs, but normalize here to avoid silent misclustering.
	std::vector<DWORD> sortedLBAs = errorLBAs;
	if (!std::is_sorted(sortedLBAs.begin(), sortedLBAs.end())) {
		std::sort(sortedLBAs.begin(), sortedLBAs.end());
	}

	int tolerance = CalculateClusterTolerance(scanSpeed);

	ErrorCluster current;
	current.startLBA = sortedLBAs[0];
	current.endLBA = sortedLBAs[0];
	current.errorCount = 1;

	for (size_t i = 1; i < sortedLBAs.size(); i++) {
		const DWORD next = sortedLBAs[i];

		// Overflow-safe window test.
		const bool inWindow =
			(next >= current.endLBA) &&
			(static_cast<uint64_t>(next) - static_cast<uint64_t>(current.endLBA) <=
				static_cast<uint64_t>(tolerance));

		// If the next error is within the adaptive tolerance window, extend the current cluster
		if (inWindow) {
			current.endLBA = next;
			current.errorCount++;
		}
		else {
			// Gap exceeds tolerance; finalize current cluster and start a new one
			clusters.push_back(current);
			current.startLBA = next;
			current.endLBA = next;
			current.errorCount = 1;
		}
	}
	// Don't forget the last cluster
	clusters.push_back(current);
}

void OpticalDrive::AnalyzeErrorPatterns(const std::vector<DWORD>& errorLBAs,
	DiscRotAnalysis& analysis) {
	if (errorLBAs.empty()) {
		analysis.rotRiskLevel = "NONE";
		return;
	}

	if (analysis.zones.outerSectors > 0 && analysis.zones.innerSectors > 0) {
		double outerRate = analysis.zones.OuterErrorRate();
		double innerRate = analysis.zones.InnerErrorRate();
		analysis.edgeConcentration =
			(outerRate > innerRate * 2.0 && outerRate > 1.0) ||
			(innerRate > outerRate * 2.0 && innerRate > 1.0);
	}

	analysis.progressivePattern =
		analysis.zones.InnerErrorRate() < analysis.zones.MiddleErrorRate() &&
		analysis.zones.MiddleErrorRate() < analysis.zones.OuterErrorRate() &&
		analysis.zones.OuterErrorRate() > 0.5;

	int smallClusters = 0;
	for (const auto& c : analysis.clusters) {
		if (c.size() <= 3) smallClusters++;
	}
	analysis.pinholePattern = (smallClusters > 10) &&
		(smallClusters > static_cast<int>(analysis.clusters.size()) / 2);

	analysis.readInstability = analysis.inconsistencyRate > 5.0;

	analysis.rotRiskLevel = AssessRotRisk(analysis);
}

std::string OpticalDrive::AssessRotRisk(const DiscRotAnalysis& analysis) {
	int score = 0;

	if (analysis.edgeConcentration) score += 25;
	if (analysis.progressivePattern) score += 25;
	if (analysis.pinholePattern) score += 15;
	if (analysis.readInstability) score += 20;
	if (analysis.inconsistencyRate > 10.0) score += 15;

	// Severe C2 errors in a single sector indicate physical damage
	if (analysis.maxC2InSingleSector >= 100) score += 20;
	else if (analysis.maxC2InSingleSector >= 50) score += 10;

	if (score >= 75) return "CRITICAL";
	if (score >= 50) return "HIGH";
	if (score >= 30) return "MODERATE";
	if (score >= 10) return "LOW";
	return "NONE";
}

void OpticalDrive::PrintDiscRotReport(const DiscRotAnalysis& analysis) {
	using namespace Console;

	std::cout << "\n";
	SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
	std::cout << Sym::TopLeft;
	for (int i = 0; i < 58; i++) std::cout << Sym::Horizontal;
	std::cout << Sym::TopRight << "\n";
	Heading("  DISC ROT ANALYSIS REPORT\n");
	SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
	std::cout << Sym::BottomLeft;
	for (int i = 0; i < 58; i++) std::cout << Sym::Horizontal;
	std::cout << Sym::BottomRight << "\n";
	Reset();

	std::cout << "\n";
	Heading("  Zone Error Rates\n");
	SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
	std::cout << "  (Disc surface divided into three radial zones)\n\n";
	Reset();

	auto printZone = [](const char* label, double rate, int errors, int sectors) {
		using namespace Console;

		// Fill scales with rate (capped at 10% = full bar). Severity scales
		// with rate too, but uses a finer-grained scale (5% = full red).
		double fillFrac = std::min(1.0, rate / 10.0);
		double severity = std::min(1.0, rate / 5.0);

		std::ostringstream suffix;
		suffix << std::fixed << std::setprecision(2) << rate << "%  ("
			<< errors << "/" << sectors << ")  ";
		if (rate > 5.0) suffix << Sym::Cross << " severe";
		else if (rate > 1.0) suffix << Sym::Warn << " moderate";
		else suffix << Sym::Check << " healthy";

		DrawScoreBar(label, fillFrac, severity, 30, suffix.str());
		};

	printZone("Inner  (0-33%):  ", analysis.zones.InnerErrorRate(),
		analysis.zones.innerErrors, analysis.zones.innerSectors);
	printZone("Middle (33-66%): ", analysis.zones.MiddleErrorRate(),
		analysis.zones.middleErrors, analysis.zones.middleSectors);
	printZone("Outer  (66-100%):", analysis.zones.OuterErrorRate(),
		analysis.zones.outerErrors, analysis.zones.outerSectors);

	std::cout << "\n";
	Heading("  Error Clusters\n");
	Reset();
	std::cout << "  Total clusters:  " << analysis.clusters.size() << "\n";
	if (!analysis.clusters.empty()) {
		int maxSize = 0;
		for (const auto& c : analysis.clusters)
			if (c.size() > maxSize) maxSize = c.size();
		std::cout << "  Largest cluster: " << maxSize << " sectors";
		if (maxSize > 100) { Error("  (severe - large contiguous damage)"); }
		else if (maxSize > 20) { Warning("  (moderate - localized damage)"); }
		else { Success("  (minor - small scratch or defect)"); }
		std::cout << "\n";
	}

	std::cout << "\n";
	Heading("  Disc Rot Indicators\n");
	Reset();

	auto indicator = [](bool v, const char* yesExplain, const char* noExplain) {
		using namespace Console;
		if (v) {
			SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
			std::cout << Sym::Cross << " YES  - " << yesExplain;
		}
		else {
			SetColorRGB(Theme::GreenR, Theme::GreenG, Theme::GreenB);
			std::cout << Sym::Check << " NO   - " << noExplain;
		}
		Reset();
		std::cout << "\n";
		};

	std::cout << "  Edge concentration:  ";
	indicator(analysis.edgeConcentration,
		"Errors concentrated at disc edges (classic rot pattern)",
		"Errors not edge-concentrated");
	std::cout << "  Progressive pattern: ";
	indicator(analysis.progressivePattern,
		"Error rate increases toward outer edge (spreading damage)",
		"No progressive error increase");
	std::cout << "  Pinhole pattern:     ";
	indicator(analysis.pinholePattern,
		"Small scattered error spots (early-stage pitting)",
		"No pinhole defects detected");
	std::cout << "  Read instability:    ";
	if (analysis.readInstability) {
		SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
		std::cout << Sym::Cross << " YES  - Same sectors return different data on re-read ("
			<< static_cast<int>(analysis.inconsistencyRate) << "% unstable)\n";
	}
	else {
		SetColorRGB(Theme::GreenR, Theme::GreenG, Theme::GreenB);
		std::cout << Sym::Check << " NO   - Reads are consistent across re-reads\n";
	}
	Reset();

	if (analysis.pioneerQualityScanRun) {
		std::cout << "\n  Pioneer E22:        " << analysis.pioneerE22Total << " total, "
			<< std::fixed << std::setprecision(2) << analysis.pioneerE22AvgPerSecond
			<< "/sec avg, " << analysis.pioneerE22Peak << "/sec peak"
			<< " [" << analysis.pioneerE22Rating << "]\n";
		std::cout << "                       Diagnostic only; E22 is not a verified C2/CU result.\n";
	}

	// Pioneer CD Check (0xE6) uncorrectable cross-check — real data-loss signal
	// that the vendor scan and per-sector C2 can't see on Pioneer drives.
	if (analysis.pioneerDrive) {
		std::cout << "  Uncorrectable:       ";
		if (!analysis.pioneerCdCheckRun) {
			SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
			std::cout << "NOT MEASURED - Pioneer CD Check unavailable or incomplete; CU/E32 unknown\n";
		}
		else if (analysis.pioneerCdCheckC2Bytes > 0) {
			SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
			std::cout << Sym::Cross << " YES  - " << analysis.pioneerCdCheckC2Bytes
				<< " C2-uncorrectable byte(s), worst window (Pioneer CD Check) - data loss\n";
		}
		else {
			SetColorRGB(Theme::GreenR, Theme::GreenG, Theme::GreenB);
			std::cout << Sym::Check << " NO   - No uncorrectable data reported (Pioneer CD Check 0xE6)\n";
		}
		Reset();
	}

	std::cout << "\n";
	Heading("  Risk Assessment\n");
	Reset();
	std::cout << "  Disc Rot Risk: ";
	if (analysis.rotRiskLevel == "CRITICAL" || analysis.rotRiskLevel == "HIGH")
		SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
	else if (analysis.rotRiskLevel == "MODERATE")
		SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
	else
		SetColorRGB(Theme::GreenR, Theme::GreenG, Theme::GreenB);
	std::cout << "\033[1m" << analysis.rotRiskLevel << "\033[22m\n";
	Reset();

	if (!analysis.recommendation.empty()) {
		SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
		std::cout << "\n  " << Sym::Arrow << " " << analysis.recommendation << "\n";
		Reset();
	}

	SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
	std::cout << Sym::BottomLeft;
	for (int i = 0; i < 58; i++) std::cout << Sym::Horizontal;
	std::cout << Sym::BottomRight << "\n";
	Reset();
}

bool OpticalDrive::SaveDiscRotLog(const DiscRotAnalysis& analysis, const std::wstring& path) {
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f)
		return false;

	fprintf(f, "# ==============================\n");
	fprintf(f, "# Disc Rot Analysis Report\n");
	fprintf(f, "# ==============================\n");
	fprintf(f, "#\n");
	fprintf(f, "# Risk Level:            %s\n", analysis.rotRiskLevel.c_str());
	if (!analysis.recommendation.empty())
		fprintf(f, "# Recommendation:        %s\n", analysis.recommendation.c_str());
	fprintf(f, "#\n");

	fprintf(f, "# --- Zone Error Rates ---\n");
	fprintf(f, "# Inner  (0-33%%%%):       %.2f%% (%d/%d)\n",
		analysis.zones.InnerErrorRate(), analysis.zones.innerErrors, analysis.zones.innerSectors);
	fprintf(f, "# Middle (33-66%%%%):      %.2f%% (%d/%d)\n",
		analysis.zones.MiddleErrorRate(), analysis.zones.middleErrors, analysis.zones.middleSectors);
	fprintf(f, "# Outer  (66-100%%%%):     %.2f%% (%d/%d)\n",
		analysis.zones.OuterErrorRate(), analysis.zones.outerErrors, analysis.zones.outerSectors);
	fprintf(f, "#\n");

	fprintf(f, "# --- Read Consistency ---\n");
	fprintf(f, "# Inconsistent Sectors:  %d / %d tested\n",
		analysis.inconsistentSectors, analysis.totalRereadTests);
	fprintf(f, "# Inconsistency Rate:    %.2f%%\n", analysis.inconsistencyRate);
	fprintf(f, "#\n");

	fprintf(f, "# --- Disc Rot Indicators ---\n");
	fprintf(f, "# Edge Concentration:    %s\n", analysis.edgeConcentration ? "YES" : "NO");
	fprintf(f, "# Progressive Pattern:   %s\n", analysis.progressivePattern ? "YES" : "NO");
	fprintf(f, "# Pinhole Pattern:       %s\n", analysis.pinholePattern ? "YES" : "NO");
	fprintf(f, "# Read Instability:      %s\n", analysis.readInstability ? "YES" : "NO");
	if (analysis.pioneerQualityScanRun) {
		fprintf(f, "# Pioneer E22:          %d total, %.2f/sec avg, %d/sec peak [%s] (diagnostic, not C2/CU)\n",
			analysis.pioneerE22Total, analysis.pioneerE22AvgPerSecond,
			analysis.pioneerE22Peak, analysis.pioneerE22Rating.c_str());
	}
	if (analysis.pioneerDrive && !analysis.pioneerCdCheckRun) {
		fprintf(f, "# Uncorrectable (CDChk): NOT MEASURED - CU/E32 unknown\n");
	}
	else if (analysis.pioneerCdCheckRun) {
		// Pioneer CD Check (0xE6) uncorrectable cross-check — real data-loss
		// measurement the vendor scan / per-sector C2 can't provide on Pioneer.
		fprintf(f, "# Uncorrectable (CDChk): %s (C1 uncorr=%d frames, C2 uncorr=%d bytes, worst window)\n",
			analysis.pioneerCdCheckC2Bytes > 0 ? "YES - data loss" : "NO",
			analysis.pioneerCdCheckC1Frames, analysis.pioneerCdCheckC2Bytes);
	}
	fprintf(f, "#\n");

	fprintf(f, "# ==============================\n");
	fprintf(f, "# Zone Summary\n");
	fprintf(f, "# ==============================\n");
	fprintf(f, "Zone,ErrorRate,Errors,TotalSectors\n");
	fprintf(f, "Inner (0-33%%),%.2f,%d,%d\n",
		analysis.zones.InnerErrorRate(), analysis.zones.innerErrors, analysis.zones.innerSectors);
	fprintf(f, "Middle (33-66%%),%.2f,%d,%d\n",
		analysis.zones.MiddleErrorRate(), analysis.zones.middleErrors, analysis.zones.middleSectors);
	fprintf(f, "Outer (66-100%%),%.2f,%d,%d\n",
		analysis.zones.OuterErrorRate(), analysis.zones.outerErrors, analysis.zones.outerSectors);
	fprintf(f, "\n");

	fprintf(f, "# ==============================\n");
	fprintf(f, "# Error Clusters (%zu total)\n", analysis.clusters.size());
	fprintf(f, "# ==============================\n");
	fprintf(f, "ClusterIndex,StartLBA,EndLBA,SectorCount,ErrorCount\n");
	for (size_t i = 0; i < analysis.clusters.size(); i++) {
		const auto& c = analysis.clusters[i];
		fprintf(f, "%zu,%lu,%lu,%d,%d\n",
			i, c.startLBA, c.endLBA, c.size(), c.errorCount);
	}

	fclose(f);
	return true;
}

void OpticalDrive::AnalyzeC1RotPatterns(const QCheckResult& c1Result,
	DWORD firstLBA, DWORD lastLBA, DiscRotAnalysis& analysis) {
	if (c1Result.samples.empty()) return;

	// Guard invalid ordering and avoid unsigned underflow.
	if (lastLBA <= firstLBA) return;
	const uint64_t range = static_cast<uint64_t>(lastLBA) - static_cast<uint64_t>(firstLBA);
	if (range == 0) return;

	// Split C1 data into three zones and compute avg C1 per zone
	double innerC1 = 0, middleC1 = 0, outerC1 = 0;
	int innerN = 0, middleN = 0, outerN = 0;

	for (const auto& s : c1Result.samples) {
		// Ignore out-of-scan-range samples to avoid wrap and bad positioning.
		if (s.lba < firstLBA || s.lba > lastLBA)
			continue;

		const uint64_t pos = static_cast<uint64_t>(s.lba) - static_cast<uint64_t>(firstLBA);
		double posPct = static_cast<double>(pos) / static_cast<double>(range);

		if (posPct < 0.33) { innerC1 += s.c1; innerN++; }
		else if (posPct < 0.66) { middleC1 += s.c1; middleN++; }
		else { outerC1 += s.c1; outerN++; }
	}

	if (innerN == 0 && middleN == 0 && outerN == 0)
		return;

	double avgInner = innerN > 0 ? innerC1 / innerN : 0;
	double avgMiddle = middleN > 0 ? middleC1 / middleN : 0;
	double avgOuter = outerN > 0 ? outerC1 / outerN : 0;

	std::cout << "\n--- C1 Zone Analysis (early warning) ---\n";
	std::cout << "  Inner  avg C1/sec: " << std::fixed << std::setprecision(1) << avgInner << "\n";
	std::cout << "  Middle avg C1/sec: " << avgMiddle << "\n";
	std::cout << "  Outer  avg C1/sec: " << avgOuter << "\n";

	// C1-based rot indicators (these fire BEFORE C2 errors appear)
	bool c1EdgeElevated = (avgOuter > avgInner * 3.0) && (avgOuter > 10.0);
	bool c1Progressive = (avgInner < avgMiddle) && (avgMiddle < avgOuter) && (avgOuter > 10.0);
	bool c1OverallHigh = (c1Result.avgC1PerSecond > 50.0);
	bool c1RedBookFail = (c1Result.avgC1PerSecond >= 220.0);

	if (c1EdgeElevated)
		std::cout << "  ** C1 elevated at outer edge - early disc rot signal **\n";
	if (c1Progressive)
		std::cout << "  ** C1 rising inner->outer - progressive degradation pattern **\n";
	if (c1RedBookFail)
		std::cout << "  ** C1 exceeds Red Book limit - disc is stressed **\n";

	// Boost the rot risk score based on C1 findings
	// These are early warnings that wouldn't show up in C2 alone
	int c1Score = 0;
	if (c1EdgeElevated) c1Score += 15;
	if (c1Progressive) c1Score += 20;
	if (c1OverallHigh) c1Score += 10;
	if (c1RedBookFail) c1Score += 15;

	if (c1Score > 0) {
		// Re-assess with C1 data factored in
		std::string current = analysis.rotRiskLevel;
		if (current == "NONE" && c1Score >= 15)
			analysis.rotRiskLevel = "LOW";
		if (current == "NONE" && c1Score >= 30)
			analysis.rotRiskLevel = "MODERATE";
		if (current == "LOW" && c1Score >= 20)
			analysis.rotRiskLevel = "MODERATE";
		if (current == "MODERATE" && c1Score >= 25)
			analysis.rotRiskLevel = "HIGH";

		if (analysis.rotRiskLevel != current) {
			std::cout << "  Risk level upgraded from " << current
				<< " to " << analysis.rotRiskLevel
				<< " based on C1 early-warning data\n";
		}
	}

	// ── Factor Pioneer E22 diagnostics into the rot verdict ─────────
	// On Pioneer BD burners (e.g. BDR-S13U) the per-sector READ CD C2 path
	// (Phase 1) frequently returns GOOD with an all-zero C2 area. The vendor
	// quality scan's E22 counter is useful as an early-warning diagnostic, but
	// it is not verified C2/E32 and does not establish copyability. Only a
	// sustained, edge-concentrated, or progressive E22 pattern raises rot risk.
	if (c1Result.scanMethod.find("Pioneer") != std::string::npos) {
		double innerE22 = 0, middleE22 = 0, outerE22 = 0;
		int inN = 0, midN = 0, outN = 0;
		for (const auto& s : c1Result.samples) {
			if (s.lba < firstLBA || s.lba > lastLBA) continue;
			const uint64_t pos = static_cast<uint64_t>(s.lba) - static_cast<uint64_t>(firstLBA);
			double posPct = static_cast<double>(pos) / static_cast<double>(range);
			if (posPct < 0.33) { innerE22 += s.pioneerE22; inN++; }
			else if (posPct < 0.66) { middleE22 += s.pioneerE22; midN++; }
			else { outerE22 += s.pioneerE22; outN++; }
		}
		double aInE22 = inN > 0 ? innerE22 / inN : 0;
		double aMidE22 = midN > 0 ? middleE22 / midN : 0;
		double aOutE22 = outN > 0 ? outerE22 / outN : 0;

		if (c1Result.totalPioneerE22 > 0) {
			std::cout << "\n--- E22 Zone Analysis (Pioneer diagnostic) ---\n";
			std::cout << "  Inner  avg E22/sec: " << std::fixed << std::setprecision(1) << aInE22 << "\n";
			std::cout << "  Middle avg E22/sec: " << aMidE22 << "\n";
			std::cout << "  Outer  avg E22/sec: " << aOutE22 << "\n";
			std::cout << "  Total E22: " << c1Result.totalPioneerE22
				<< " (avg " << std::fixed << std::setprecision(2) << c1Result.avgPioneerE22PerSecond
				<< "/sec, peak " << c1Result.maxPioneerE22PerSecond << "/sec)\n";
		}

		bool e22EdgeElevated = (aOutE22 > aInE22 * 3.0) && (aOutE22 > 2.0);
		bool e22Progressive = (aInE22 < aMidE22) && (aMidE22 < aOutE22) && (aOutE22 > 2.0);
		bool e22OverallHigh = (c1Result.avgPioneerE22PerSecond > 1.0);
		bool e22Heavy = (c1Result.avgPioneerE22PerSecond > 5.0);

		if (e22EdgeElevated)
			std::cout << "  ** E22 elevated at outer edge - disc rot signal **\n";
		if (e22Progressive)
			std::cout << "  ** E22 rising inner->outer - progressive degradation **\n";
		if (e22Heavy)
			std::cout << "  ** Sustained heavy E22 - reduced correction margin **\n";

		int e22Score = 0;
		if (e22EdgeElevated) e22Score += 20;
		if (e22Progressive) e22Score += 20;
		if (e22OverallHigh) e22Score += 15;
		if (e22Heavy) e22Score += 20;

		if (e22Score > 0) {
			std::string current = analysis.rotRiskLevel;
			if (current == "NONE" && e22Score >= 15) analysis.rotRiskLevel = "LOW";
			if (current == "NONE" && e22Score >= 30) analysis.rotRiskLevel = "MODERATE";
			if (current == "LOW" && e22Score >= 20) analysis.rotRiskLevel = "MODERATE";
			if (current == "MODERATE" && e22Score >= 25) analysis.rotRiskLevel = "HIGH";
			if (analysis.rotRiskLevel != current)
				std::cout << "  Risk level upgraded from " << current
					<< " to " << analysis.rotRiskLevel
					<< " based on Pioneer E22 diagnostics\n";
		}
	}
}
