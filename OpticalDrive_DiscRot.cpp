#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include "ConsoleColor.h"
#include "ConsoleGraph.h"
#include "QualityScanSession.h"
#include "DiscRotQuality.h"
#include "DiscRotReadConsistency.h"
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
	result.maxC1PerSample = 0;
	result.maxC1SampleIndex = -1;
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
		if (s.c1 > result.maxC1PerSample) {
			result.maxC1PerSample = s.c1;
			result.maxC1SampleIndex = i;
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

	ComputeTimedC1(result);
	const auto c2 = BuildQCheckCounterGraph(result, &QCheckSample::c2);
	const auto e22 = BuildQCheckCounterGraph(result, &QCheckSample::pioneerE22);
	result.avgC2PerSecond = c2.RateAvailable() ? c2.average : 0.0;
	result.avgPioneerE22PerSecond = e22.RateAvailable() ? e22.average : 0.0;
}

// The E22 tier thresholds used to be duplicated here and in
// OpticalDrive_QCheck.cpp. They now come from ScanResults.h / ScanQualityRating
// so the rot scan and the quality scan cannot disagree about the same disc, and
// both judge the sustained level instead of the raw peak.
}  // namespace

bool OpticalDrive::RunDiscRotScan(DiscInfo& disc, DiscRotAnalysis& result, int scanSpeed) {
	// Lock the tray for the multi-phase scan so an accidental eject can't abort it.
	DriveDoorLockGuard doorLock(m_drive);
	std::cout << "\n=== Disc Rot Detection Scan ===\n";
	std::cout << "This scan checks read reliability and heuristic degradation patterns.\n\n";

	DriveCapabilities cacheCaps;
	DetectDriveCapabilities(cacheCaps);

	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: C2 error detection required but not supported.\n";
		return false;
	}

	DWORD firstLBA = 0, lastLBA = 0;
	DWORD totalSectors = 0;
	DiscRot::AudioRanges audioRanges;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) {
			DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
			if (totalSectors == 0) firstLBA = start;
			lastLBA = t.endLBA;
			audioRanges.emplace_back(start, t.endLBA);
			totalSectors += t.endLBA - start + 1;
		}
	}

	if (totalSectors == 0) {
		std::cout << "No audio tracks to scan.\n";
		return false;
	}

	audioRanges = DiscRot::NormalizeAudioRanges(std::move(audioRanges));
	auto cancelled = []() { return g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey(); };
	auto evictAudioCache = [&](DWORD lba) {
		return DiscRot::EvictAudioCache(lba, audioRanges, cacheCaps.bufferSizeKB,
			[&](DWORD start, DWORD count, BYTE* data) { return m_drive.ReadSectorsAudioOnly(start, count, data); },
			cancelled);
	};
	result = DiscRotAnalysis{};
	result.discIdentity = Diagnostics::ScanDiscIdentity(disc);
	result.qualityRequestedSpeed = scanSpeed;
	ScopedDriveSpeed restoreSpeed(m_drive);
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

	if (!m_drive.IsOpen()) { std::cout << "ERROR: Drive closed after scan cleanup failed. Reopen it.\n"; return false; }
	if (usePlextor || usePioneer || useLiteOn) {
		c1Result.supported = true;
		c1Result.scanMethod = usePlextor
			? "Plextor Q-Check (0xE9/0xEB)"
			: usePioneer ? "Pioneer (0x3B/0x3C)"
			: m_drive.LiteOnScanMethodName();
		c1Result.cuMeasured = usePlextor || (useLiteOn && m_drive.LiteOnScanMeasuresCu());
		c1Result.totalSectors = lastLBA - firstLBA + 1;
		c1Result.graphStartLba = firstLBA;
		c1Result.graphSectors = std::uint64_t{lastLBA} - firstLBA + 1;
		c1Result.totalSeconds = (c1Result.totalSectors + 74) / 75;

		std::cout << "Phase 0: C1 quality scan (early degradation detection)...\n";
		std::cout << "  Method: " << c1Result.scanMethod << "\n";

		m_drive.SetSpeed(scanSpeed);

        c1Result.startup.Reset(firstLBA,(std::min)(Diagnostics::kQualityStartupSectors,c1Result.totalSectors));
        c1Result.startupCacheCleared=Diagnostics::PrepareQualityScanCache(m_drive,audioRanges,
            firstLBA,firstLBA+c1Result.startup.plannedSectors-1,cacheCaps.bufferSizeKB,cancelled);
        result.qualityStartup=c1Result.startup;
        result.qualityStartupCacheCleared=c1Result.startupCacheCleared;
        if(cancelled())return false;
		QualityScanSession c1Session([&]() {
			return usePlextor ? m_drive.PlextorQCheckStop()
				: usePioneer ? m_drive.PioneerScanStop() : m_drive.LiteOnScanStop();
		});
		bool started = usePlextor
			? m_drive.PlextorQCheckStart(firstLBA, lastLBA)
			: usePioneer ? m_drive.PioneerScanStart(firstLBA, lastLBA)
			: m_drive.LiteOnScanStart(firstLBA, lastLBA);

		if (started) {
			c1Result.throughput.Begin(Diagnostics::ScanNowMs());
			const bool speedReady=Diagnostics::CaptureScanSpeedChecked(m_drive,c1Result.speed,cancelled);
			Diagnostics::QualitySampleSequence sequence(firstLBA,lastLBA);
			bool scanDone = false;
			bool c1Cancelled = !speedReady;
			bool c1Failed = false;
			std::string c1FailureReason;
			DWORD lastReportedLBA = DWORD(-1);
			auto lastLBAProgress = std::chrono::steady_clock::now();
			DWORD progressLBA = DWORD(-1);
			constexpr auto QCHECK_STALL_TIMEOUT = std::chrono::seconds(30);

			ProgressIndicator c1Progress(40);
			c1Progress.SetLabel("  C1 Scan");
			c1Progress.SetShowTransferRate(false);
			c1Progress.Start();

			while (!scanDone && !c1Cancelled) {
				if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
					c1Cancelled = true;
					break;
				}

				if (usePlextor)
					std::this_thread::sleep_for(std::chrono::milliseconds(500));

				int c1 = 0, c2 = 0, cu = 0;
				DWORD currentLBA = 0;
				DWORD measuredSectors = 0;
				bool sampleValid = true;

				bool pollOk = usePlextor
					? m_drive.PlextorQCheckPoll(c1, c2, cu, currentLBA, scanDone)
					: usePioneer
					? m_drive.PioneerScanPoll(c1, c2, cu, currentLBA, scanDone,
						&sampleValid, &measuredSectors)
					: m_drive.LiteOnScanPoll(c1, c2, cu, currentLBA, scanDone, &measuredSectors, &sampleValid);

				if (!pollOk && (usePlextor || usePioneer)) {
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
					pollOk = usePlextor
						? m_drive.PlextorQCheckPoll(c1, c2, cu, currentLBA, scanDone)
						: m_drive.PioneerScanPoll(c1, c2, cu, currentLBA, scanDone,
							&sampleValid, &measuredSectors);
				}
				if (!pollOk) {
					c1Failed = true;
					std::ostringstream reason;
					reason << "lost communication or returned invalid positions";
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


				auto pollTime = std::chrono::steady_clock::now();
				if (scanDone || progressLBA == DWORD(-1) || currentLBA > progressLBA) {
					progressLBA = currentLBA;
					lastLBAProgress = pollTime;
				}
				else if (pollTime - lastLBAProgress >= QCHECK_STALL_TIMEOUT) {
					c1Failed = true;
					c1FailureReason = "stalled for 30 seconds at LBA " +
						std::to_string(currentLBA);
					break;
				}
                if (!Diagnostics::CaptureScanSpeedChecked(m_drive,c1Result.speed,cancelled)) {c1Cancelled=true;break;}
                if (usePlextor && measuredSectors==0 && currentLBA==0 && c1==0 && c2==0 && cu==0 && !scanDone)
                    continue;
                const auto decision=sequence.Observe(currentLBA,measuredSectors,c1,c2,cu,sampleValid);
                if (decision==Diagnostics::QualitySampleDecision::Invalid) {
                    c1Failed=true;c1FailureReason="invalid quality sample range, order or counter";break;
                }
                if (decision==Diagnostics::QualitySampleDecision::Ignore) {if(scanDone) break;continue;}
                lastReportedLBA=currentLBA;
                c1Result.throughput.Observe(measuredSectors,Diagnostics::ScanNowMs());
				QCheckSample sample;
				sample.lba = currentLBA;
				sample.measuredSectors = measuredSectors;
				sample.elapsedMs = c1Result.throughput.endMs-c1Result.throughput.startMs;
				sample.c1 = c1;
				// Pioneer reports E22 here, not verified C2/E32. Keep it in the
				// diagnostic field so it cannot masquerade as a copy error.
				if (usePioneer)
					sample.pioneerE22 = c2;
				else
					sample.c2 = c2;
				sample.cu = cu;
				c1Result.samples.push_back(sample);
                c1Result.startup.Record(sample.lba,sample.measuredSectors,sample.c1,usePioneer?sample.pioneerE22:sample.c2,sample.cu);
				c1Result.totalC1 += sample.c1;
				c1Result.totalC2 += sample.c2;
				c1Result.totalCU += sample.cu;
				c1Result.totalPioneerE22 += sample.pioneerE22;
				int idx = static_cast<int>(c1Result.samples.size()) - 1;
				if (c1 > c1Result.maxC1PerSample) {
					c1Result.maxC1PerSample = c1;
					c1Result.maxC1SampleIndex = idx;
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
				c1Progress.SetLabel("  C1 Scan " + Diagnostics::ScanSpeedText(c1Result.throughput.CurrentX()));

				if (currentLBA >= firstLBA) {
					int done = static_cast<int>(std::min<DWORD>(
						currentLBA - firstLBA + 1, c1Result.totalSectors));
					c1Progress.Update(done,
						static_cast<int>(c1Result.totalSectors));
				}
			}

			c1Result.throughput.Finish(Diagnostics::ScanNowMs());
			result.qualitySpeed=c1Result.speed;
			result.qualityThroughput=c1Result.throughput;
			result.qualitySamples=c1Result.samples;
            result.qualityStartup=c1Result.startup;
			c1Progress.Finish(!c1Cancelled && !c1Failed,
				static_cast<int>(c1Result.totalSectors));

			if (!c1Session.Stop()) {
				std::cout << "\nERROR: Could not stop C1 scan; remaining Disc Rot checks cancelled. Reopen the drive.\n";
				m_drive.Close();
				return false;
			}

			if (c1Cancelled || cancelled()) {
				m_drive.SetSpeed(0);
				std::cout << "*** Disc rot scan cancelled ***\n";
				return false;
			}
			if (c1Failed) {
				std::cout << "  C1 quality scan " << c1FailureReason
					<< "; partial raw counts retained, C1 rates and ratings withheld.\n";
			}

			RecalculateQCheckTotals(c1Result);

			hasC1 = !c1Failed && !c1Result.samples.empty() && (c1Result.totalC1 > 0 ||
				c1Result.totalC2 > 0 || c1Result.totalCU > 0 || c1Result.totalPioneerE22 > 0);
			c1Result.c1Unverified = !hasC1;
			DiscRot::RecordQualityEvidence(c1Result, !c1Failed, result);
			if (!c1Failed && !hasC1 && !c1Result.samples.empty())
				std::cout << "  C1 NOT RATED: all counters are zero; measurement unverified.\n";

			// Same sustained-level statistics the quality scan computes, from
			// the same helper, so both scans rate this disc identically. Only
			// used for pattern analysis when the phase completed with activity.
			// RecordQualityEvidence retains raw totals separately for partial phases.
			if (hasC1) {
				ComputeScanPeakContext(c1Result.samples, c1Result.speed.ActualSpeed(), c1Result.peaks);
				ComputeTimedC1(c1Result);
				result.peaks = c1Result.peaks;
			}
			if (hasC1)
				std::cout << "\r  C1 scan complete: " << c1Result.samples.size()
				<< " samples, avg C1=" << std::fixed << std::setprecision(1)
				<< (c1Result.c1.RateAvailable() ? std::to_string(c1Result.avgC1PerSecond) + "/sec" : "unavailable (duration unknown)") << "\n\n";
			// Pioneer E22 remains a separate early-warning diagnostic. Verified
			// C2/E32 and CU are not inferred from it.
			if (usePioneer && hasC1) {
				result.pioneerQualityScanRun = true;
				result.pioneerE22Total = c1Result.totalPioneerE22;
				result.pioneerE22AvgPerSecond = c1Result.avgPioneerE22PerSecond;
				result.pioneerE22Peak = c1Result.maxPioneerE22PerSecond;
				result.pioneerE22Observations = BuildQCheckCounterGraph(c1Result, &QCheckSample::pioneerE22);
				result.pioneerE22Rating = RatePioneerE22(
					result.pioneerE22Total,
					result.pioneerE22AvgPerSecond,
					result.peaks);
			}
		}
		else {
			if (!c1Session.Stop()) {
				std::cout << "  ERROR: C1 cleanup failed; drive closed before further checks.\n";
				m_drive.Close();
				return false;
			}
			std::cout << "  C1 quality scan could not start; continuing with "
				"the independent C2 and consistency phases.\n";
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
		RunPioneerCdCheckCrosscheck(disc, c1Result);
		CopyPioneerCdCheckEvidence(c1Result, result);
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
	progress.SetCdSpeedUnits(true);
	progress.Start();

	ScsiDrive::C2ReadOptions c2Opts;
	c2Opts.countBytes = true;

	DWORD scannedSectors = 0;
	int maxC2InSector = 0;
	const int c2BucketCount = std::max(1, static_cast<int>((totalSectors + 74) / 75));
	std::vector<int> discRotC2PerSecond(c2BucketCount, 0);
	std::vector<int> discRotReadFailuresPerSecond(c2BucketCount, 0);
	int pioneerTransientC2 = 0;
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

			size_t secIdx = std::min<size_t>(
				static_cast<size_t>(scannedSectors / 75), discRotC2PerSecond.size() - 1);
			const auto sector = DiscRot::ReadPhase1Sector(lba, isPioneerDrive,
				[&](DWORD address, BYTE* audio, int& c2) {
					return m_drive.ReadSectorWithC2Ex(address, audio, nullptr, c2, nullptr, c2Opts);
				}, evictAudioCache, cancelled);
			if (sector.cancelled || cancelled()) { progress.Finish(false); return false; }
			DiscRot::RecordPhase1Evidence(sector, result);
			if (sector.transientC2) ++pioneerTransientC2;
			if (sector.HasUnrecoveredFailure()) ++discRotReadFailuresPerSecond[secIdx];
			const bool hasError = sector.HasUnrecoveredFailure() || sector.c2Errors > 0;
			ClassifyZone(lba, firstLBA, lastLBA, hasError ? 1 : 0, result.zones);
			if (hasError) errorLBAs.push_back(lba);
			if (sector.c2Errors > 0) {
				discRotC2PerSecond[secIdx] += sector.c2Errors;
				maxC2InSector = std::max(maxC2InSector, sector.c2Errors);
			}

			scannedSectors++;
			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}
	progress.Finish(true);
	if (pioneerTransientC2 > 0) {
		std::cout << "  [Pioneer] Retained warning for " << pioneerTransientC2
			<< " transient C2 sector" << (pioneerTransientC2 == 1 ? "" : "s")
			<< " not reproduced on cache-cleared verification read.\n";
	}
	if (result.recoveredReadFailures > 0) {
		std::cout << "  [Pioneer] Recovered " << result.recoveredReadFailures
			<< " transient read failure" << (result.recoveredReadFailures == 1 ? "" : "s")
			<< " on verification read.\n";
	}

	std::sort(errorLBAs.begin(), errorLBAs.end());
	DetectErrorClusters(errorLBAs, result.clusters, scanSpeed);
	result.maxC2InSingleSector = maxC2InSector;

	// Adaptive Zone-Based Sampling
	std::cout << "\nPhase 2: Adaptive read consistency check...\n";
	if (cacheCaps.bufferSizeKB <= 0)
		std::cout << "  Drive buffer capacity is unknown; matching rereads will remain unverified.\n";
	else
		std::cout << "  Reading beyond the reported " << cacheCaps.bufferSizeKB
			<< " KiB buffer before each comparison; this can increase scan time.\n";
	m_drive.SetSpeed(scanSpeed);

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
	progress.SetShowTransferRate(false);
	progress.Start();

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				progress.Finish(false);
				std::cout << "\n*** Disc rot scan cancelled during consistency check ***\n";
				return false;
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
			bool cacheCleared = false;
			const bool readable = TestReadConsistency(lba, 3, inconsistent,
				audioRanges, cacheCaps.bufferSizeKB, cacheCleared);
			if (cancelled()) { progress.Finish(false); return false; }
			if (!readable) ++result.consistencyReadFailures;
			if (!cacheCleared) ++result.consistencyUnverifiedSamples;
			samplesChecked++;
			if (!readable || inconsistent > 0) {
				inconsistentSamples++;
				inconsistentLBAs.push_back(lba);
			}

			progress.Update(samplesChecked, expectedSamples);
		}
	}
	progress.Finish(true);

	result.totalRereadTests = samplesChecked;
	result.inconsistentSectors = inconsistentSamples;
	result.inconsistencyRate = samplesChecked > 0
		? static_cast<double>(inconsistentSamples) / samplesChecked * 100.0 : 0;

	AnalyzeErrorPatterns(errorLBAs, result);

	// ── Factor C1 data into rot assessment ───────────────────────────
	if (hasC1) {
		AnalyzeC1RotPatterns(c1Result, firstLBA, lastLBA, result);
	}

	// Read evidence takes precedence over the absence of spatial patterns.
	DiscRot::Finalize(result);

	PrintDiscRotReport(result);

	// Print C1 graph if available
	if (!c1Result.samples.empty()) {
		const auto c1Graph = BuildQCheckCounterGraph(c1Result, &QCheckSample::c1);
		const auto& c1Values = c1Graph.values;

		int maxC1 = 1;
		for (int v : c1Values)
			if (v > maxC1) maxC1 = v;

		// Ensure the chart is tall enough to show the Red Book reference line
		if (!c1Graph.rawCounts && maxC1 < 250) maxC1 = 250;

		Console::GraphOptions c1Opts;
		c1Opts.title = "C1 Quality Profile";
		c1Opts.subtitle = c1Graph.rawCounts ? "Recorded C1 counts per sample; per-second rate unavailable"
			: "C1 = corrected errors - early warning for degradation";
		c1Opts.width = 60;
		c1Opts.height = 10;
		Console::ConfigureC1Graph(c1Opts);
		Console::ConfigureTimedGraph(c1Opts, c1Graph);


		const auto& buckets = c1Graph.values;
		if (c1Graph.valid)
			Console::DrawBarGraph(buckets, maxC1, c1Opts, c1Result.totalSeconds);

		// Pioneer E22 uses the same source samples as the numeric summary. Show
		// it separately from C1 and C2 because it is diagnostic-only.
		if (usePioneer) {
			const auto e22Graph = BuildQCheckCounterGraph(c1Result, &QCheckSample::pioneerE22);
			int peakE22 = e22Graph.valid ? *std::max_element(e22Graph.values.begin(), e22Graph.values.end()) : 0;
			if (e22Graph.valid && (peakE22 > 0 || e22Graph.rawCounts)) {
				Console::GraphOptions e22Opts;
				e22Opts.title = "Disc Rot Pioneer E22 Profile (Diagnostic Only)";
				e22Opts.subtitle = e22Graph.rawCounts ? "Recorded E22 counts per sample; diagnostic only; per-second rate unavailable"
					: "E22 distribution used for early-warning pattern analysis; not C2/CU";
				e22Opts.width = 60;
				e22Opts.height = 10;
				e22Opts.unitSuffix = "/sec";
				e22Opts.severityLowThreshold = 25;
				e22Opts.severityHighThreshold = 100;
				e22Opts.severityLowLabel = "1-24/sec low";
				e22Opts.severityModerateLabel = "25-99/sec elevated";
				e22Opts.severityHighLabel = "100+/sec heavy";
				Console::ConfigureTimedGraph(e22Opts, e22Graph);
				const auto& e22Buckets = e22Graph.values;
				peakE22 = *std::max_element(e22Buckets.begin(), e22Buckets.end());
				Console::DrawBarGraph(e22Buckets, std::max(peakE22, e22Graph.rawCounts ? 1 : 100),
					e22Opts, c1Result.totalSeconds);
			}
		}
	}

	// Phase 1 distribution: show retained positive C2 activity (Pioneer
	// positives are re-read when possible). A zero-only
	// Pioneer bitmap is deliberately not rendered as a green clean graph.
	int peakDiscRotC2 = *std::max_element(discRotC2PerSecond.begin(), discRotC2PerSecond.end());
	if (peakDiscRotC2 > 0) {
		Console::GraphOptions graphOptions;
		graphOptions.title = "Disc Rot C2 Distribution";
		graphOptions.subtitle = "Retained C2 error-pointer activity per second during Phase 1";
		graphOptions.width = 60;
		graphOptions.height = 10;
		graphOptions.unitSuffix = "/sec";
		graphOptions.severityLowThreshold = 5;
		graphOptions.severityHighThreshold = 20;
		graphOptions.severityLowLabel = "1-4/sec low";
		graphOptions.severityModerateLabel = "5-19/sec moderate";
		graphOptions.severityHighLabel = "20+/sec high";
		auto c2Buckets = Console::BucketData(discRotC2PerSecond, graphOptions.width);
		Console::DrawBarGraph(c2Buckets, peakDiscRotC2, graphOptions,
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

bool OpticalDrive::TestReadConsistency(DWORD lba, int passes, int& mismatchCount,
	const std::vector<std::pair<DWORD, DWORD>>& audioRanges, int bufferSizeKB, bool& cacheCleared) {
	auto cancelled = []() { return g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey(); };
	const auto checked = DiscRot::CheckReadConsistency(lba, passes,
		[&](DWORD sector, BYTE* data) { return m_drive.ReadSectorAudioOnly(sector, data); },
		[&](DWORD sector) {
			return DiscRot::EvictAudioCache(sector, audioRanges, bufferSizeKB,
				[&](DWORD start, DWORD count, BYTE* data) { return m_drive.ReadSectorsAudioOnly(start, count, data); },
				cancelled);
		}, cancelled);
	mismatchCount = checked.mismatches;
	cacheCleared = checked.cacheCleared;
	return checked.readable;
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
	analysis.readInstability = analysis.inconsistencyRate > 5.0;
	if (errorLBAs.empty()) {
		analysis.rotRiskLevel = AssessRotRisk(analysis);
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

	analysis.rotRiskLevel = AssessRotRisk(analysis);
}

std::string OpticalDrive::AssessRotRisk(const DiscRotAnalysis& analysis) {
	return DiscRot::AssessPatternRisk(analysis);
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
	std::cout << "\n--- C1 Observations ---\n";
	std::cout << "  Disc layout: " << analysis.discIdentity << "\n";
	std::cout << "  Phase 0 requested speed: " << (analysis.qualityRequestedSpeed>0 ? std::to_string(analysis.qualityRequestedSpeed)+"x" : "maximum") << "\n";
	Diagnostics::PrintScanTelemetry(std::cout,analysis.qualitySpeed,analysis.qualityThroughput);
    std::cout << "  Sample grid: 75 sectors, origin LBA " << analysis.qualityStartup.firstLba << "\n"
        << "  Startup cache preparation: " << (analysis.qualityStartupCacheCleared?"completed before scan start":"UNVERIFIED") << "\n";
    Diagnostics::PrintQualityStartup(std::cout,analysis.qualityStartup,
        analysis.qualityScanMethod.find("Pioneer")!=std::string::npos?"E22":"C2",analysis.qualityCuMeasured,true);
	ScanQuality::PrintC1Summary(std::cout, analysis.c1, analysis.c1RequestedSectors);
	DiscRot::PrintReadEvidence(std::cout, analysis, "  ");

	// Same measurement-confidence header the quality scan prints, so the two
	// reports state their limits in the same words.
	if (analysis.peaks.scanSpeedX > 0) {
		std::cout << "\n";
		std::cout << "  Drive-reported speed: ~" << analysis.peaks.scanSpeedX << "x\n";
		std::cout << "  Peak confidence: "
			<< ScanQuality::ConfidenceLabel(analysis.peaks.PeakConfidence()) << "\n";
		ScanQuality::PrintConfidenceCaveat(std::cout,
			analysis.peaks.PeakConfidence(), "    ");
	}

	std::cout << "\n";
	Heading("  Zone Error Rates\n");
	SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
	std::cout << "  (Scanned audio span divided into three LBA zones)\n\n";
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
		else suffix << "low observed error rate";

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
		if (maxSize > 100) { Error("  (large error region; cause unconfirmed)"); }
		else if (maxSize > 20) { Warning("  (localized read problems; cause unconfirmed)"); }
		else { std::cout << "  (small error region; cause unconfirmed)"; }
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
		"Errors concentrated at ends of the scanned audio span; cause unconfirmed",
		"Errors not edge-concentrated");
	std::cout << "  Progressive pattern: ";
	indicator(analysis.progressivePattern,
		"Error rate increases toward the end of the scanned audio span; cause unconfirmed",
		"No progressive error increase");
	std::cout << "  Pinhole pattern:     ";
	indicator(analysis.pinholePattern,
		"Small scattered error clusters; pitting is not established",
		"Small-cluster threshold not met; physical pinholes were not assessed");
	std::cout << "  Read instability:    ";
	if (analysis.readInstability) {
		SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
		std::cout << Sym::Cross << " YES  - Sampled rereads differ or fail ("
			<< static_cast<int>(analysis.inconsistencyRate) << "% unstable)\n";
	}
	else {
		if (analysis.totalRereadTests == 0 || analysis.consistencyUnverifiedSamples > 0)
			Warning("UNKNOWN - cache eviction was not established for all rereads\n");
		else if (analysis.inconsistentSectors > 0)
			std::cout << "Below threshold, but " << analysis.inconsistentSectors << " sampled rereads differed or failed\n";
		else
			std::cout << "No differences observed in sampled rereads\n";
	}
	Reset();

	if (analysis.pioneerQualityScanRun) {
		std::cout << "\n  Pioneer E22:        " << analysis.pioneerE22Total << " total, "
			<< ScanQuality::CounterAverageText(analysis.pioneerE22Observations)
			<< " avg, " << ScanQuality::CounterPeakText(analysis.pioneerE22Observations) << " peak"
			<< " [" << analysis.pioneerE22Rating << "]\n";
		std::cout << "                       Diagnostic only; E22 is not a verified C2/CU result.\n";
	}

	// Pioneer CD Check (0xE6) uncorrectable cross-check — real data-loss signal
	// that the vendor scan and per-sector C2 can't see on Pioneer drives.
	if (analysis.pioneerDrive) {
		std::cout << "  Uncorrectable:       ";
		if (analysis.pioneerCdCheckPartial) {
			if (HasPioneerCdCheckLoss(analysis)) SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
			else SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
			std::cout << PioneerCdCheckStatus(analysis) << " ("
				<< analysis.pioneerCdCheckC2Bytes << " bytes, worst observed window)\n";
		}
		else if (!analysis.pioneerCdCheckRun) {
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
	std::cout << "  Preservation Risk (heuristic): ";
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
	std::ostringstream c1Summary;
	c1Summary << "# Disc layout: " << analysis.discIdentity << "\n";
	c1Summary << "# Phase 0 requested speed: " << (analysis.qualityRequestedSpeed>0 ? std::to_string(analysis.qualityRequestedSpeed)+"x" : "maximum") << "\n";
	Diagnostics::PrintScanTelemetry(c1Summary,analysis.qualitySpeed,analysis.qualityThroughput,"# ");
    c1Summary << "# Sample grid: 75 sectors, origin LBA " << analysis.qualityStartup.firstLba << "\n"
        << "# Startup cache preparation: " << (analysis.qualityStartupCacheCleared?"completed before scan start":"UNVERIFIED") << "\n";
    Diagnostics::PrintQualityStartup(c1Summary,analysis.qualityStartup,
        analysis.qualityScanMethod.find("Pioneer")!=std::string::npos?"E22":"C2",analysis.qualityCuMeasured,true,"# ");
	ScanQuality::PrintC1Summary(c1Summary, analysis.c1, analysis.c1RequestedSectors, "# ");
	DiscRot::PrintReadEvidence(c1Summary, analysis, "# ");
	fputs(c1Summary.str().c_str(), f);
	fprintf(f, "# ==============================\n");
	fprintf(f, "#\n");
	fprintf(f, "# Risk Level:            %s\n", analysis.rotRiskLevel.c_str());
	if (!analysis.recommendation.empty())
		fprintf(f, "# Recommendation:        %s\n", analysis.recommendation.c_str());
	fprintf(f, "#\n");

	fprintf(f, "# --- Zone Error Rates ---\n");
	fprintf(f, "# Inner  (0-33%%):         %.2f%% (%d/%d)\n",
		analysis.zones.InnerErrorRate(), analysis.zones.innerErrors, analysis.zones.innerSectors);
	fprintf(f, "# Middle (33-66%%):        %.2f%% (%d/%d)\n",
		analysis.zones.MiddleErrorRate(), analysis.zones.middleErrors, analysis.zones.middleSectors);
	fprintf(f, "# Outer  (66-100%%):       %.2f%% (%d/%d)\n",
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
		fprintf(f, "# Pioneer E22:          %d total, %s avg, %s peak [%s] (diagnostic, not C2/CU)\n",
			analysis.pioneerE22Total, ScanQuality::CounterAverageText(analysis.pioneerE22Observations).c_str(),
			ScanQuality::CounterPeakText(analysis.pioneerE22Observations).c_str(), analysis.pioneerE22Rating.c_str());
	}
	if (analysis.pioneerCdCheckPartial) {
		fprintf(f, "# Uncorrectable (CDChk): %s (C1 uncorr=%d frames, C2 uncorr=%d bytes, worst observed window)\n",
			PioneerCdCheckStatus(analysis), analysis.pioneerCdCheckC1Frames, analysis.pioneerCdCheckC2Bytes);
	}
	else if (analysis.pioneerDrive && !analysis.pioneerCdCheckRun) {
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

    fprintf(f, "\n# Phase 0 raw observations; unknown duration is blank, never discarded.\n");
    fprintf(f, "LBA,C1,C2,CU,PioneerE22,CoveredSectors,ElapsedMilliseconds,Region\n");
    for (const auto& sample:analysis.qualitySamples) {
        fprintf(f,"%lu,%d,",sample.lba,sample.c1);
        if (analysis.qualityScanMethod.find("Pioneer")==std::string::npos) fprintf(f,"%d",sample.c2);
        fprintf(f,",");if(analysis.qualityCuMeasured) fprintf(f,"%d",sample.cu);
        fprintf(f,",");if(analysis.qualityScanMethod.find("Pioneer")!=std::string::npos) fprintf(f,"%d",sample.pioneerE22);
        fprintf(f,",");if(sample.measuredSectors>0) fprintf(f,"%lu",sample.measuredSectors);
        fprintf(f,",%llu,%s\n",static_cast<unsigned long long>(sample.elapsedMs),analysis.qualityStartup.Contains(sample.lba)?"Startup":"Main");
    }
    const bool written=ferror(f)==0;
    return fclose(f)==0 && written;
}

void OpticalDrive::AnalyzeC1RotPatterns(const QCheckResult& c1Result,
	DWORD firstLBA, DWORD lastLBA, DiscRotAnalysis& analysis) {
	if (c1Result.samples.empty()) return;

	// Guard invalid ordering and avoid unsigned underflow.
	if (lastLBA <= firstLBA) return;
	const uint64_t range = static_cast<uint64_t>(lastLBA) - static_cast<uint64_t>(firstLBA);
	if (range == 0) return;

	if (!c1Result.c1.RateAvailable()) {
		std::cout << "  C1 zone rates not assessed: sample duration is unverified.\n";
		return;
	}

	// Split measured C1 counts and covered disc sectors into three zones.
	double innerC1 = 0, middleC1 = 0, outerC1 = 0;
	std::uint64_t innerN = 0, middleN = 0, outerN = 0;

	for (const auto& s : c1Result.samples) {
		// Ignore out-of-scan-range samples to avoid wrap and bad positioning.
		if (s.lba < firstLBA || s.lba > lastLBA)
			continue;

		const uint64_t pos = static_cast<uint64_t>(s.lba) - static_cast<uint64_t>(firstLBA);
		double posPct = static_cast<double>(pos) / static_cast<double>(range);

		if (posPct < 0.33) { innerC1 += s.c1; innerN += s.measuredSectors; }
		else if (posPct < 0.66) { middleC1 += s.c1; middleN += s.measuredSectors; }
		else { outerC1 += s.c1; outerN += s.measuredSectors; }
	}

	if (innerN == 0 && middleN == 0 && outerN == 0)
		return;

	double avgInner = innerN > 0 ? innerC1 * 75.0 / innerN : 0;
	double avgMiddle = middleN > 0 ? middleC1 * 75.0 / middleN : 0;
	double avgOuter = outerN > 0 ? outerC1 * 75.0 / outerN : 0;

	std::cout << "\n--- C1 Zone Analysis (early warning) ---\n";
	std::cout << "  Inner  avg C1/sec: " << std::fixed << std::setprecision(1) << avgInner << "\n";
	std::cout << "  Middle avg C1/sec: " << avgMiddle << "\n";
	std::cout << "  Outer  avg C1/sec: " << avgOuter << "\n";

	// Application early-warning indicators based on the measured distribution.
	// Speed, scratches, drive behaviour and chemical degradation can all affect
	// these rates; zone ratios do not uniquely identify the cause.
	const bool zonesComparable = innerN > 0 && middleN > 0 && outerN > 0;
	const auto conf = c1Result.peaks.PeakConfidence();

	bool c1EdgeElevated = zonesComparable && (avgOuter > avgInner * 3.0) && (avgOuter > 10.0);
	bool c1Progressive = zonesComparable && (avgInner < avgMiddle) &&
		(avgMiddle < avgOuter) && (avgOuter > 10.0);
	const auto c1Band = c1Result.c1.Rating();
	bool c1OverallHigh = c1Band == ScanQuality::C1Rating::Fair ||
		c1Band == ScanQuality::C1Rating::Poor;
	bool c1RateHigh = c1Band == ScanQuality::C1Rating::Poor;

	ScanQuality::PrintC1Policy(std::cout);

	if (c1EdgeElevated)
		std::cout << "  ** C1 elevated near end of scanned audio - cause unconfirmed **\n";
	if (c1Progressive)
		std::cout << "  ** C1 rising across scanned audio - spatial pattern, cause unconfirmed **\n";
	if (c1RateHigh)
		std::cout << "  ** C1 average is in the high observed-rate band (>=220/sec) **\n";
	if (c1OverallHigh && !ScanQuality::PeakEvidenceAdmissible(conf))
		ScanQuality::PrintConfidenceCaveat(std::cout, conf);
	std::cout << "  C1 patterns contribute to a heuristic risk score; they do not "
		"diagnose chemical disc rot or predict remaining life.\n";

	// Boost the rot risk score based on C1 findings
	// These are early warnings that wouldn't show up in C2 alone
	int c1Score = 0;
	if (c1EdgeElevated) c1Score += 15;
	if (c1Progressive) c1Score += 20;
	if (c1OverallHigh) c1Score += 10;
	if (c1RateHigh) c1Score += 15;

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
			std::cout << "  Risk level increased from " << current
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
		std::uint64_t inN = 0, midN = 0, outN = 0;
		for (const auto& s : c1Result.samples) {
			if (s.lba < firstLBA || s.lba > lastLBA) continue;
			const uint64_t pos = static_cast<uint64_t>(s.lba) - static_cast<uint64_t>(firstLBA);
			double posPct = static_cast<double>(pos) / static_cast<double>(range);
			if (posPct < 0.33) { innerE22 += s.pioneerE22; inN += s.measuredSectors; }
			else if (posPct < 0.66) { middleE22 += s.pioneerE22; midN += s.measuredSectors; }
			else { outerE22 += s.pioneerE22; outN += s.measuredSectors; }
		}
		double aInE22 = inN > 0 ? innerE22 * 75.0 / inN : 0;
		double aMidE22 = midN > 0 ? middleE22 * 75.0 / midN : 0;
		double aOutE22 = outN > 0 ? outerE22 * 75.0 / outN : 0;

		if (c1Result.totalPioneerE22 > 0) {
			std::cout << "\n--- E22 Zone Analysis (Pioneer diagnostic) ---\n";
			std::cout << "  Inner  avg E22/sec: " << std::fixed << std::setprecision(1) << aInE22 << "\n";
			std::cout << "  Middle avg E22/sec: " << aMidE22 << "\n";
			std::cout << "  Outer  avg E22/sec: " << aOutE22 << "\n";
			std::cout << "  Total E22: " << c1Result.totalPioneerE22
				<< " (avg " << std::fixed << std::setprecision(2) << c1Result.avgPioneerE22PerSecond
				<< "/sec, sustained " << c1Result.peaks.sustainedPioneerE22PerSecond
				<< "/sec, peak " << ScanQuality::CounterPeakText(BuildQCheckCounterGraph(c1Result, &QCheckSample::pioneerE22)) << ")\n";
			if (std::all_of(c1Result.samples.begin(), c1Result.samples.end(),
				[](const QCheckSample& s) { return s.measuredSectors == 75; }) &&
				ScanQuality::TransientNoteWarranted(
					c1Result.maxPioneerE22PerSecond,
					c1Result.peaks.peakPioneerE22Transient,
					ScanQuality::kMinE22PeakWorthExplaining)) {
				ScanQuality::SeriesStats shown;
				shown.peak = c1Result.maxPioneerE22PerSecond;
				shown.peakRunLength = c1Result.peaks.peakPioneerE22RunLength;
				shown.sustainedPeak = c1Result.peaks.sustainedPioneerE22PerSecond;
				ScanQuality::PrintWrapped(std::cout,
					ScanQuality::TransientNote("E22", shown), "  ");
			}
			if (c1Result.peaks.pioneerE22PeakTracksC1)
				ScanQuality::PrintWrapped(std::cout,
					"E22 and C1 peak at the same time slice - one event counted by "
					"two decoder stages, not two independent findings.", "  ");
		}

		// Same basis as the C1 block: averages and zone ratios, never a single
		// slice, so these stay admissible at any scan speed.
		bool e22EdgeElevated = (aOutE22 > aInE22 * 3.0) && (aOutE22 > 2.0);
		bool e22Progressive = (aInE22 < aMidE22) && (aMidE22 < aOutE22) && (aOutE22 > 2.0);
		bool e22OverallHigh = (c1Result.avgPioneerE22PerSecond > 1.0);
		bool e22Heavy = (c1Result.avgPioneerE22PerSecond > 5.0);

		if (e22EdgeElevated)
			std::cout << "  ** E22 elevated near end of scanned audio - cause unconfirmed **\n";
		if (e22Progressive)
			std::cout << "  ** E22 rising across scanned audio - spatial pattern, cause unconfirmed **\n";
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
				std::cout << "  Risk level increased from " << current
					<< " to " << analysis.rotRiskLevel
					<< " based on Pioneer E22 diagnostics\n";
		}
	}
}
