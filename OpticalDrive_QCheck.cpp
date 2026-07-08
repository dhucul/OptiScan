// ============================================================================
// OpticalDrive_QCheck.cpp - Hardware-driven CD quality scan
//
// Performs a C1/C2/CU error-rate scan using either Plextor Q-Check vendor
// commands (0xE9 start / 0xEB poll), Pioneer vendor scan (0x3B/0x3C), or
// the LiteOn/MediaTek equivalent (0xDF).  Unlike the D8-based BLER scan
// which reads audio data and inspects C2 pointers, this mode puts the drive
// into a dedicated error-measurement state where the CIRC decoder reports
// aggregate correction statistics per time slice — no audio is transferred.
//
// The scan produces the same metrics as QPXTool's C1/C2 scan:
//   C1 = block error rate (first-level Reed-Solomon correction)
//   C2 = second-level correction (C1 failures that needed C2 help)
//   CU = uncorrectable errors (both C1 and C2 failed — data loss)
// ============================================================================
#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include "ConsoleGraph.h"
#include "PioneerVendor.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <functional>
#include <fstream>
#include <filesystem>

namespace {
bool IsPioneerScanMethod(const std::string& method) {
	return method.find("Pioneer") != std::string::npos;
}

// Classify the Pioneer E22 diagnostic counter into a 4-tier rating that
// parallels archivalC1Rating.  E22 is a raw firmware counter exposed by
// the vendor scan; it is *not* verified C2 and is not a copy trigger.  This
// rating is purely informational — even "Concerning" does not by itself
// mean the disc is uncopyable.
//
// Tiers:
//   Ideal       total == 0                          — no E22 anywhere
//   Good        avg < 0.25/s AND peak < 25          — normal background
//   Acceptable  avg < 1.0/s  AND peak < 100         — elevated activity
//   Concerning  above                               — heavy E22 activity
const char* PioneerE22Rating(int total, double avg, int peak) {
	if (total == 0) return "Ideal";
	if (avg < 0.25 && peak < 25) return "Good";
	if (avg < 1.0 && peak < 100) return "Acceptable";
	return "Concerning";
}

// Short descriptor for the parenthetical after the rating word.
const char* PioneerE22RatingDescription(const std::string& rating) {
	if (rating == "Ideal")      return "no E22 reported";
	if (rating == "Good")       return "low background, normal for Pioneer scans";
	if (rating == "Acceptable") return "elevated diagnostic activity";
	return "heavy diagnostic activity";
}

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

// PioneerPureReadOffGuard and PioneerPerformanceModeGuard are now in
// PioneerVendor.h (shared with OpticalDrive_DiscRot.cpp).
}  // namespace

// ============================================================================
// Plextor Q-Check Quality Scan (0xE9 / 0xEB vendor commands)
// ============================================================================
// This is fundamentally different from the D8-based BLER scan.  The drive
// enters a dedicated error-measurement mode: it spins at ~1x and reports
// aggregate CIRC decoder statistics (C1/C2/CU) per time slice without
// transferring any audio data.  This matches QPXTool's C1/C2 scan.
// ============================================================================

bool OpticalDrive::RunQCheckScan(const DiscInfo& disc, QCheckResult& result, int scanSpeed) {
	// Lock the tray for the scan so an accidental eject can't abort it. Ref-counted,
	// so the C2/BLER scans that delegate here on Pioneer nest correctly.
	DriveDoorLockGuard doorLock(m_drive);
	std::cout << "\n=== CD Quality Scan (C1/C2/CU) ===\n";

	// ── Probe drive for hardware quality scan support ────────
	// Try Plextor Q-Check first (classic PX-708A through PX-760A drives),
	// then Pioneer vendor scan (0x3B/0x3C), then fall back to the
	// LiteOn/MediaTek vendor commands.
	bool usePlextor = m_drive.SupportsQCheck();
	PioneerVendor pioneerProbe(m_drive);
	PioneerPureReadOffGuard pioneerPureReadGuard(m_drive, pioneerProbe.IsPioneerDrive());
	PioneerPerformanceModeGuard pioneerPerfGuard(m_drive, pioneerProbe.IsPioneerDrive());
	bool usePioneer = false;
	bool useLiteOn = false;

	if (!usePlextor) {
		usePioneer = m_drive.SupportsPioneerScan();
		if (!usePioneer)
			useLiteOn = m_drive.SupportsLiteOnScan();
	}

	// Neither scan backend is available, so report the hardware limitation
	// once without leaking probe-level fallback chatter into normal output.
	if (!usePlextor && !usePioneer && !useLiteOn) {
		std::cout << "ERROR: This drive does not support hardware C1/C2/CU quality scanning.\n";
		std::cout << "       Supported backends: Plextor Q-Check, Pioneer, LiteOn/MediaTek.\n";
		result.supported = false;
		return false;
	}

	result.supported = true;

	if (usePlextor)
		result.scanMethod = "Plextor Q-Check (0xE9/0xEB)";
	else if (usePioneer)
		result.scanMethod = "Pioneer (0x3B/0x3C)";
	else
		result.scanMethod = "LiteOn/MediaTek";

	// The Pioneer vendor scan reports C1 (BLER) and the E22 second-stage
	// counter but no uncorrectable (E32/CU) figure, so its CU is 0 by omission.
	// Flag that so the report doesn't present it as a passed CU check.
	result.cuMeasured = !usePioneer;

	std::cout << "Using " << result.scanMethod << "\n";

	// ── Determine scan range ─────────────────────────────────
	// Scan all audio sectors on the disc, starting from LBA 0 for track 1
	// (to include the hidden pre-gap) and from pregapLBA for other tracks.
	// Data tracks are skipped — the error-measurement mode only works on
	// audio sectors (the drive expects Red Book framing).
	DWORD firstLBA = 0, lastLBA = 0;
	bool first = true;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) {
			DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
			if (first) { firstLBA = start; first = false; }
			lastLBA = t.endLBA;
		}
	}

	if (first) {
		std::cout << "No audio tracks found.\n";
		return false;
	}

	// Pre-calculate disc statistics for progress display and ETA.
	// 75 sectors = 1 second of CD audio (44100 Hz × 2 ch × 16-bit / 2352 bytes).
	result.totalSectors = lastLBA - firstLBA + 1;
	result.totalSeconds = (result.totalSectors + 74) / 75;

	std::cout << "Scan range: LBA " << firstLBA << " - " << lastLBA
		<< " (" << result.totalSectors << " sectors, ~"
		<< result.totalSeconds << " sec)\n";
	if (usePlextor)
		std::cout << "Drive will scan at ~1x internally (hardware-driven).\n";
	else if (usePioneer)
		std::cout << "Drive will scan internally (hardware-driven).\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	// ── Start the hardware scan ──────────────────────────────
	// For LiteOn drives, set the spindle speed before starting — the drive
	// scans at whatever speed is currently configured.  Plextor Q-Check
	// and Pioneer ignore the speed setting and scan internally.
	if (useLiteOn) {
		m_drive.SetSpeed(scanSpeed);
		if (scanSpeed == 0)
			std::cout << "Scan speed: Max\n";
		else
			std::cout << "Scan speed: " << scanSpeed << "x\n";
	}

	// Send the vendor-specific "start scan" command.  The drive begins
	// scanning immediately and will report results via polling.
	bool started = usePlextor ? m_drive.PlextorQCheckStart(firstLBA, lastLBA)
		: usePioneer ? m_drive.PioneerScanStart(firstLBA, lastLBA)
		: m_drive.LiteOnScanStart(firstLBA, lastLBA);

	if (!started) {
		std::cout << "ERROR: Failed to start quality scan.\n";
		return false;
	}

	bool primaryScanStopped = false;
	auto stopPrimaryScan = [&]() {
		if (primaryScanStopped)
			return;
		if (usePlextor) m_drive.PlextorQCheckStop();
		else if (usePioneer) m_drive.PioneerScanStop();
		else m_drive.LiteOnScanStop();
		primaryScanStopped = true;
		};

	// ── Poll for results ─────────────────────────────────────
	// The drive scans asynchronously.  We poll periodically to retrieve
	// C1/C2/CU counts and the current LBA position.  Each successful
	// poll returns one time-slice of aggregated error statistics.
	bool scanDone = false;
	int sampleIndex = 0;
	DWORD lastReportedLBA = DWORD(-1);   // Tracks duplicate reports from the same position

	// ── Timer state for elapsed / ETA display ────────────────
	auto scanStartTime = std::chrono::steady_clock::now();
	auto lastProgressPaint = scanStartTime - std::chrono::milliseconds(250);
	int lastLineLength = 0;              // For padding '\r' lines to clear remnants
	constexpr int BAR_WIDTH = 25;        // Width of the UTF-8 progress bar in columns
	constexpr auto PROGRESS_PAINT_INTERVAL = std::chrono::milliseconds(250);

	while (!scanDone) {
		// ── Check for user cancellation ──────────────────────
		// Stop the hardware scan gracefully before returning so the drive
		// doesn't continue spinning in measurement mode indefinitely.
		if (InterruptHandler::Instance().IsInterrupted() || InterruptHandler::Instance().CheckEscapeKey()) {
			stopPrimaryScan();
			std::cout << "\n*** Quality scan cancelled by user ***\n";
			return false;
		}

		// Plextor Q-Check is asynchronous — the drive scans internally and
		// we poll for updates.  Pioneer and LiteOn block on their SCSI
		// command sequence, so adding a fixed delay makes scans needlessly
		// slow.
		if (usePlextor)
			std::this_thread::sleep_for(std::chrono::milliseconds(500));

		int c1 = 0, c2 = 0, cu = 0;
		DWORD currentLBA = 0;

		// Poll the drive for the next time-slice of error statistics.
		// Returns false on communication failure, true otherwise.
		// `scanDone` is set to true by the driver when it reaches lastLBA.
		bool pollOk = usePlextor
			? m_drive.PlextorQCheckPoll(c1, c2, cu, currentLBA, scanDone)
			: (usePioneer ? m_drive.PioneerScanPoll(c1, c2, cu, currentLBA, scanDone)
				: m_drive.LiteOnScanPoll(c1, c2, cu, currentLBA, scanDone));

		if (!pollOk) {
			// One retry for asynchronous scans (Plextor / Pioneer) —
			// transient SCSI timeouts are common when the drive is busy
			// with its internal scan loop.
			if (usePlextor || usePioneer) {
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				pollOk = usePlextor
					? m_drive.PlextorQCheckPoll(c1, c2, cu, currentLBA, scanDone)
					: m_drive.PioneerScanPoll(c1, c2, cu, currentLBA, scanDone);
			}
			if (!pollOk) {
				// Communication lost.  Stop the scan if possible.
				stopPrimaryScan();
				// If we already have partial data, treat it as a completed
				// scan and report what we have — better than nothing.
				if (!result.samples.empty()) break;
				std::cout << "\nERROR: Lost communication with drive during scan.\n";
				return false;
			}
		}

		// Skip empty responses — the drive hasn't produced data yet
		// (still seeking to the start position or spinning up).
		// Pioneer tracks LBA in software starting at firstLBA, so LBA 0
		// is a valid position — don't apply this filter for Pioneer scans.
		if (!usePioneer && currentLBA == 0 && c1 == 0 && c2 == 0 && cu == 0 && !scanDone)
			continue;

		// Skip duplicate LBA reports — the drive sometimes returns the
		// same position twice before advancing to the next time slice.
		if (currentLBA == lastReportedLBA && !scanDone)
			continue;
		lastReportedLBA = currentLBA;

		// Discard the first 3 samples — the drive is still seeking/spinning
		// up and reports accumulated startup errors in the initial responses.
		// QPXTool does the same; without this the first sample creates a
		// massive spike that dominates the entire graph and skews statistics.
		// Applies to all scan paths (Plextor, Pioneer, LiteOn).
		if (sampleIndex < 3) {
			sampleIndex++;
			continue;
		}

		// LiteOn drives don't set the `scanDone` flag automatically —
		// detect end-of-disc by checking if the current position has
		// reached or passed the last audio sector.
		if (useLiteOn && currentLBA >= lastLBA) {
			scanDone = true;
		}

		// ── Record the sample ────────────────────────────────
		QCheckSample sample;
		sample.lba = currentLBA;
		sample.c1 = c1;    // C1 corrections this time slice (first-level Reed-Solomon)
		// Pioneer's vendor scan reports its second-level error counter (E22) at
		// the same place other drives report C2.  Surface it as C2 so it is
		// counted, graphed, track-bucketed, and drives the verdict — matching how
		// QPxTool presents the Pioneer reading.
		sample.c2 = c2;          // C2 (LiteOn/Plextor) or E22-as-C2 (Pioneer)
		sample.cu = cu;     // Uncorrectable errors (both correction stages failed)
		result.samples.push_back(sample);

		// Running totals for summary statistics.
		result.totalC1 += sample.c1;
		result.totalC2 += sample.c2;
		result.totalCU += sample.cu;
		result.totalPioneerE22 += sample.pioneerE22;

		// Track peak values and their positions for the report.
		int idx = static_cast<int>(result.samples.size()) - 1;

		if (c1 > result.maxC1PerSecond) {
			result.maxC1PerSecond = c1;
			result.maxC1SecondIndex = idx;    // Sample index where peak C1 occurred
		}
		if (sample.c2 > result.maxC2PerSecond) {
			result.maxC2PerSecond = sample.c2;
			result.maxC2SecondIndex = idx;
		}
		if (sample.pioneerE22 > result.maxPioneerE22PerSecond) {
			result.maxPioneerE22PerSecond = sample.pioneerE22;
			result.maxPioneerE22SecondIndex = idx;
		}
		if (cu > result.maxCUPerSecond)
			result.maxCUPerSecond = cu;

		sampleIndex++;

		// ── Compute progress, elapsed, and ETA ───────────────
		// Progress is calculated from the LBA position relative to the
		// total scan range, not from sample count, since time slices
		// don't have a fixed sector width.
		double pct = 0.0;
		if (currentLBA >= firstLBA && result.totalSectors > 0) {
			pct = static_cast<double>(currentLBA - firstLBA) * 100.0 / result.totalSectors;
			if (pct > 100.0) pct = 100.0;
		}

		auto now = std::chrono::steady_clock::now();
		int elapsedSec = static_cast<int>(
			std::chrono::duration_cast<std::chrono::seconds>(now - scanStartTime).count());

		// ETA: linear extrapolation from elapsed time and progress fraction.
		// Only shown after 1% progress to avoid wildly inaccurate estimates
		// during the initial seek.
		int etaSec = -1;
		if (pct > 1.0 && pct < 100.0) {
			double remaining = (100.0 - pct) / pct;
			etaSec = static_cast<int>(elapsedSec * remaining);
		}

		const bool shouldPaintProgress =
			scanDone || pct >= 100.0 ||
			(now - lastProgressPaint) >= PROGRESS_PAINT_INTERVAL;
		if (!shouldPaintProgress)
			continue;
		lastProgressPaint = now;

		// ── Format progress bar ──────────────────────────────
		// Uses UTF-8 block characters: █ (filled) and ░ (empty) for a
		// smooth visual progress indicator on the same console line (\r).
		int filled = static_cast<int>(pct * BAR_WIDTH / 100.0);
		std::ostringstream line;
		line << "\r  [";
		for (int i = 0; i < BAR_WIDTH; i++)
			line << (i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91"); // █ or ░
		line << "] " << std::fixed << std::setprecision(1) << pct << "%";

		// Append elapsed time as M:SS.
		line << "  " << (elapsedSec / 60) << ":"
			<< std::setfill('0') << std::setw(2) << (elapsedSec % 60);

		// Append ETA in adaptive format (Xh Ym Zs / Xm Zs / Zs).
		if (etaSec >= 0) {
			line << " ETA:";
			if (etaSec >= 3600)
				line << (etaSec / 3600) << "h"
				<< std::setfill('0') << std::setw(2) << ((etaSec % 3600) / 60) << "m"
				<< std::setfill('0') << std::setw(2) << (etaSec % 60) << "s";
			else if (etaSec >= 60)
				line << (etaSec / 60) << "m"
				<< std::setfill('0') << std::setw(2) << (etaSec % 60) << "s";
			else
				line << etaSec << "s";
		}

		// Show live error counts so the user can spot problems immediately
		// without waiting for the full report.
		line << "  C1=" << c1
			<< " C2=" << c2
			<< " CU=" << cu;

		// Pad with spaces to overwrite any leftover characters from a
		// longer previous line (e.g. when ETA shrinks).
		std::string output = line.str();
		if (static_cast<int>(output.size()) < lastLineLength)
			output.append(lastLineLength - output.size(), ' ');
		lastLineLength = static_cast<int>(output.size());

		std::cout << output << std::flush;
	}

	stopPrimaryScan();

	// ── Print final elapsed time ─────────────────────────────
	auto scanEndTime = std::chrono::steady_clock::now();
	int totalElapsed = static_cast<int>(
		std::chrono::duration_cast<std::chrono::seconds>(scanEndTime - scanStartTime).count());

	// Format elapsed time in adaptive units (h/m/s) for readability.
	std::ostringstream elapsed;
	if (totalElapsed >= 3600)
		elapsed << (totalElapsed / 3600) << "h "
		<< std::setfill('0') << std::setw(2) << ((totalElapsed % 3600) / 60) << "m "
		<< std::setfill('0') << std::setw(2) << (totalElapsed % 60) << "s";
	else if (totalElapsed >= 60)
		elapsed << (totalElapsed / 60) << "m "
		<< std::setfill('0') << std::setw(2) << (totalElapsed % 60) << "s";
	else
		elapsed << totalElapsed << "s";

	std::cout << "\n  Done in " << elapsed.str()
		<< " (" << result.samples.size() << " samples)\n";

	// ── Remove startup spike(s) ──────────────────────────────
	// Even after discarding the first 3 raw samples, the earliest recorded
	// samples can still contain inflated error counts from the drive's
	// spin-up / seek settling.  Compare early samples against the median
	// error rate: if any are 10× above median, trim them.  This matches
	// QPXTool's behaviour and prevents a single initial spike from
	// dominating the graph Y-axis and inflating the overall rating.
	bool spikesTrimmed = false;    // Hoisted — needed by c1Unverified check later

	if (result.samples.size() > 50) {
		// Compute median total error (C1+C2+CU) across all samples.
		std::vector<int> allErrs;
		allErrs.reserve(result.samples.size());
		for (const auto& s : result.samples)
			allErrs.push_back(s.c1 + s.c2 + s.cu);
		std::sort(allErrs.begin(), allErrs.end());
		int median = allErrs[allErrs.size() / 2];

		// Only check the first 30 samples (or half the dataset, whichever
		// is smaller) — startup artefacts won't appear later in the scan.
		size_t checkEnd = std::min<size_t>(30, result.samples.size() / 2);

		// Iterate backwards so erasing doesn't invalidate lower indices.
		for (int i = static_cast<int>(checkEnd) - 1; i >= 0; i--) {
			int err = result.samples[i].c1 + result.samples[i].c2 + result.samples[i].cu;
			// Two criteria: absolute threshold (>10) when disc is clean,
			// or relative threshold (10× median) for noisy discs.
			if ((median == 0 && err > 10) || (median > 0 && err > median * 10)) {
				// Subtract the trimmed sample's counts from running totals.
				result.totalC1 -= result.samples[i].c1;
				result.totalC2 -= result.samples[i].c2;
				result.totalCU -= result.samples[i].cu;
				result.totalPioneerE22 -= result.samples[i].pioneerE22;
				result.samples.erase(result.samples.begin() + i);
				spikesTrimmed = true;
			}
		}

		if (spikesTrimmed)
			RecalculateQCheckTotals(result);
	}

	// Pioneer's E22 counter is now treated as C2 (see sample recording above),
	// so it flows through the same startup-spike trimming as every other scan
	// backend and needs no separate vendor-artifact handling.

	// Track whether the drive reported any C2 during the primary scan,
	// before the recheck potentially zeroes the total.  Used later to
	// avoid a false c1Unverified flag — if C2 was reported, the drive's
	// measurement mode is at least partially functional.
	bool hadC2BeforeRecheck = result.totalC2 > 0;

	// ── C2 recheck: verify transient C2 errors ───────────────
	// C2 errors are significant — they mean C1 correction failed and the
	// drive needed second-level error correction.  However, a single scan
	// can produce transient C2 spikes from vibration, dust, or thermal
	// drift.  To avoid false alarms, re-scan the entire disc if any C2
	// errors were detected.  If the re-scan shows 0 C2, the original
	// C2 counts are discarded as transient artefacts.
	if (result.totalC2 > 0 &&
		!InterruptHandler::Instance().IsInterrupted()) {
		std::cout << "\n  C2 errors detected (" << result.totalC2
			<< " total) - running verification re-scan...\n";

		// Stop the current scan session and wait for the drive to settle
		// before starting a fresh scan.
		stopPrimaryScan();
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));

		// Start a second complete scan over the same LBA range.
		bool recheckStarted = usePlextor
			? m_drive.PlextorQCheckStart(firstLBA, lastLBA)
			: (usePioneer ? m_drive.PioneerScanStart(firstLBA, lastLBA)
				: m_drive.LiteOnScanStart(firstLBA, lastLBA));

		if (recheckStarted) {
			int recheckC2Total = 0;        // Only C2 matters for this pass
			bool recheckDone = false;
			bool recheckStopped = false;
			int recheckSampleIdx = 0;
			DWORD recheckLastLBA = DWORD(-1);
			int recheckLastLine = 0;
			auto recheckStart = std::chrono::steady_clock::now();
			auto lastRecheckProgressPaint = recheckStart - std::chrono::milliseconds(250);

			// Same polling loop as the primary scan, but we only accumulate
			// C2 counts — C1 and CU from the recheck are discarded.
			while (!recheckDone) {
				if (InterruptHandler::Instance().IsInterrupted() || InterruptHandler::Instance().CheckEscapeKey()) {
					if (usePlextor) m_drive.PlextorQCheckStop();
					else if (usePioneer) m_drive.PioneerScanStop();
					else m_drive.LiteOnScanStop();
					recheckStopped = true;
					std::cout << "\n  *** Recheck cancelled - keeping original C2 results ***\n";
					break;
				}

				// Same async delay as the primary loop.  Only Plextor needs
				// pacing; Pioneer and LiteOn block on their SCSI command
				// sequence.
				if (usePlextor)
					std::this_thread::sleep_for(std::chrono::milliseconds(500));

				int rc1 = 0, rc2 = 0, rcu = 0;
				DWORD rLBA = 0;

				bool rpoll = usePlextor
					? m_drive.PlextorQCheckPoll(rc1, rc2, rcu, rLBA, recheckDone)
					: (usePioneer ? m_drive.PioneerScanPoll(rc1, rc2, rcu, rLBA, recheckDone)
						: m_drive.LiteOnScanPoll(rc1, rc2, rcu, rLBA, recheckDone));

				if (!rpoll) {
					// Same retry logic as the primary scan — async scans
					// (Plextor / Pioneer) get one retry on transient failure.
					if (usePlextor || usePioneer) {
						std::this_thread::sleep_for(std::chrono::milliseconds(200));
						rpoll = usePlextor
							? m_drive.PlextorQCheckPoll(rc1, rc2, rcu, rLBA, recheckDone)
							: m_drive.PioneerScanPoll(rc1, rc2, rcu, rLBA, recheckDone);
					}
					if (!rpoll) {
						if (usePlextor) m_drive.PlextorQCheckStop();
						else if (usePioneer) m_drive.PioneerScanStop();
						else m_drive.LiteOnScanStop();
						recheckStopped = true;
						std::cout << "\n  Recheck communication lost - keeping original C2 results.\n";
						break;
					}
				}

				// Same filtering as primary scan: skip empty / duplicate / startup samples.
				// Pioneer tracks LBA in software starting at firstLBA, so LBA 0
				// is a valid position — don't apply this filter for Pioneer scans.
				if (!usePioneer && rLBA == 0 && rc1 == 0 && rc2 == 0 && rcu == 0 && !recheckDone)
					continue;
				if (rLBA == recheckLastLBA && !recheckDone)
					continue;
				recheckLastLBA = rLBA;

				// Discard first 3 samples (startup artefacts).
				if (recheckSampleIdx < 3) {
					recheckSampleIdx++;
					continue;
				}

				if (useLiteOn && rLBA >= lastLBA)
					recheckDone = true;

				recheckC2Total += rc2;
				recheckSampleIdx++;

				// ── Recheck progress bar ─────────────────────
				double rpct = 0.0;
				if (rLBA >= firstLBA && result.totalSectors > 0) {
					rpct = static_cast<double>(rLBA - firstLBA) * 100.0 / result.totalSectors;
					if (rpct > 100.0) rpct = 100.0;
				}

				auto rnow = std::chrono::steady_clock::now();
				int rElapsed = static_cast<int>(
					std::chrono::duration_cast<std::chrono::seconds>(rnow - recheckStart).count());

				const bool shouldPaintRecheckProgress =
					recheckDone || rpct >= 100.0 ||
					(rnow - lastRecheckProgressPaint) >= PROGRESS_PAINT_INTERVAL;
				if (!shouldPaintRecheckProgress)
					continue;
				lastRecheckProgressPaint = rnow;

				std::ostringstream rline;
				int rfilled = static_cast<int>(rpct * BAR_WIDTH / 100.0);
				rline << "\r  Recheck [";
				for (int i = 0; i < BAR_WIDTH; i++)
					rline << (i < rfilled ? "\xe2\x96\x88" : "\xe2\x96\x91");
				rline << "] " << std::fixed << std::setprecision(1) << rpct << "%"
					<< "  " << (rElapsed / 60) << ":"
					<< std::setfill('0') << std::setw(2) << (rElapsed % 60)
					<< "  C2=" << recheckC2Total;

				std::string routput = rline.str();
				if (static_cast<int>(routput.size()) < recheckLastLine)
					routput.append(recheckLastLine - routput.size(), ' ');
				recheckLastLine = static_cast<int>(routput.size());
				std::cout << routput << std::flush;
			}

			// ── Evaluate recheck results ─────────────────────
			if (recheckDone) {
				if (recheckC2Total == 0) {
					// Recheck found zero C2 — the original C2 errors were
					// transient (vibration, dust particle, thermal).  Zero
					// out the C2 data in the primary result so the report
					// and rating reflect the clean re-scan.
					std::cout << "\n  Recheck PASSED: 0 C2 errors on re-scan.\n";
					std::cout << "  Original C2 count (" << result.totalC2
						<< ") discarded as transient.\n";

					for (auto& s : result.samples) {
						s.c2 = 0;
					}
					result.totalC2 = 0;
					result.maxC2PerSecond = 0;
					result.maxC2SecondIndex = -1;
				}
				else {
					// C2 errors reproduced — they're genuine surface or
					// pressing defects.  Keep the original (first-pass)
					// results intact for accurate reporting.
					std::cout << "\n  Recheck CONFIRMED: " << recheckC2Total
						<< " C2 errors on re-scan (original: " << result.totalC2 << ").\n";
					std::cout << "  C2 errors are genuine - keeping original results.\n";
				}
			}

			// Ensure the scan session is stopped regardless of outcome.
			if (!recheckStopped) {
				if (usePlextor) m_drive.PlextorQCheckStop();
				else if (usePioneer) m_drive.PioneerScanStop();
				else m_drive.LiteOnScanStop();
			}
		}
		else {
			std::cout << "  WARNING: Could not start recheck scan - keeping original C2 results.\n";
		}
	}

	// ── Bucket C2 / CU errors by track ───────────────────────
	// Walk every sample and find the audio track whose LBA range contains
	// the sample.  Aggregate C2 and CU counts per track so the report can
	// pinpoint which track(s) hold the trouble.  Only audio tracks are
	// considered — Q-Check skips data tracks during scanning.
	{
		result.errorTracks.clear();
		for (const auto& s : result.samples) {
			if (s.c2 == 0 && s.cu == 0) continue;
			for (const auto& t : disc.tracks) {
				if (!t.isAudio) continue;
				DWORD trackStart = (t.trackNumber == 1) ? 0 : t.pregapLBA;
				if (s.lba >= trackStart && s.lba <= t.endLBA) {
					auto it = std::find_if(result.errorTracks.begin(), result.errorTracks.end(),
						[&](const QCheckTrackErrors& e) { return e.trackNumber == t.trackNumber; });
					if (it == result.errorTracks.end()) {
						QCheckTrackErrors entry;
						entry.trackNumber = t.trackNumber;
						entry.c2Count = s.c2;
						entry.cuCount = s.cu;
						result.errorTracks.push_back(entry);
					}
					else {
						it->c2Count += s.c2;
						it->cuCount += s.cu;
					}
					break;
				}
			}
		}
		std::sort(result.errorTracks.begin(), result.errorTracks.end(),
			[](const QCheckTrackErrors& a, const QCheckTrackErrors& b) {
				return a.trackNumber < b.trackNumber;
			});
	}

	// ── Compute summary statistics ───────────────────────────
	// Average errors per second (per sample) for the report and rating.
	DWORD sampleCount = static_cast<DWORD>(result.samples.size());
	if (sampleCount > 0) {
		result.avgC1PerSecond = static_cast<double>(result.totalC1) / sampleCount;
		result.avgC2PerSecond = static_cast<double>(result.totalC2) / sampleCount;
		result.avgPioneerE22PerSecond = static_cast<double>(result.totalPioneerE22) / sampleCount;
	}

	// ── Quality rating ───────────────────────────────────────
	// Multi-tier rating based on CIRC error hierarchy:
	//   CU > 0           → BAD    (uncorrectable = data loss)
	//   C2 high avg      → POOR   (heavy second-level correction)
	//   C2 any           → FAIR   (some second-level correction needed)
	//   C1 avg ≥ 220     → POOR   (exceeds Red Book BLER limit)
	//   C1 avg ≥ 50      → FAIR   (elevated but within spec)
	//   C1 avg ≥ 5       → GOOD   (normal wear)
	//   C1 avg < 5       → EXCELLENT (pristine disc)
	bool pioneerScan = IsPioneerScanMethod(result.scanMethod);

	if (result.totalCU > 0)
		result.qualityRating = "BAD";
	else if (result.totalC2 > 0 && result.avgC2PerSecond > 10.0)
		result.qualityRating = "POOR";
	else if (result.totalC2 > 0)
		result.qualityRating = "FAIR";
	else if (result.avgC1PerSecond >= 220.0)
		result.qualityRating = "POOR";
	else if (result.avgC1PerSecond >= 50.0)
		result.qualityRating = "FAIR";
	else if (result.avgC1PerSecond >= 5.0)
		result.qualityRating = "GOOD";
	else
		result.qualityRating = "EXCELLENT";

	// ── C1 load quality tier ─────────────────────────────────
	// Judge C1 by rate, not raw total.  A longer disc naturally accumulates
	// more total C1, so the total is useful context but not a quality tier.
	if (result.avgC1PerSecond < 1.0)
		result.totalC1Quality = "Exceptional";
	else if (result.avgC1PerSecond < 5.0)
		result.totalC1Quality = "Very good";
	else if (result.avgC1PerSecond < 50.0)
		result.totalC1Quality = "Normal";
	else if (result.avgC1PerSecond < 220.0)
		result.totalC1Quality = "Marginal";
	else
		result.totalC1Quality = "Poor";

	// ── Archival suitability rating ──────────────────────────
	// For long-term archival, peak C1 matters more than average — a single
	// region with high C1 may degrade further over time and eventually
	// produce C2/CU errors.
	if (result.maxC1PerSecond < 50)
		result.archivalC1Rating = "Ideal";
	else if (result.maxC1PerSecond < 100)
		result.archivalC1Rating = "Good";
	else if (result.maxC1PerSecond <= 220)
		result.archivalC1Rating = "Acceptable";
	else
		result.archivalC1Rating = "Poor";

	// ── Pioneer E22 diagnostic rating ────────────────────────
	// Only meaningful on Pioneer vendor scans; left empty otherwise.
	if (IsPioneerScanMethod(result.scanMethod)) {
		result.pioneerE22Rating = PioneerE22Rating(
			result.totalPioneerE22,
			result.avgPioneerE22PerSecond,
			result.maxPioneerE22PerSecond);
	}

	// ── Flag potentially non-functional quality scan ─────────
	// A real CD always has some C1 correction activity — even a pristine
	// pressing produces a few C1 errors per second.  If the entire disc
	// scanned with zero C1 AND zero C2, the drive likely accepted the
	// vendor command but is not actually reporting CIRC statistics.
	// This is a known issue with drives that pass the capability probe
	// (e.g. respond to 0xE9/0xDF without error) but don't implement the
	// measurement mode in firmware.
	//
	// Guards against false positives:
	//   !hadC2BeforeRecheck — if the drive did report C2 in the primary
	//     scan, the measurement mode is at least partially functional
	//     (even if the recheck later zeroed the C2 as transient).
	//   !spikesTrimmed — if startup spikes were removed, the original
	//     data had non-zero errors; the zeroes are from trimming, not
	//     from a non-functional drive.
	if (result.totalC1 == 0 && result.totalC2 == 0 && result.totalCU == 0
		&& !result.samples.empty() && !hadC2BeforeRecheck && !spikesTrimmed
		&& result.totalPioneerE22 == 0) {
		result.c1Unverified = true;
		result.qualityRating = "UNVERIFIED";
	}

	// ── Pioneer uncorrectable cross-check via CD Check (0xE6) ─
	// The vendor scan reported C1 + E22 but no CU. On Pioneer drives that still
	// implement the CD Check protocol, measure real uncorrectable (C2) data over
	// the same audio range so the report can give a true data-loss answer.
	//
	// This is a full extra 1x pass, so gate it: uncorrectable (E32) errors can
	// only exist where the second stage (C2/E22) was already exercised, so a disc
	// that scanned pristine — a clean rating, zero E22, and no C1 hot-spot — has
	// nothing for the cross-check to find. Skip it there to avoid doubling the
	// scan time, and run it whenever there is any hint of trouble (any E22, an
	// elevated/peaky C1, or a FAIR/POOR/BAD/UNVERIFIED verdict). The dedicated
	// Pioneer CD Check (menu option 28) can still force an uncorrectable pass on
	// a clean disc on demand.
	if (usePioneer && !InterruptHandler::Instance().IsInterrupted()) {
		const bool discLooksClean =
			(result.qualityRating == "EXCELLENT" || result.qualityRating == "GOOD")
			&& result.totalC2 == 0
			&& result.maxC1PerSecond < 50;

		if (discLooksClean) {
			result.pioneerCdCheckSkippedClean = true;
			std::cout << "\n  Disc scanned clean (no C2/E22, low C1) - skipping the CD Check\n"
				<< "  uncorrectable cross-check to save a full pass. Use Pioneer CD Check\n"
				<< "  (option 28) to force an uncorrectable measurement anyway.\n";
		}
		else if (RunPioneerCdCheckCrosscheck(disc, result) &&
			result.pioneerCdCheckC2Bytes > 0) {
			// Uncorrectable data confirmed = data loss, regardless of the C1/E22
			// verdict. Escalate to BAD to match how CU > 0 is treated on the
			// Plextor/LiteOn backends.
			result.qualityRating = "BAD";
		}
	}

	PrintQCheckReport(result);
	return true;
}

// ============================================================================
// RunPioneerCdCheckCrosscheck — Pioneer CD Check (0xE6) uncorrectable pass
// ----------------------------------------------------------------------------
// The Pioneer 0x3B/0x3C vendor quality scan reports C1 (BLER) and the E22
// second-stage counter but no uncorrectable (E32/CU) figure. The separate
// Pioneer CD Check (0xE6+0x300000) protocol DOES report a real C2-uncorrectable
// byte count. When the drive implements it, run it over the same audio range so
// the quality-scan report can show a genuine uncorrectable result. Fills the
// result.pioneerCdCheck* fields; returns true only on a valid measurement.
// Mirrors the polling shape of RunPioneerCdCheck (MainMenu.cpp) but condensed:
// no grading/printout, just the worst-window C1/C2 uncorrectable figures.
// ============================================================================
bool OpticalDrive::RunPioneerCdCheckCrosscheck(const DiscInfo& disc, QCheckResult& result) {
	PioneerVendor pv(m_drive);
	if (!pv.IsPioneerDrive())
		return false;

	// Audio range: track 1 start .. last audio-track end (same span the vendor
	// scan covered).
	DWORD startLBA = 0, endLBA = 0;
	bool first = true;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		if (first) { startLBA = t.startLBA; first = false; }
		if (t.endLBA > endLBA) endLBA = t.endLBA;
	}
	if (first || endLBA <= startLBA)
		return false;

	// Address conventions match RunPioneerCdCheck: the CD Check start takes an
	// "inner" address (LBA + 0x6000 + 150) while progress is reported in frame
	// addresses (LBA + 150). Unit size 38 sectors per measurement window.
	constexpr DWORD kInnerOffset = 0x6000 + 150;
	constexpr DWORD kFrameOffset = 150;
	constexpr DWORD kUnitSectors = 38;
	const DWORD startFrame = startLBA + kFrameOffset;
	const DWORD endFrame   = endLBA + kFrameOffset;
	const DWORD startInner = startLBA + kInnerOffset;
	const DWORD totalSectors = endLBA - startLBA + 1;

	std::cout << "\n  Pioneer CD Check uncorrectable cross-check (0xE6)...\n";

	{
		BYTE sk = 0, asc = 0, ascq = 0;
		if (!pv.CdCheckStartWithSense(startInner, kUnitSectors, sk, asc, ascq)) {
			// Most likely this firmware doesn't implement CD Check (e.g. BDR-S13U
			// returns Illegal Request). CU stays unmeasured; not an error.
			std::cout << "  CD Check not available on this drive - CU left unmeasured.\n";
			return false;
		}
	}

	ProgressIndicator prog(40);
	prog.SetLabel("  CD Check");
	prog.Start();

	int worstC1 = 0, worstC2 = 0;
	bool sawValid = false, sawProgress = false, cancelled = false;
	DWORD lastEnd = startFrame;
	int stallTicks = 0;
	// Worst case: a full 80-minute CD at 1x. Cap wall-clock at 90 minutes.
	constexpr int kPollMs = 500;
	constexpr int kMaxIters = (90 * 60 * 1000) / kPollMs;
	constexpr int kStallLimit = 60;            // 30 s with no advance
	constexpr int kStartupGraceIters = 20;     // 10 s before declaring a start-up stall

	for (int i = 0; i < kMaxIters; i++) {
		if (InterruptHandler::Instance().IsInterrupted() ||
			InterruptHandler::Instance().CheckEscapeKey()) {
			cancelled = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));

		PioneerCdCheckResult r;
		if (!pv.CdCheckRead(r))
			break;
		if (r.dataValid) {
			sawValid = true;
			if (r.c1Uncorrectable > worstC1) worstC1 = r.c1Uncorrectable;
			if (r.c2Uncorrectable > worstC2) worstC2 = r.c2Uncorrectable;
		}

		DWORD ea = r.endAddress;
		if (ea > startFrame) {
			sawProgress = true;
			DWORD done = std::min<DWORD>(ea - startFrame, totalSectors);
			prog.Update(static_cast<int>(done), static_cast<int>(totalSectors));
		}

		if (ea == lastEnd) {
			if (sawProgress || i >= kStartupGraceIters) {
				if (++stallTicks >= kStallLimit) break;
			}
		}
		else {
			stallTicks = 0;
			lastEnd = ea;
		}
		if (sawProgress && ea >= endFrame) break;
	}

	prog.Finish(sawProgress && !cancelled);
	pv.CdCheckStop();

	if (cancelled) {
		std::cout << "  CD Check cross-check cancelled - CU left unmeasured.\n";
		return false;
	}
	if (!sawProgress || !sawValid) {
		std::cout << "  CD Check produced no valid measurement - CU left unmeasured.\n";
		return false;
	}

	result.pioneerCdCheckRun = true;
	result.pioneerCdCheckC1Frames = worstC1;
	result.pioneerCdCheckC2Bytes = worstC2;
	return true;
}

// ============================================================================
// PrintQCheckReport — Formatted console report with graphs
// ============================================================================
// Outputs a multi-section report covering:
//   1. Scan metadata (samples, duration, sectors)
//   2. C1 block error rate analysis vs Red Book 220/sec limit
//   3. Total C1 quality tier and archival suitability
//   4. C2 error analysis with severity classification
//   5. CU (uncorrectable) count
//   6. Three individual bar graphs (C1, C2, CU distribution over time)
//   7. Combined normalised overlay graph (C1 green, C2 yellow, CU red)
//   8. Final quality verdict with colour-coded rating
// ============================================================================

void OpticalDrive::PrintQCheckReport(const QCheckResult& result) {
	// The Pioneer vendor scan's E22 counter is reported as C2 (see
	// RunQCheckScan), so the report renders a Pioneer scan exactly like a
	// normal C2 scan.  The scan-method line below still identifies the source
	// as "Pioneer (0x3B/0x3C)" so the provenance of the C2 figure is clear.
	const bool pioneerScan = false;
	const bool pioneerE22Observed = false;

	// Some backends (the Pioneer vendor scan) don't measure CU at all, so a
	// zero CU total is an absence of measurement, not a clean result. Gate the
	// CU verdict/graph/heatmap on this so we never imply a check that never ran.
	const bool cuMeasured = result.cuMeasured;

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              CD QUALITY SCAN REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	// ── Section 1: Scan metadata ─────────────────────────────
	std::cout << "\n--- Scan Info ---\n";
	if (!result.scanMethod.empty())
		std::cout << "  Scan method:     " << result.scanMethod << "\n";
	std::cout << "  Samples collected: " << result.samples.size() << "\n";
	std::cout << "  Disc length:       "
		<< (result.totalSeconds / 60) << ":"
		<< std::setfill('0') << std::setw(2) << (result.totalSeconds % 60)
		<< std::setfill(' ') << " (mm:ss)\n";
	std::cout << "  Sectors covered:   " << result.totalSectors << "\n";

	// ── Section 2: C1 errors (Block Error Rate) ──────────────
	// C1 is the first level of Reed-Solomon error correction in the CIRC
	// decoder.  Red Book specifies a maximum average of 220 C1 errors per
	// second (the "BLER limit").  Values below this indicate the disc is
	// within specification; values above suggest surface degradation.
	std::cout << "\n--- C1 Errors (Block Error Rate) ---\n";
	std::cout << "  Total C1:    " << result.totalC1 << "\n";
	std::cout << "  Avg C1/sec:  " << std::fixed << std::setprecision(2)
		<< result.avgC1PerSecond;
	std::cout << (result.avgC1PerSecond < 220.0 ? "  [PASS]" : "  [FAIL]")
		<< "  (Red Book limit: 220/sec)\n";
	std::cout << "  Max C1/sec:  " << result.maxC1PerSecond;
	if (result.maxC1SecondIndex >= 0 && result.maxC1SecondIndex < static_cast<int>(result.samples.size()))
		std::cout << "  (at LBA " << result.samples[result.maxC1SecondIndex].lba << ")";

	// Warn if recent samples (near end of disc) show a large C1 spike —
	// this pattern is common in disc rot that starts at the outer edge.
	if (result.samples.size() >= 10 && result.avgC1PerSecond > 50.0) {
		int warningMargin = 5;
		for (int i = static_cast<int>(result.samples.size()) - 1; i >= 0; i--) {
			if (result.samples[i].lba < result.samples.back().lba - warningMargin)
				break;
			if (result.samples[i].c1 > result.avgC1PerSecond * 10) {
				std::cout << "  WARNING: Recent C1 spike detected (LBA "
					<< result.samples[i].lba << ", C1=" << result.samples[i].c1 << ")\n";
				break;
			}
		}
	}
	std::cout << "\n";

	// Human-readable C1 assessment based on average rate.
	if (result.avgC1PerSecond < 5.0)
		std::cout << "  C1 Assessment: EXCELLENT - minimal correction needed\n";
	else if (result.avgC1PerSecond < 50.0)
		std::cout << "  C1 Assessment: GOOD - normal wear\n";
	else if (result.avgC1PerSecond < 220.0)
		std::cout << "  C1 Assessment: FAIR - elevated but within Red Book limits\n";
	else
		std::cout << "  C1 Assessment: POOR - exceeds Red Book BLER limit\n";

	// ── Section 3: C1 load quality ───────────────────────────
	// Rate-normalized C1 quality.  Raw total is shown as context only,
	// because longer discs naturally accumulate more C1 events.
	std::cout << "\n--- C1 Load Quality ---\n";
	std::cout << "  Total C1:      " << result.totalC1 << "\n";
	std::cout << "  Avg C1/sec:    " << std::fixed << std::setprecision(2)
		<< result.avgC1PerSecond << "\n";
	std::cout << "  Interpretation: ";
	if (result.totalC1Quality == "Exceptional" || result.totalC1Quality == "Very good")
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
	else if (result.totalC1Quality == "Normal" || result.totalC1Quality == "Marginal")
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
	else
		Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
	std::cout << result.totalC1Quality;
	Console::Reset();
	if (result.totalC1Quality == "Exceptional")
		std::cout << " (rare, usually high-quality pressings)";
	else if (result.totalC1Quality == "Marginal")
		std::cout << " (still within spec)";
	else if (result.totalC1Quality == "Poor")
		std::cout << " (poor burn or aging disc)";
	std::cout << "\n";

	// ── Section 4: Archival suitability (peak C1) ────────────
	// For long-term preservation, peak C1 is more important than average —
	// a localised hot-spot may worsen over time and eventually become
	// uncorrectable.  Thresholds: <50 ideal, <100 good, ≤220 acceptable.
	std::cout << "\n--- Archival Audio (Peak C1) ---\n";
	std::cout << "  Peak C1/sec: " << result.maxC1PerSecond << "\n";
	std::cout << "  Rating:      ";
	if (result.archivalC1Rating == "Ideal" || result.archivalC1Rating == "Good")
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
	else if (result.archivalC1Rating == "Acceptable")
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
	else
		Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
	std::cout << result.archivalC1Rating;
	Console::Reset();
	if (result.archivalC1Rating == "Ideal")
		std::cout << " (< 50/sec)";
	else if (result.archivalC1Rating == "Good")
		std::cout << " (< 100/sec)";
	else if (result.archivalC1Rating == "Acceptable")
		std::cout << " (100-220/sec, not ideal for archival)";
	else
		std::cout << " (exceeds Red Book limit)";
	std::cout << "\n";

	// ── Section 5: C2 errors ─────────────────────────────────
	// C2 errors indicate the first-level (C1) correction failed and the
	// drive needed the second Reed-Solomon stage.  Any non-zero C2 count
	// is a concern; sustained high C2 rates indicate significant damage.
	if (pioneerScan) {
		std::cout << "\n--- Verified C2 Errors ---\n";
		std::cout << "  Verified C2 in Q-Check: NOT CHECKED by Pioneer vendor scan\n";
		std::cout << "  Copy decision:          Use Disc Rot Phase 1 / C2 scan for yes-or-no C2.\n";
		std::cout << "\n--- Pioneer E22 Diagnostic (Not a copy trigger) ---\n";
		std::cout << "  Meaning: Raw Pioneer firmware counter; common at low levels.\n";
		std::cout << "  Rating:      ";
		if (result.pioneerE22Rating == "Ideal" || result.pioneerE22Rating == "Good")
			Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
		else if (result.pioneerE22Rating == "Acceptable")
			Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
		else
			Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
		std::cout << result.pioneerE22Rating;
		Console::Reset();
		std::cout << " (" << PioneerE22RatingDescription(result.pioneerE22Rating) << ")\n";
		std::cout << "  Total E22:   " << result.totalPioneerE22 << "\n";
		std::cout << "  Avg E22/sec: " << std::fixed << std::setprecision(2)
			<< result.avgPioneerE22PerSecond << "\n";
		std::cout << "  Max E22/sec: " << result.maxPioneerE22PerSecond;
		if (result.maxPioneerE22SecondIndex >= 0 &&
			result.maxPioneerE22SecondIndex < static_cast<int>(result.samples.size()))
			std::cout << "  (at LBA " << result.samples[result.maxPioneerE22SecondIndex].lba << ")";
		std::cout << "\n";
	}
	else {
		std::cout << "\n--- C2 Errors ---\n";
		std::cout << "  Total C2:    " << result.totalC2 << "\n";
		std::cout << "  Avg C2/sec:  " << std::fixed << std::setprecision(2)
			<< result.avgC2PerSecond << "\n";
		std::cout << "  Max C2/sec:  " << result.maxC2PerSecond;
		if (result.maxC2SecondIndex >= 0 && result.maxC2SecondIndex < static_cast<int>(result.samples.size()))
			std::cout << "  (at LBA " << result.samples[result.maxC2SecondIndex].lba << ")";

		// Classify C2 severity by average rate.
		if (result.totalC2 > 0) {
			if (result.avgC2PerSecond < 1.0)
				std::cout << "  (few, isolated spikes)";
			else if (result.avgC2PerSecond < 10.0)
				std::cout << "  (moderate spike activity)";
			else
				std::cout << "  (significant, sustained errors)";
		}
		std::cout << "\n";

		if (result.totalC2 == 0)
			std::cout << "  C2 Assessment: PERFECT - no C2 correction needed\n";
		else if (result.avgC2PerSecond < 1.0)
			std::cout << "  C2 Assessment: ACCEPTABLE - few C2 corrections (C1 fallthrough)\n";
		else if (result.avgC2PerSecond < 10.0)
			std::cout << "  C2 Assessment: FAIR - moderate C2 correction load\n";
		else
			std::cout << "  C2 Assessment: POOR - heavy C2 correction load\n";
	}

	// List which track(s) the C2 errors fell in.  Useful for narrowing
	// damage to a specific song so the user can re-rip / replace just that
	// portion of the disc.
	if (!pioneerScan && result.totalC2 > 0) {
		bool first = true;
		std::cout << "  Affected tracks:";
		for (const auto& te : result.errorTracks) {
			if (te.c2Count == 0) continue;
			std::cout << (first ? " " : ", ") << te.trackNumber
				<< " (" << te.c2Count << " C2)";
			first = false;
		}
		if (first) std::cout << " (none mapped - sample LBAs outside any track range)";
		std::cout << "\n";
	}

	// ── Section 6: CU (uncorrectable errors) ─────────────────
	// CU means both C1 and C2 correction failed — the original data is
	// lost.  Any CU > 0 means audible glitches or interpolated samples.
	std::cout << "\n--- CU (Uncorrectable) ---\n";
	if (cuMeasured) {
		std::cout << "  Total CU:    " << result.totalCU << "\n";
		std::cout << "  Max CU/sec:  " << result.maxCUPerSecond << "\n";
		if (result.totalCU == 0)
			std::cout << "  CU Assessment: PERFECT - all errors were correctable\n";
		else
			std::cout << "  CU Assessment: BAD - data loss likely in " << result.totalCU << " sector(s)\n";
	}
	else if (result.pioneerCdCheckRun) {
		// The vendor scan has no CU counter, but the Pioneer CD Check (0xE6)
		// cross-check measured real uncorrectable data over the same range.
		std::cout << "  Source: Pioneer CD Check (0xE6) uncorrectable cross-check\n";
		std::cout << "  C1 uncorrectable frames: " << result.pioneerCdCheckC1Frames
			<< "  (worst window)\n";
		std::cout << "  C2 uncorrectable bytes:  " << result.pioneerCdCheckC2Bytes
			<< "  (worst window)\n";
		if (result.pioneerCdCheckC2Bytes == 0)
			std::cout << "  CU Assessment: GOOD - no uncorrectable (C2) data reported\n";
		else
			std::cout << "  CU Assessment: BAD - uncorrectable data present; data loss likely\n";
	}
	else if (result.pioneerCdCheckSkippedClean) {
		// The vendor scan found the disc pristine, so the uncorrectable pass was
		// skipped by design (nothing for it to find). Say so — this is a
		// reasoned skip, not an unknown.
		std::cout << "  CU Assessment: NOT CHECKED - disc scanned clean\n";
		std::cout << "  The vendor scan found no C2/E22 activity and low C1, so uncorrectable\n";
		std::cout << "  (E32) errors are not possible here and the extra CD Check pass was\n";
		std::cout << "  skipped to save time. Use Pioneer CD Check (option 28) to force one.\n";
	}
	else {
		// Pioneer vendor scan with no CD Check available: CU is unknown, not
		// zero. Say so plainly and point to the backends that can answer the
		// copy/data-loss question.
		std::cout << "  CU Assessment: NOT MEASURED by this scan backend\n";
		std::cout << "  The Pioneer 0x3B/0x3C vendor scan reports C1 and E22 only, and this\n";
		std::cout << "  drive's firmware did not answer the CD Check (0xE6) cross-check, so\n";
		std::cout << "  uncorrectable errors could not be measured. A zero here would be the\n";
		std::cout << "  absence of a measurement, not a clean result.\n";
		std::cout << "  For a real uncorrectable/data-loss check, use the C2 error scan (option 7).\n";
	}

	if (cuMeasured && result.totalCU > 0) {
		bool first = true;
		std::cout << "  Affected tracks:";
		for (const auto& te : result.errorTracks) {
			if (te.cuCount == 0) continue;
			std::cout << (first ? " " : ", ") << te.trackNumber
				<< " (" << te.cuCount << " CU)";
			first = false;
		}
		if (first) std::cout << " (none mapped - sample LBAs outside any track range)";
		std::cout << "\n";
	}

	// ── Section 7: Distribution graphs ───────────────────────
	// Draw per-metric bar graphs showing error distribution over time.
	// Each column represents a time-bucketed average; height shows the
	// error rate.  The C1 graph includes a reference line at 220/sec
	// (Red Book BLER limit).
	if (!result.samples.empty()) {
		constexpr int GRAPH_WIDTH = 60;   // Columns in the bar graph
		constexpr int GRAPH_HEIGHT = 12;  // Rows in the bar graph

		// Extract per-metric value arrays for bucketing.
		std::vector<int> c1Vals, c2Vals, cuVals;
		for (const auto& s : result.samples) {
			c1Vals.push_back(s.c1);
			c2Vals.push_back(s.c2);
			cuVals.push_back(s.cu);
		}

		// ── C1 distribution graph ────────────────────────────
		int peakC1 = *std::max_element(c1Vals.begin(), c1Vals.end());
		if (peakC1 > 0) {
			// Y-axis minimum of 250 ensures the 220/sec reference line is
			// always visible even on pristine discs with very low C1.
			int graphMax = std::max(peakC1, 250);
			auto buckets = Console::BucketData(c1Vals, GRAPH_WIDTH);
			Console::GraphOptions opts;
			opts.title = "C1 Error Distribution (BLER)";
			opts.subtitle = "Each column = a time slice; height = C1 errors/sec";
			opts.width = GRAPH_WIDTH;
			opts.height = GRAPH_HEIGHT;
			opts.refLine = 220;                              // Red Book BLER limit
			opts.refLabel = "Red Book BLER limit (220/sec)";
			Console::DrawBarGraph(buckets, graphMax, opts, result.totalSeconds);
		}

		// ── C2 distribution graph ────────────────────────────
		if (!pioneerScan) {
			int peakC2 = *std::max_element(c2Vals.begin(), c2Vals.end());
			if (peakC2 > 0) {
				auto buckets = Console::BucketData(c2Vals, GRAPH_WIDTH);
				Console::GraphOptions opts;
				opts.title = "C2 Error Distribution";
				opts.subtitle = "Each column = a time slice; height = C2 errors/sec";
				opts.width = GRAPH_WIDTH;
				opts.height = GRAPH_HEIGHT;
				opts.severityLowThreshold = 5;
				opts.severityHighThreshold = 20;
				opts.severityLowLabel = "1-4/sec low";
				opts.severityModerateLabel = "5-19/sec moderate";
				opts.severityHighLabel = "20+/sec high";
				Console::DrawBarGraph(buckets, peakC2, opts, result.totalSeconds);
			}
			else {
				Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
				std::cout << "\n  " << Console::Sym::Check << " No C2 errors.\n";
				Console::Reset();
			}
		}

		// ── CU distribution graph ────────────────────────────
		// Skip entirely when the backend doesn't measure CU: an all-zero graph
		// or a green "No CU events" check would both misrepresent unmeasured
		// data as a clean result.
		if (cuMeasured) {
			int peakCU = *std::max_element(cuVals.begin(), cuVals.end());
			if (peakCU > 0) {
				auto buckets = Console::BucketData(cuVals, GRAPH_WIDTH);
				Console::GraphOptions opts;
				opts.title = "CU (Uncorrectable) Distribution";
				opts.subtitle = "Each column = a time slice; height = CU events/sec";
				opts.width = GRAPH_WIDTH;
				opts.height = GRAPH_HEIGHT;
				Console::DrawBarGraph(buckets, peakCU, opts, result.totalSeconds);
			}
			else {
				Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
				std::cout << "\n  " << Console::Sym::Check << " No CU events.\n";
				Console::Reset();
			}
		}

		// ── Section 8: Disc health heatmap ────────────────────
		// One row per verdict-driving metric. Each cell is tiered by an
		// absolute threshold (not peak-normalised), so a healthy disc
		// reads almost entirely "none" and trouble spots stand out by
		// colour rather than relative height. Pioneer E22 is raw vendor
		// diagnostic data, so it is intentionally excluded.
		const bool includeC2InCombined = !pioneerScan;

		std::vector<Console::HeatmapRow> heat;
		{
			Console::HeatmapRow r;
			r.label = "C1";
			r.values = Console::BucketData(c1Vals, GRAPH_WIDTH);
			r.lowThresh = 50;      // 1-49/sec = low
			r.highThresh = 220;    // ≥220/sec = high (Red Book limit)
			heat.push_back(std::move(r));
		}
		if (includeC2InCombined) {
			Console::HeatmapRow r;
			r.label = "C2";
			r.values = Console::BucketData(c2Vals, GRAPH_WIDTH);
			r.lowThresh = 5;
			r.highThresh = 20;
			heat.push_back(std::move(r));
		}
		if (cuMeasured) {
			Console::HeatmapRow r;
			r.label = "CU";
			r.values = Console::BucketData(cuVals, GRAPH_WIDTH);
			r.lowThresh = 2;       // any CU is a concern
			r.highThresh = 5;
			heat.push_back(std::move(r));
		}

		const std::string subtitle = pioneerScan
			? "Each column = a time slice; cell colour = error severity (Pioneer E22 excluded)"
			: "Each column = a time slice; cell colour = error severity";

		// Build the "(C1, C2, CU)" title from the rows actually shown so it can't
		// advertise a CU row the backend never measured.
		std::string heatTitle = "Disc Health Map (C1";
		if (includeC2InCombined) heatTitle += ", C2";
		if (cuMeasured) heatTitle += ", CU";
		heatTitle += ")";

		Console::DrawHeatmap(heat, heatTitle, subtitle, result.totalSeconds);

		// Peak-value caption beneath the heatmap.
		Console::SetColorRGB(Console::Theme::DimR, Console::Theme::DimG, Console::Theme::DimB);
		std::cout << "  Peak: C1 " << result.maxC1PerSecond << "/sec";
		if (includeC2InCombined) std::cout << "   C2 " << result.maxC2PerSecond << "/sec";
		if (cuMeasured) std::cout << "   CU " << result.maxCUPerSecond << "/sec";
		if (pioneerE22Observed)
			std::cout << "   E22 (diagnostic) " << result.maxPioneerE22PerSecond << "/sec";
		std::cout << "\n";
		Console::Reset();
	}

	// ── Final verdict ────────────────────────────────────────
	// Colour-coded overall quality rating with actionable advice.
	Console::SetColorRGB(Console::Theme::DimR, Console::Theme::DimG, Console::Theme::DimB);
	std::cout << "\n" << std::string(60, '-') << "\n";

	std::string qr = result.qualityRating;
	if (qr == "EXCELLENT" || qr == "GOOD")
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
	else if (qr == "FAIR" || qr == "UNVERIFIED")
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
	else
		Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);

	std::cout << (pioneerScan ? "  C1/CU QUALITY: " : "  QUALITY:       ")
		<< qr << "\n";
	Console::Reset();

	// Repeat total-C1 and archival ratings in the summary block so the
	// user doesn't have to scroll back to the detailed sections.
	std::cout << "  Total C1:       ";
	if (result.totalC1Quality == "Exceptional" || result.totalC1Quality == "Very good")
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
	else if (result.totalC1Quality == "Normal" || result.totalC1Quality == "Marginal")
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
	else
		Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
	std::cout << result.totalC1Quality;
	Console::Reset();
	std::cout << " (" << result.totalC1 << " total)\n";

	std::cout << "  Archival Peak:  ";
	if (result.archivalC1Rating == "Ideal" || result.archivalC1Rating == "Good")
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
	else if (result.archivalC1Rating == "Acceptable")
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
	else
		Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
	std::cout << result.archivalC1Rating;
	Console::Reset();
	std::cout << " (peak " << result.maxC1PerSecond << "/sec)\n";

	if (result.totalC2 > 0) {
		std::cout << "  C2:            ";
		if (result.avgC2PerSecond < 1.0)
			Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
		else
			Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
		if (result.avgC2PerSecond < 1.0)
			std::cout << "Isolated";
		else if (result.avgC2PerSecond < 10.0)
			std::cout << "Moderate";
		else
			std::cout << "Heavy";
		Console::Reset();
		std::cout << " (" << result.totalC2 << " total, peak "
			<< result.maxC2PerSecond << "/sec)\n";
	}
	if (pioneerE22Observed) {
		std::cout << "  Pioneer E22:   ";
		if (result.pioneerE22Rating == "Ideal" || result.pioneerE22Rating == "Good")
			Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
		else if (result.pioneerE22Rating == "Acceptable")
			Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
		else
			Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
		std::cout << result.pioneerE22Rating;
		Console::Reset();
		std::cout << " (" << result.totalPioneerE22
			<< " total, peak " << result.maxPioneerE22PerSecond << "/sec)\n";
	}
	if (pioneerScan) {
		std::cout << "  Copy Decision: NOT TESTED by Pioneer Q-Check\n";
	}

	Console::SetColorRGB(Console::Theme::DimR, Console::Theme::DimG, Console::Theme::DimB);
	std::cout << std::string(60, '-') << "\n";
	Console::Reset();

	// Actionable recommendation based on the overall rating.
	if (qr == "EXCELLENT") {
		if (pioneerScan)
			std::cout << "  C1/CU scan is excellent, but copyability was not tested here.\n";
		else
			std::cout << "  Disc is in excellent condition.\n";
	}
	else if (qr == "GOOD") {
		if (pioneerScan)
			std::cout << "  C1/CU scan shows normal wear, but copyability was not tested here.\n";
		else
			std::cout << "  Normal wear, no concerns.\n";
	}
	else if (qr == "FAIR") {
		if (result.totalC2 > 0)
			std::cout << "  C2 correction activity detected. Clean and verify with another scan.\n";
		else
			std::cout << "  Elevated C1 rate. Consider cleaning the disc.\n";
	}
	else if (qr == "POOR")
		std::cout << "  Significant errors. Back up this disc.\n";
	else if (qr == "UNVERIFIED")
		std::cout << "  Results could not be verified - see warning below.\n";
	else
		std::cout << "  Critical errors detected. Data loss likely.\n";
	if (pioneerScan)
		std::cout << "  Run Disc Rot Phase 1 / C2 scan or an actual copy/read verification for the copy decision.\n";
	std::cout << std::string(60, '=') << "\n";

	// ── Drive compatibility warning ──────────────────────────
	if (result.c1Unverified) {
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
		std::cout << "\n  ** WARNING: Zero C1 errors across entire disc. **\n";
		Console::Reset();
		std::cout << "     This is extremely unlikely on any real CD.\n"
			<< "     Your drive may have accepted the quality scan command\n"
			<< "     but does not actually report CIRC error statistics.\n"
			<< "     Quality scan results from this drive are unreliable.\n\n"
			<< "     Suggestions:\n"
			<< "       - Try the C2 error scan (option 7) instead.\n"
			<< "       - Use a known-compatible drive (Plextor PX-708A\n"
			<< "         through PX-760A, or LiteOn/MediaTek-based).\n";
	}
}

// ============================================================================
// SaveQCheckLog — Export scan data to CSV for external analysis
// ============================================================================
// Format: commented header block with summary statistics, followed by a
// CSV table with one row per time-slice sample (Time, Second, LBA, C1, C2, CU).
// The header uses '#' prefixes so it's ignored by most CSV parsers but
// readable when opened in a text editor.
// ============================================================================
bool OpticalDrive::SaveQCheckLog(const QCheckResult& result, const std::wstring& filename) {
	std::ofstream log(std::filesystem::path(filename), std::ios::out | std::ios::trunc);
	if (!log) return false;
	// Pioneer's E22 counter is logged as C2 (see RunQCheckScan), so the CSV is a
	// standard C1/C2/CU log regardless of backend. The scan-method field still
	// records "Pioneer (0x3B/0x3C)" for provenance.
	const bool pioneerScan = false;

	// ── Header block: summary statistics ─────────────────────
	// Written as '#'-prefixed comments so CSV parsers skip them but
	// humans can read the summary without importing into a spreadsheet.
	log << "# ==============================\n";
	log << "# CD Quality Scan Log\n";
	log << "# ==============================\n";
	log << "#\n";
	if (!result.scanMethod.empty())
		log << "# Scan Method:           " << result.scanMethod << "\n";
	log << (pioneerScan ? "# C1/CU Quality:         " : "# Quality Rating:        ")
		<< result.qualityRating << "\n";
	if (pioneerScan)
		log << "# Copy Decision:         NOT TESTED by Pioneer Q-Check\n";
	log << "# Total Sectors:         " << result.totalSectors << "\n";
	log << "# Samples Collected:     " << result.samples.size() << "\n";
	log << "# Disc Length:           "
		<< (result.totalSeconds / 60) << ":"
		<< std::setfill('0') << std::setw(2) << (result.totalSeconds % 60)
		<< std::setfill(' ') << " (mm:ss)\n";
	log << "#\n";
	log << "# --- C1 Statistics ---\n";
	log << "# Total C1:              " << result.totalC1 << "\n";
	log << "# Avg C1/sec:            " << std::fixed << std::setprecision(2)
		<< result.avgC1PerSecond << "\n";
	log << "# Max C1/sec:            " << result.maxC1PerSecond;
	if (result.maxC1SecondIndex >= 0 && result.maxC1SecondIndex < static_cast<int>(result.samples.size()))
		log << " (at LBA " << result.samples[result.maxC1SecondIndex].lba << ")";
	log << "\n";
	log << "# Avg C1/sec Pass:       " << (result.avgC1PerSecond < 220.0 ? "PASS" : "FAIL")
		<< " (Red Book limit: 220/sec)\n";
	log << "#\n";
	log << "# --- C2 Statistics ---\n";
	if (pioneerScan)
		log << "# Verified C2:           Not checked by Pioneer Q-Check vendor scan\n";
	log << "# Total C2:              " << result.totalC2 << "\n";
	log << "# Avg C2/sec:            " << std::fixed << std::setprecision(2)
		<< result.avgC2PerSecond << "\n";
	log << "# Max C2/sec:            " << result.maxC2PerSecond;
	if (result.maxC2SecondIndex >= 0 && result.maxC2SecondIndex < static_cast<int>(result.samples.size()))
		log << " (at LBA " << result.samples[result.maxC2SecondIndex].lba << ")";
	log << "\n";
	if (result.totalC2 > 0) {
		log << "# C2 Affected Tracks:    ";
		bool first = true;
		for (const auto& te : result.errorTracks) {
			if (te.c2Count == 0) continue;
			log << (first ? "" : ", ") << te.trackNumber << "(" << te.c2Count << ")";
			first = false;
		}
		if (first) log << "(none mapped)";
		log << "\n";
	}
	if (pioneerScan) {
		log << "#\n";
		log << "# --- Pioneer E22 Diagnostic (Not a copy trigger) ---\n";
		log << "# Meaning:               Raw Pioneer firmware counter; common at low levels\n";
		log << "# Rating:                " << result.pioneerE22Rating
			<< " (" << PioneerE22RatingDescription(result.pioneerE22Rating) << ")\n";
		log << "# Total E22:             " << result.totalPioneerE22 << "\n";
		log << "# Avg E22/sec:           " << std::fixed << std::setprecision(2)
			<< result.avgPioneerE22PerSecond << "\n";
		log << "# Max E22/sec:           " << result.maxPioneerE22PerSecond;
		if (result.maxPioneerE22SecondIndex >= 0 &&
			result.maxPioneerE22SecondIndex < static_cast<int>(result.samples.size()))
			log << " (at LBA " << result.samples[result.maxPioneerE22SecondIndex].lba << ")";
		log << "\n";
	}
	log << "#\n";
	log << "# --- CU Statistics ---\n";
	if (!result.cuMeasured) {
		if (result.pioneerCdCheckRun) {
			// Vendor scan has no per-slice CU, but the Pioneer CD Check (0xE6)
			// cross-check measured real uncorrectable data over the same range.
			log << "# CU Measured:           via Pioneer CD Check (0xE6) cross-check\n";
			log << "# CDCheck C1 uncorr:     " << result.pioneerCdCheckC1Frames << " frames (worst window)\n";
			log << "# CDCheck C2 uncorr:     " << result.pioneerCdCheckC2Bytes << " bytes (worst window)\n";
			log << "# Uncorrectable verdict: "
				<< (result.pioneerCdCheckC2Bytes == 0 ? "GOOD - none reported"
					: "BAD - uncorrectable data present")
				<< "\n";
			log << "#                        (per-slice CU column below is 0 by omission)\n";
		}
		else if (result.pioneerCdCheckSkippedClean) {
			// Disc scanned pristine, so the extra uncorrectable pass was skipped.
			log << "# CU Measured:           NOT CHECKED - disc scanned clean (no C2/E22, low C1)\n";
			log << "#                        (uncorrectable errors not possible; CD Check pass skipped)\n";
		}
		else {
			// Pioneer vendor scan: CU is not measured, so the 0s below and in the
			// per-sample CU column are placeholders, not a passed uncorrectable check.
			log << "# CU Measured:           NO - Pioneer vendor scan reports C1/E22 only\n";
			log << "#                        (CU column below is 0 by omission, not measurement)\n";
		}
	}
	log << "# Total CU:              " << result.totalCU << "\n";
	log << "# Max CU/sec:            " << result.maxCUPerSecond << "\n";
	if (result.totalCU > 0) {
		log << "# CU Affected Tracks:    ";
		bool first = true;
		for (const auto& te : result.errorTracks) {
			if (te.cuCount == 0) continue;
			log << (first ? "" : ", ") << te.trackNumber << "(" << te.cuCount << ")";
			first = false;
		}
		if (first) log << "(none mapped)";
		log << "\n";
	}
	log << "#\n";
	log << "# --- C1 Load Quality ---\n";
	log << "# Total C1 Quality:      " << result.totalC1Quality << "\n";
	log << "#\n";
	log << "# --- Archival Audio ---\n";
	log << "# Peak C1 Rating:        " << result.archivalC1Rating << "\n";
	if (result.c1Unverified) {
		log << "#\n";
		log << "# *** WARNING: Zero C1 errors across entire disc.        ***\n";
		log << "# *** Quality scan results may be unreliable - the drive ***\n";
		log << "# *** may not actually populate CIRC error statistics.   ***\n";
	}
	log << "#\n";

	// ── Per-sample CSV data ──────────────────────────────────
	// One row per time slice.  "Time" is formatted as M:SS for human
	// readability; "Second" is the zero-based sample index for plotting.
	log << "# ==============================\n";
	log << (pioneerScan ? "# Per-Second C1/C2/CU/PioneerE22 Data\n" : "# Per-Second C1/C2/CU Data\n");
	log << "# ==============================\n";
	log << (pioneerScan ? "Time,Second,LBA,C1,C2,CU,PioneerE22\n" : "Time,Second,LBA,C1,C2,CU\n");

	for (size_t i = 0; i < result.samples.size(); i++) {
		const auto& s = result.samples[i];
		int minutes = static_cast<int>(i) / 60;
		int seconds = static_cast<int>(i) % 60;

		log << minutes << ":" << std::setfill('0') << std::setw(2) << seconds
			<< std::setfill(' ')
			<< "," << i
			<< "," << s.lba
			<< "," << s.c1
			<< "," << s.c2
			<< "," << s.cu;
		if (pioneerScan)
			log << "," << s.pioneerE22;
		log << "\n";
	}

	log.flush();
	return log.good();
}
