#define NOMINMAX
#include "OpticalDrive.h"
#include "QualityScanSession.h"
#include "DiagnosticAssessment.h"
#include "DiscBalanceAssessment.h"
#include "InterruptHandler.h"
#include "PioneerVendor.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>


// ============================================================================
// Disc Balance Check - Detects vibration / wobble by sweeping read speed
// ============================================================================

bool OpticalDrive::CheckDiscBalance(DiscInfo& disc, int& balanceScore) {
	balanceScore = 0;
	DriveDoorLockGuard doorLock(m_drive);

	// Pioneer PureRead interpolates/re-reads to hide errors after retries, which
	// would mask the per-sector C2 variance this balance check relies on to detect
	// wobble. Force it Off for the whole sweep (restored on scope exit); no-ops on
	// non-Pioneer drives. Consistent with the Q-Check, C2, BLER and Disc-Rot scans.
	// NOTE: unlike those scans we deliberately do NOT force Performance speed mode
	// here — this check sweeps read speed on purpose, so the drive's speed must
	// stay under our control.
	PioneerVendor pioneerProbe(m_drive);
	const bool isPioneerDrive = pioneerProbe.IsPioneerDrive();
	ScopedDriveSpeed restoreSpeed(m_drive);
	PioneerPureReadOffGuard pioneerPureReadGuard(m_drive, isPioneerDrive);
	PioneerCdCheckSummary balanceCdCheck;
	bool balanceCdCheckAttempted = false;

	// Probe hardware quality-scan availability early — these probes print
	// diagnostics, so do them before the progress bar starts.  Prefer
	// Pioneer on Pioneer drives to avoid irrelevant LiteOn probe chatter.
	bool hasPioneerHwC1 = m_drive.SupportsPioneerScan();
	bool hasReadCdC2 = m_drive.CheckC2Support();
	bool hasLiteOnHwC1 = !hasPioneerHwC1 && m_drive.SupportsLiteOnScan();
	if (!m_drive.IsOpen()) { std::cout << "ERROR: Drive closed after scan cleanup failed. Reopen it.\n"; return false; }
	bool hasHwC1 = hasPioneerHwC1 || hasLiteOnHwC1;
	if (!hasReadCdC2 && !hasHwC1) {
		std::cout << "ERROR: Disc balance check requires READ CD C2 or a supported\n"
			<< "       Pioneer/LiteOn hardware quality-scan backend.\n";
		return false;
	}
	if (!hasReadCdC2) {
		std::cout << "  NOTE: READ CD C2 is unavailable; the timing sweep will use\n"
			<< "        audio-only reads and the hardware quality scan.\n";
	}

	const int speeds[] = { 4, 8, 16, 24, 32, 40 };
	const int NUM_SPEEDS = sizeof(speeds) / sizeof(speeds[0]);
	const int SAMPLE_COUNT = 50;
	constexpr int READS_PER_SAMPLE = 3;

	// Distance to stay back from target LBA when pre-positioning the head.
	// Must exceed the drive's read-ahead buffer (typically 64-256 KB = 27-109
	// sectors) so the target sector is NOT prefetched into cache.
	constexpr DWORD READ_AHEAD_MARGIN = 150;

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) return false;

	// Build a flat list of all sample LBAs spaced evenly across the disc
	std::vector<DWORD> sampleLBAs;
	DWORD maxLBA = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio && t.endLBA > maxLBA) maxLBA = t.endLBA;
	}
	// Build sample LBAs with outer-edge bias: use a quadratic distribution
	// so ~60% of samples fall in the outer 40% of the disc, where wobble
	// effects are strongest (centrifugal force ∝ radius²).
	for (int i = 0; i < SAMPLE_COUNT && maxLBA > 0; i++) {
        double t = 0.0;
        if constexpr (SAMPLE_COUNT > 1)
            t = static_cast<double>(i) / static_cast<double>(SAMPLE_COUNT - 1);
		// Concave bias toward outer edge: 1-(1-t)² = 2t - t²
		// maps [0,1] → [0,1] with higher sample density near 1.0
		double biased = 2.0 * t - t * t;
		// Blend 50% uniform + 50% concave to keep some inner coverage
		double blended = 0.5 * t + 0.5 * biased;
		DWORD lba = static_cast<DWORD>(blended * maxLBA);

		for (const auto& tr : disc.tracks) {
			if (!tr.isAudio) continue;
			DWORD start = (tr.trackNumber == 1) ? 0 : tr.pregapLBA;
			if (lba >= start && lba <= tr.endLBA) {
				sampleLBAs.push_back(lba);
				break;
			}
		}
	}
	if (sampleLBAs.empty()) return false;

	// Actual sample count may be less than SAMPLE_COUNT on mixed-mode discs
	int totalTests = NUM_SPEEDS * static_cast<int>(sampleLBAs.size()) * READS_PER_SAMPLE;
	int completed = 0;

	std::cout << "\nSweeping " << sampleLBAs.size() << " sample sectors across "
		<< NUM_SPEEDS << " speeds (" << READS_PER_SAMPLE << " reads each)...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Balance");
	progress.Start();

	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
	// The balance error signal combines READ CD C2 pointer counts (when
	// available) with explicit penalties for unstable or failed reads. Keep the
	// raw C2 rate separate so synthetic timing penalties are never presented as
	// measured C2 data.
	std::vector<double> avgReadErrorSignalPerSpeed(NUM_SPEEDS, 0.0);
	std::vector<double> avgReadCdC2PerSpeed(NUM_SPEEDS, 0.0);
	std::vector<double> jitterCoeffVar(NUM_SPEEDS, 0.0);
	std::vector<double> avgReadTimeMs(NUM_SPEEDS, 0.0);
	std::vector<double> avgStabilityRatio(NUM_SPEEDS, 0.0);
	std::vector<bool> stabilityMeasured(NUM_SPEEDS, false);
	std::vector<int> validReadSamplesPerSpeed(NUM_SPEEDS, 0);
	// Actual read speed (x) the drive ran at each requested step, from the MODE
	// SENSE readback. Many drives clamp low requests to a floor (e.g. 4x/8x
	// both run at 10x); capturing the real value lets the report say so. 0 =
	// not captured.
	std::vector<int> actualSpeedX(NUM_SPEEDS, 0);

	for (int s = 0; s < NUM_SPEEDS; s++) {
		m_drive.SetSpeed(speeds[s]);
		// Capture the speed the drive actually settled on (one MODE SENSE read,
		// no verify-retry loop): on a drive that clamps low requests, a verify
		// against the requested speed would deliberately fail and waste three
		// retries. We only want the readback, not a match.
		{
			WORD actualKBps = 0, actualWriteKBps = 0;
			if (m_drive.GetActualSpeed(actualKBps, actualWriteKBps) && actualKBps > 0)
				actualSpeedX[s] = (static_cast<int>(actualKBps) + CD_SPEED_1X / 2) / CD_SPEED_1X;
		}
		Sleep(200); // Let the drive stabilize at new speed

		int totalReadErrorSignal = 0, totalReadCdC2 = 0, tested = 0;
		std::vector<double> readTimesMs;
		std::vector<DWORD> successfulReadLBAs;
		readTimesMs.reserve(sampleLBAs.size());
		successfulReadLBAs.reserve(sampleLBAs.size());
		double stabilitySum = 0.0;
		int stabilityCount = 0;

		for (DWORD lba : sampleLBAs) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				std::cout << "\n\n*** Balance check cancelled by user ***\n";
				m_drive.SetSpeed(0);
				m_drive.SpinDown();
				progress.Finish(false);
				return false;
			}

			// Take the minimum-time successful read across READS_PER_SAMPLE
			// attempts to strip rotational latency noise, leaving drive
			// behavior as the dominant signal.
			double bestMs = (std::numeric_limits<double>::max)();
			double worstMs = 0.0;
			int bestReadCdC2 = 0;
			bool anyOk = false;
			int okCount = 0;

			for (int r = 0; r < READS_PER_SAMPLE; r++) {
				int c2tmp = 0;
				DefeatDriveCache(lba, maxLBA);
				DWORD positionLBA = (lba > READ_AHEAD_MARGIN)
					? lba - READ_AHEAD_MARGIN
					: lba + READ_AHEAD_MARGIN;
				m_drive.ReadSectorAudioOnly(positionLBA, buf.data());

				auto t0 = std::chrono::high_resolution_clock::now();
				bool ok = hasReadCdC2
					? m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2tmp)
					: m_drive.ReadSectorAudioOnly(lba, buf.data());
				auto t1 = std::chrono::high_resolution_clock::now();
				double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

				if (ok) {
					anyOk = true;
					okCount++;
					if (ms < bestMs) {
						bestMs = ms;
						bestReadCdC2 = c2tmp;
					}
					if (ms > worstMs) worstMs = ms;
				}

				completed++;
				progress.Update(completed, totalTests);
			}

			if (anyOk) {
				readTimesMs.push_back(bestMs);
				successfulReadLBAs.push_back(lba);
				validReadSamplesPerSpeed[s]++;
				// Use worst/best ratio as a per-sector wobble indicator
				if (bestMs > 0.001 && worstMs / bestMs > 3.0)
					totalReadErrorSignal += 50;  // Synthetic balance penalty, not C2
				totalReadCdC2 += bestReadCdC2;
				totalReadErrorSignal += bestReadCdC2;

				// Track per-sector read stability: worst/best ratio.
				// Wobble causes the same sector to read at wildly different
				// times on successive attempts due to servo hunting.
				if (okCount >= 2 && bestMs > 0.001) {
					stabilitySum += worstMs / bestMs;
					stabilityCount++;
				}
			}
			else {
				totalReadErrorSignal += 100; // Read-failure penalty, not C2
			}
			tested++;
		}
		avgReadErrorSignalPerSpeed[s] = (tested > 0)
			? static_cast<double>(totalReadErrorSignal) / tested : 0.0;
		avgReadCdC2PerSpeed[s] = (tested > 0)
			? static_cast<double>(totalReadCdC2) / tested : 0.0;

		// Coefficient of variation = stddev / mean (dimensionless, comparable across speeds)
		// Trimmed mean + CV: drop top/bottom 10% to resist OS/USB outliers
		if (readTimesMs.size() >= 5) {
			std::vector<double> sorted = readTimesMs;
			std::sort(sorted.begin(), sorted.end());
			size_t trimCount = sorted.size() / 10;  // 10% from each tail
			if (trimCount == 0) trimCount = 1;       // always trim at least 1

			double trimSum = 0.0;
			size_t trimN = 0;
			for (size_t i = trimCount; i < sorted.size() - trimCount; i++) {
				trimSum += sorted[i];
				trimN++;
			}

			double trimMean = trimSum / trimN;
			double trimVarSum = 0.0;
			for (size_t i = trimCount; i < sorted.size() - trimCount; i++) {
				double diff = sorted[i] - trimMean;
				trimVarSum += diff * diff;
			}
			double trimStddev = std::sqrt(trimVarSum / (trimN - 1));
			jitterCoeffVar[s] = (trimMean > 0.001) ? (trimStddev / trimMean) : 0.0;
			avgReadTimeMs[s] = trimMean;
		}
		else if (readTimesMs.size() >= 2) {
			// Too few samples to trim — fall back to raw stats
			double sum = 0.0;
			for (double t : readTimesMs) sum += t;
			double mean = sum / readTimesMs.size();
			double varSum = 0.0;
			for (double t : readTimesMs) {
				double diff = t - mean;
				varSum += diff * diff;
			}
			double stddev = std::sqrt(varSum / (readTimesMs.size() - 1));
			jitterCoeffVar[s] = (mean > 0.001) ? (stddev / mean) : 0.0;
			avgReadTimeMs[s] = mean;
		}

		// CAV detrend: at high speeds, remove the inner-to-outer gradient
		// so that normal CAV positional variance doesn't inflate jitter.
		// Successful LBA/time pairs remain aligned even when intervening reads fail.
		if (speeds[s] >= 16 && readTimesMs.size() >= 10) {
			// Build paired (LBA, time) and sort by time to trim outliers
			struct Sample { double lba; double ms; };
			std::vector<Sample> samples(readTimesMs.size());
			for (size_t i = 0; i < readTimesMs.size(); i++) {
				samples[i] = { static_cast<double>(successfulReadLBAs[i]), readTimesMs[i] };
			}
			std::sort(samples.begin(), samples.end(),
				[](const Sample& a, const Sample& b) { return a.ms < b.ms; });

			size_t trimN = samples.size() / 10;
			if (trimN == 0) trimN = 1;
			size_t lo = trimN, hi = samples.size() - trimN;

			// Linear regression on trimmed data, using LBA as X
			double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
			size_t n = hi - lo;
			for (size_t i = lo; i < hi; i++) {
				sumX += samples[i].lba;  sumY += samples[i].ms;
				sumXY += samples[i].lba * samples[i].ms;
				sumX2 += samples[i].lba * samples[i].lba;
			}
			double denom = n * sumX2 - sumX * sumX;
			if (n >= 5 && std::abs(denom) > 1e-12) {
				double slope = (n * sumXY - sumX * sumY) / denom;
				double intercept = (sumY - slope * sumX) / n;

				// Only detrend if slope is negative (outer = faster, i.e. CAV)
				if (slope < 0.0) {
					double mean = sumY / n;
					double residSqSum = 0.0;
					for (size_t i = lo; i < hi; i++) {
						double residual = samples[i].ms
							- (intercept + slope * samples[i].lba);
						residSqSum += residual * residual;
					}
					double residStddev = std::sqrt(residSqSum / (n - 1));
					double detrendedCV = (mean > 0.001)
						? (residStddev / mean) : 0.0;
					jitterCoeffVar[s] = std::min(jitterCoeffVar[s], detrendedCV);
				}
			}
		}

		// Average per-sector worst/best ratio: 1.0 = perfectly stable,
		// >2.0 = the same sector takes 2× longer on a bad read than a good one.
		if (stabilityCount > 0) {
			avgStabilityRatio[s] = stabilitySum / stabilityCount;
			stabilityMeasured[s] = true;
		}
	}

	// A relative score is meaningless without enough successful timing reads.
	// Require at least half the requested sectors (minimum two where possible)
	// at two speeds, including one of the audio-relevant low-speed steps.
	const int requestedSamples = static_cast<int>(sampleLBAs.size());
	const int minValidSamples = std::min(requestedSamples,
		std::max(2, (requestedSamples + 1) / 2));
	int adequatelySampledSpeeds = 0;
	bool lowSpeedCoverage = false;
	for (int s = 0; s < NUM_SPEEDS; s++) {
		if (validReadSamplesPerSpeed[s] >= minValidSamples) {
			adequatelySampledSpeeds++;
			if (s <= 2) lowSpeedCoverage = true;
		}
	}
	if (adequatelySampledSpeeds < 2 || !lowSpeedCoverage) {
		m_drive.SetSpeed(0);
		m_drive.SpinDown();
		progress.Finish(false);
		std::cout << "\nERROR: Insufficient successful timing reads to score disc balance.\n";
		std::cout << "       Need at least " << minValidSamples
			<< " successful sample sectors at two speeds, including a low-speed baseline.\n";
		return false;
	}
	if (!Diagnostics::HasDistinctBalanceSpeeds(actualSpeedX, validReadSamplesPerSpeed, minValidSamples)) {
		progress.Finish(false);
		std::cout << "\nINCOMPLETE: Fewer than two distinct drive-reported speeds had sufficient readable samples.\n"
			<< "Disc balance was not scored; clamped or unverified speed requests are not independent measurements.\n";
		return false;
	}

	progress.Finish(true);

	// ── Thermal drift check ────────────────────────────────────────────
	// Re-test baseline speed to detect if disc heating shifted read times.
	double driftRatio = 1.0;
	{
		m_drive.SetSpeed(speeds[0]);
		Sleep(300);

		constexpr int DRIFT_SAMPLES = 10;
		int driftCount = std::min(DRIFT_SAMPLES, static_cast<int>(sampleLBAs.size()));
		double driftSum = 0.0;
		int driftValid = 0;

		// Use evenly-spaced samples across the full disc (matching the
		// original sweep's mix of inner/outer) for an apples-to-apples
		// comparison with avgReadTimeMs[0].
		int step = std::max(1, static_cast<int>(sampleLBAs.size()) / driftCount);
		for (int i = 0; i < driftCount; i++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) break;

			int idx = std::min(i * step,
				static_cast<int>(sampleLBAs.size()) - 1);
			DWORD lba = sampleLBAs[idx];
			DefeatDriveCache(lba, maxLBA);
			DWORD positionLBA = (lba > READ_AHEAD_MARGIN)
				? lba - READ_AHEAD_MARGIN : lba + READ_AHEAD_MARGIN;
			m_drive.ReadSectorAudioOnly(positionLBA, buf.data());

			auto t0 = std::chrono::high_resolution_clock::now();
			int c2tmp = 0;
			bool ok = hasReadCdC2
				? m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2tmp)
				: m_drive.ReadSectorAudioOnly(lba, buf.data());
			auto t1 = std::chrono::high_resolution_clock::now();
			double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

			if (ok) { driftSum += ms; driftValid++; }
		}
		if (g_interrupt.IsInterrupted()) {
			m_drive.SpinDown();
			std::cout << "\n*** Balance check cancelled during thermal-drift re-test ***\n";
			return false;
		}

		if (driftValid > 0 && avgReadTimeMs[0] > 0.001) {
			double retest = driftSum / driftValid;
			driftRatio = retest / avgReadTimeMs[0];
		}
	}

	m_drive.SetSpeed(0);

	// ── Hardware C1 sweep (Pioneer or LiteOn/MediaTek) ──────────────────
	// If the drive supports a hardware quality scan, collect per-speed C1
	// error rates from the hardware ECC decoder. Pioneer supplies diagnostic
	// E22 here; LiteOn supplies C2. Normalize counts only from interval
	// lengths explicitly returned by the backend; poll count is not duration.
	std::vector<double> hwC1PerSpeed(NUM_SPEEDS, 0.0);
	std::vector<ScanQuality::C1Statistics> hwC1Stats(NUM_SPEEDS);
	std::vector<double> hwSecondStagePerSpeed(NUM_SPEEDS, 0.0);
	std::vector<int> hwSamplesPerSpeed(NUM_SPEEDS, 0);
	bool hwSweepFailed = false;
	const char* hwSecondStageLabel = hasPioneerHwC1 ? "E22" : "C2";

	if (hasHwC1) {
		constexpr int HW_SAMPLES_PER_SPEED = 15;

		std::cout << "\nRunning hardware C1/" << hwSecondStageLabel << " sweep ("
			<< (hasPioneerHwC1 ? "Pioneer" : "LiteOn/MediaTek")
			<< " ECC decoder)...\n";

		// Start scan from the outer 25% of the disc where wobble is worst
		DWORD outerStartLBA = maxLBA * 3 / 4;

		for (int s = 0; s < NUM_SPEEDS; s++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				m_drive.SpinDown();
				std::cout << "\n*** Balance check cancelled by user ***\n";
				return false;
			}

			m_drive.SetSpeed(speeds[s]);
			Sleep(300);

			QualityScanSession eccSession([&]() {
				return hasPioneerHwC1 ? m_drive.PioneerScanStop() : m_drive.LiteOnScanStop();
			});
			bool started = hasPioneerHwC1
				? m_drive.PioneerScanStart(outerStartLBA, maxLBA)
				: m_drive.LiteOnScanStart(outerStartLBA, maxLBA);
			if (!started) {
				if (!eccSession.Stop()) {
					std::cout << "\nERROR: Hardware scan cleanup failed; drive closed. Reopen it.\n";
					m_drive.Close();
					return false;
				}
				hwSweepFailed = true;
				break;
			}

			int totalC1 = 0, totalSecondStage = 0, validSamples = 0;
			std::vector<ScanQuality::C1Interval> c1Intervals;
			bool cancelled = false;
			bool communicationLost = false;
			DWORD firstLBA = 0, lastLBA = 0;
			bool haveFirstLBA = false;
			DWORD lastReportedLBA = DWORD(-1);
			DWORD progressLBA = DWORD(-1);
			int startupSamples = 0;
			auto lastLBAProgress = std::chrono::steady_clock::now();
			constexpr auto QCHECK_STALL_TIMEOUT = std::chrono::seconds(30);
			while (validSamples < HW_SAMPLES_PER_SPEED) {
				if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
					cancelled = true;
					break;
				}

				int c1 = 0, secondStage = 0, cu = 0;
				DWORD lba = 0;
				DWORD measuredSectors = 0;
				bool done = false;
				bool sampleValid = true;

				bool pollOk = hasPioneerHwC1
					? m_drive.PioneerScanPoll(c1, secondStage, cu, lba, done,
						&sampleValid, &measuredSectors)
					: m_drive.LiteOnScanPoll(c1, secondStage, cu, lba, done, &measuredSectors, &sampleValid);
				if (!pollOk && hasPioneerHwC1) {
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
					pollOk = m_drive.PioneerScanPoll(
						c1, secondStage, cu, lba, done, &sampleValid, &measuredSectors);
				}
				if (!pollOk) {
					communicationLost = true;
					break;
				}



				const auto pollTime = std::chrono::steady_clock::now();
				if (done || progressLBA == DWORD(-1) || lba > progressLBA) {
					progressLBA = lba;
					lastLBAProgress = pollTime;
				}
				else if (pollTime - lastLBAProgress >= QCHECK_STALL_TIMEOUT) {
					communicationLost = true;
					break;
				}

				if (!sampleValid) {
					if (done) break;
					continue;
				}
				if (!hasPioneerHwC1 && measuredSectors == 0 && lba == 0 && c1 == 0 &&
					secondStage == 0 && cu == 0 && !done) {
					continue;
				}
				if (!haveFirstLBA) {
					firstLBA = lba;
					haveFirstLBA = true;
				}
				if (lba == lastReportedLBA) {
					if (done) break;
					continue;
				}
				lastReportedLBA = lba;

				// Skip first 3 samples — drive reports accumulated startup errors
				if (measuredSectors == 0 && startupSamples < 3 && !done) {
					startupSamples++;
					continue;
				}

				c1Intervals.push_back({lba, measuredSectors, c1});
				totalC1 += c1;
				totalSecondStage += secondStage;
				validSamples++;
				lastLBA = lba;
				if (done) break;
			}

			// Always called — even on cancel.
			if (!eccSession.Stop()) {
				std::cout << "\nERROR: Could not stop hardware scan; remaining balance checks cancelled. Reopen the drive.\n";
				m_drive.Close();
				return false;
			}

			// Log actual scan position for diagnostics
			char dbg[128];
			snprintf(dbg, sizeof(dbg), "HW ECC %dx: LBA %lu-%lu, %d samples, C1=%d %s=%d\n",
				speeds[s], (unsigned long)firstLBA, (unsigned long)lastLBA,
				validSamples, totalC1, hwSecondStageLabel, totalSecondStage);
			OutputDebugStringA(dbg);

			// A transport failure, stall, or early terminal response invalidates the
			// partial speed bucket. Disc Balance can still produce a timing-based
			// result, but must not score incomplete ECC data as a full comparison.
			if (communicationLost || validSamples < HW_SAMPLES_PER_SPEED) {
				hwSweepFailed = true;
				std::cout << "  Hardware ECC sweep at " << speeds[s] << "x "
					<< (communicationLost ? "lost communication, returned invalid positions, or stalled" : "ended early")
					<< " after " << validSamples << "/" << HW_SAMPLES_PER_SPEED
					<< " usable samples; partial bucket discarded.\n";
				validSamples = 0;
				totalC1 = 0;
				totalSecondStage = 0;
			}

			hwC1Stats[s] = ScanQuality::SummarizeCompletedC1(c1Intervals,
				HW_SAMPLES_PER_SPEED, !communicationLost && !cancelled && validSamples >= HW_SAMPLES_PER_SPEED);
			hwC1PerSpeed[s] = hwC1Stats[s].average;
			hwSecondStagePerSpeed[s] = hwC1Stats[s].RateAvailable()
				? totalSecondStage / hwC1Stats[s].MeasuredSeconds() : 0.0;
			hwSamplesPerSpeed[s] = hwC1Stats[s].RateAvailable() ? validSamples : 0;
			if (!hwC1Stats[s].RateAvailable()) {
				hwSweepFailed = true;
				std::cout << "  Hardware rate comparison unavailable at this speed: "
					"counter validity or measured duration is unverified.\n";
			}

			if (cancelled) {
				m_drive.SetSpeed(0);
				m_drive.SpinDown();
				std::cout << "\n*** Balance check cancelled by user ***\n";
				return false;
			}
			if (hwSweepFailed) break;
		}

	}

	// Pioneer E22 is diagnostic-only. Add the utility's quick radial CD Check so
	// Disc Balance reports genuine uncorrectable bytes with the same lifecycle
	// and unmeasured-vs-clean rules as Q-Check, BLER, C2, and Disc Rot. This is a
	// data-loss cross-check only; it does not alter the mechanical balance score.
	if (isPioneerDrive) {
		balanceCdCheckAttempted = true;
		RunPioneerCdCheckMeasurement(disc, PioneerCdCheckScanMode::Quick,
			balanceCdCheck, "  CD Check");
		if (balanceCdCheck.cancelled ||
			InterruptHandler::Instance().IsInterrupted()) {
			m_drive.SetSpeed(0);
			m_drive.SpinDown();
			std::cout << "\n*** Balance check cancelled during CD Check cross-check ***\n";
			return false;
		}
	}

	m_drive.SetSpeed(0);
	m_drive.SpinDown();

	// ── Scoring: combine error signal, jitter, and scaling ──────────────

	std::vector<Diagnostics::BalanceSpeedSample> measurements;
	for (int s=0; s<NUM_SPEEDS; ++s) {
		measurements.push_back({speeds[s], actualSpeedX[s], validReadSamplesPerSpeed[s],
			avgReadTimeMs[s], jitterCoeffVar[s], avgStabilityRatio[s], stabilityMeasured[s],
			avgReadErrorSignalPerSpeed[s], hwC1PerSpeed[s], hwSecondStagePerSpeed[s], hwSamplesPerSpeed[s]});
	}
	const auto assessment = Diagnostics::AssessBalance(measurements, requestedSamples,
		minValidSamples, hasHwC1, hasPioneerHwC1);
	if (!assessment.available) {
		std::cout << "INCOMPLETE: At least two distinct verified speeds with usable timing measurements are required. Disc balance was not scored.\n";
		return false;
	}
	balanceScore = assessment.score;
	const int balanceScoreFull = assessment.fullScore;
	const int errorScore = assessment.errorScore, errorScoreFull = assessment.fullErrorScore;
	const int jitterScore = assessment.jitterScore, stabilityScore = assessment.stabilityScore;
	const int scalingScore = assessment.scalingScore, safeSpeed = assessment.suggestedSpeed;
	const bool stabilityAvailable = assessment.stabilityAvailable, usingHwEcc = assessment.usingHwEcc;
	const auto& speedFellBack = assessment.speedFellBack;
	const auto& eccFellBack = assessment.eccFellBack;

	// ── Report ──────────────────────────────────────────────────────────

	// Per-speed clamp annotation, from the actual speeds captured during the
	// sweep (MODE SENSE readback). A row is clamped when the drive ran
	// meaningfully faster than requested — e.g. request 4x/8x, drive runs 10x.
	// Purely cosmetic: explains duplicate-looking low rows, no effect on score.
	std::vector<int> clampedActualX(NUM_SPEEDS, 0);   // 0 = not clamped
	for (int s = 0; s < NUM_SPEEDS; s++) {
		// Treat as clamped only if the actual multiplier is at least 2x above
		// the request (avoids flagging normal rounding like 4x->5x).
		if (actualSpeedX[s] > 0 && actualSpeedX[s] >= speeds[s] + 2)
			clampedActualX[s] = actualSpeedX[s];
	}

	// Earlier output (e.g. MSF time formatting during the disc/TOC scan) can
	// leave std::cout's fill character set to '0'. std::cout is global, so that
	// state persists into this report and would make setw() padding render as
	// zeros instead of spaces. Reset it once for the whole report.
	const int reportWidth = 60;
	const char* reportTitle = "DISC BALANCE CHECK";
	std::cout << std::setfill(' ') << std::right;
	std::cout << "\n" << std::string(reportWidth, '=') << "\n";
	std::cout << std::string((reportWidth - static_cast<int>(std::strlen(reportTitle))) / 2, ' ')
		<< reportTitle << "\n";
	std::cout << std::string(reportWidth, '=') << "\n";
	std::cout << "  (Detects vibration / wobble by sweeping read speed)\n\n";
	std::cout << "  Comparison includes verified actual speeds through " << assessment.maximumMeasuredSpeed << "x.\n";
	if (assessment.partial) std::cout << "  PARTIAL SPEED COVERAGE - unverified or insufficient timing rows are excluded.\n";
	std::cout << "  Scores and recommendations apply only to the measured speeds.\n\n";
	std::cout << "  This is a mechanical/read-stability assessment, not a C2/CU\n"
		<< "  data-loss test or proof that an extraction is bit-perfect.\n\n";
	if (balanceCdCheckAttempted) {
		std::cout << "--- Pioneer CD Check Data-Loss Cross-Check ---\n";
		if (balanceCdCheck.reliable) {
			std::cout << "  Coverage: Quick radial sampling (0.05 mm), "
				<< balanceCdCheck.validSamples << " / "
				<< balanceCdCheck.plannedSamples << " samples\n";
			std::cout << "  Worst C1 uncorrectable: "
				<< balanceCdCheck.worstC1Frames << " frames\n";
			std::cout << "  Worst C2 uncorrectable: "
				<< balanceCdCheck.worstC2Bytes << " bytes";
			if (balanceCdCheck.worstC2Bytes == 0)
				std::cout << "  (none in sampled windows)\n";
			else
				std::cout << "  ** DATA LOSS DETECTED **\n";
			std::cout << "  This sampled result is reported separately and does not change\n"
				<< "  the mechanical Balance Score or Suggested Max Rip Speed.\n\n";
		}
		else {
			std::cout << "  Uncorrectable status: UNMEASURED ("
				<< (balanceCdCheck.failureReason.empty()
					? "no complete valid measurement" : balanceCdCheck.failureReason)
				<< ")\n"
				<< "  A missing measurement is not reported as a clean zero.\n\n";
		}
	}
	auto PrintReadSignalReport = [&]() {
		std::cout << "--- READ CD / Read-Stability Signal by Speed ---\n";
		for (int s = 0; s < NUM_SPEEDS; s++) {
			std::cout << "  " << std::setw(3) << speeds[s] << "x:  ";
			if (hasReadCdC2) {
				std::cout << "READ CD C2 " << std::fixed << std::setprecision(2)
					<< avgReadCdC2PerSpeed[s] << "/sector   ";
			}
			else {
				std::cout << "READ CD C2 N/A   ";
			}
			std::cout << "balance signal " << std::fixed << std::setprecision(2)
				<< avgReadErrorSignalPerSpeed[s];
			if (!assessment.compared[s]) std::cout << "  (excluded from speed comparison)";
			std::cout << "\n";
		}

		// Warn when C2 reports zero but timing-based metrics found problems.
		// This combination suggests that the drive accepts C2 commands without
		// actually populating the error-pointer data.
		bool allC2Zero = true;
		for (int s = 0; s < NUM_SPEEDS; s++) {
			if (avgReadCdC2PerSpeed[s] > 0.0) { allC2Zero = false; break; }
		}
		if (hasReadCdC2 && allC2Zero && scalingScore < 100) {
			std::cout << "  ** NOTE: C2 reports 0 errors at all speeds, but timing\n"
				<< "     detected wobble. C2 data may not be functional on this\n"
				<< "     drive. Rely on Scaling/Jitter scores instead. **\n";
		}
	};

	if (hasHwC1) {
		std::cout << "--- Hardware Error/Diagnostic Rates by Speed (ECC decoder) ---\n";
		for (int s = 0; s < NUM_SPEEDS; s++) {
			std::cout << "\n  " << speeds[s] << "x hardware observations:\n";
			if (!assessment.compared[s]) std::cout << "    Excluded from speed comparison: speed or timing coverage unverified.\n";
			ScanQuality::PrintC1Summary(std::cout, hwC1Stats[s], 0, "    ");
			std::cout << "    " << hwSecondStageLabel << ": ";
			if (hwC1Stats[s].RateAvailable()) std::cout << hwSecondStagePerSpeed[s] << "/sec";
			else std::cout << "rate unavailable";
			if (hwSamplesPerSpeed[s] == 0)
				std::cout << "  (no usable timed rate)";
			if (speedFellBack[s] || eccFellBack[s])
				std::cout << "  ** FALLBACK (drive can't sustain this speed) **";
			else if (clampedActualX[s] > 0)
				std::cout << "  (ran at ~" << clampedActualX[s] << "x - drive floor)";
			else if (assessment.compared[s] && !assessment.primaryCompared[s])
				std::cout << "  (full-speed score only - above audio-relevant range)";
			std::cout << "\n";
		}
		if (hasPioneerHwC1) {
			std::cout << "  NOTE: Pioneer E22 is a raw diagnostic counter, not verified C2/E32\n"
				<< "        or CU. It is reported here but does not affect the Balance Score\n"
				<< "        or Suggested Max Rip Speed and is not a copy-integrity trigger.\n";
		}
		ScanQuality::PrintC1Policy(std::cout);
		ScanQuality::PrintWrapped(std::cout,
			"C1 bands describe only the sampled region at each speed. Balance "
			"scores compare speed-dependent trends, timing and read stability; "
			"they are not whole-disc C1 ratings or proof of physical imbalance.", "  ");
		if (hwSweepFailed) {
			std::cout << "  ** NOTE: Some hardware speed buckets were incomplete. Only completed\n"
				<< "     buckets at qualified measured speeds can contribute to scoring. **\n";
		}
		else if (!usingHwEcc) {
			std::cout << "  ** NOTE: Scored hardware ECC rates are flat, incomplete, or unavailable\n"
				<< "     across the requested speed settings.\n"
				<< "     The hardware scan firmware likely ignores SetSpeed - per-speed\n"
				<< "     comparison is not meaningful. Relying on READ CD/read-stability\n"
				<< "     plus Jitter/Scaling instead of hardware ECC for wobble. **\n";
		}
		if (!usingHwEcc) {
			std::cout << "\n";
			PrintReadSignalReport();
		}
		// Note which speeds contributed to scoring
		bool anyAboveCeiling = false;
		for (int s = 0; s < NUM_SPEEDS; s++) {
			if (!assessment.compared[s] || assessment.primaryCompared[s]) continue;
			if (hwC1PerSpeed[s] > 0.0
				|| (!hasPioneerHwC1 && hwSecondStagePerSpeed[s] > 0.0))
				anyAboveCeiling = true;
		}
		if (anyAboveCeiling) {
			std::cout << "  Note: Hardware counters above " << assessment.primaryMaximumSpeed
				<< "x do not affect the primary measured-range score.\n"
				<< "        Qualified counters at higher measured speeds are included in\n"
				<< "        the full-speed balance score below.\n";
		}
	}
	else {
		PrintReadSignalReport();
	}

	std::cout << "\n--- Read Time Jitter by Speed ---\n";
	for (int s = 0; s < NUM_SPEEDS; s++) {
		std::cout << "  " << std::setw(3) << speeds[s] << "x:  CV "
			<< std::fixed << std::setprecision(3) << jitterCoeffVar[s]
			<< "  (avg " << std::setprecision(1) << avgReadTimeMs[s] << " ms)"
			<< "  stability ";
		if (stabilityMeasured[s])
			std::cout << std::setprecision(2) << avgStabilityRatio[s] << "x";
		else
			std::cout << "N/A";
		std::cout << "  reads " << validReadSamplesPerSpeed[s] << "/" << requestedSamples;
		if (!assessment.compared[s]) std::cout << "  (excluded: speed or timing coverage unverified)";
		// Annotate clamped low rows so duplicate-looking times are explained.
		if (clampedActualX[s] > 0)
			std::cout << "  (ran at ~" << clampedActualX[s] << "x)";
		// Use the scorer's qualified comparisons, including repeated actual speeds.
		if (assessment.timingPenalty[s] == 2) std::cout << "  ** REGRESSION **";
		else if (assessment.timingPenalty[s] == 1) std::cout << "  * plateau *";
		std::cout << "\n";
	}

	std::cout << "\n  Error Sub-Score:     " << errorScore << " / 100";
	if (usingHwEcc && hasPioneerHwC1)
		std::cout << "  (hardware C1; Pioneer E22 diagnostic-only)";
	else if (usingHwEcc)
		std::cout << "  (hardware C1/C2)";
	else if (hasReadCdC2 && hasHwC1 && !usingHwEcc)
		std::cout << "  (READ CD/read-stability signal; hardware ECC ignored)";
	else if (hasReadCdC2)
		std::cout << "  (READ CD/read-stability signal)";
	else
		std::cout << "  (timing/read-failure signal; C2 unavailable)";
	std::cout << "\n";
	if (assessment.haveFullScore) {
		std::cout << "  Error Sub-Score*:    " << errorScoreFull
			<< " / 100  (* wider verified speed range)\n";
	}
	std::cout << "  Jitter Sub-Score:    " << jitterScore << " / 100\n";
	if (stabilityAvailable)
		std::cout << "  Stability Sub-Score: " << stabilityScore << " / 100  (per-sector read consistency)\n";
	else
		std::cout << "  Stability Sub-Score: N/A (insufficient repeated reads)\n";
	std::cout << "  Scaling Sub-Score:   " << scalingScore << " / 100\n";

	if (std::isfinite(driftRatio) && (driftRatio > 1.3 || driftRatio < 0.7)) {
		std::cout << "\n  ** WARNING: Baseline re-test shows "
			<< std::fixed << std::setprecision(0) << (std::abs(driftRatio - 1.0) * 100)
			<< "% thermal drift. Scores may be affected by disc heating. **\n";
	}

	std::cout << "\n  Balance Score: " << balanceScore << " / 100";
	if (balanceScore >= 75)      std::cout << "  (GOOD - within the verified speed range)\n";
	else if (balanceScore >= 50) std::cout << "  (FAIR - some wobble detected, reduce rip speed)\n";
	else                         std::cout << "  (POOR - significant balance problem, use 4x-8x max)\n";

	// Full-speed (mechanical / full-RPM) score. Absent only if no speed above
	// the audio-relevant ceiling was swept (with the current fixed speed table
	// it is always present). When absent, the high-speed rows below report
	// "Not tested" rather than guessing from the audio score.
	bool haveFullScore = assessment.haveFullScore;
	int fullScore = balanceScoreFull;
	if (haveFullScore) {
		std::cout << "  Balance Score (wider verified range): " << balanceScoreFull << " / 100";
		if (balanceScoreFull >= 75)      std::cout << "  (GOOD)\n";
		else if (balanceScoreFull >= 50) std::cout << "  (FAIR)\n";
		else                             std::cout << "  (POOR)\n";
	}

	// Plain-language interpretation: what the two scores mean for each
	// real-world use of the disc, gentlest demand first. Playback and audio
	// ripping (<=16x) follow the audio score; fast extraction and the
	// full-RPM mechanical view follow the full-speed score.
	auto InterpRow = [](const char* label, const char* verdict) {
		std::cout << "  " << std::left << std::setw(24) << label
			<< std::right << verdict << "\n";
	};

	std::cout << "\n";
	InterpRow("Regular playback:",
		balanceScore >= 75 ? "Very likely fine"
		: balanceScore >= 50 ? "Likely fine"
		: "Possible glitches");
	InterpRow("Audio ripping <=16x:",
		balanceScore >= 75 ? "Good"
		: balanceScore >= 50 ? "Caution - reduce speed"
		: "Poor - use 4x-8x");
	InterpRow("Higher verified speeds:",
		!haveFullScore ? "Not tested"
		: fullScore >= 75 ? "Fine"
		: fullScore >= 50 ? "Caution"
		: "Avoid");
	InterpRow("Wider verified range:",
		!haveFullScore ? "Not tested"
		: fullScore >= 75 ? "Good"
		: fullScore >= 50 ? "Fair"
		: "Poor / errors climb at high RPM");

	std::cout << "\n  Suggested Max Rip Speed: " << safeSpeed << "x\n";

	if (balanceScore < 75) {
		std::cout << "\n  Recommendation:\n";
		if (balanceScore < 50) {
			std::cout << "    - For extraction, start at " << safeSpeed
				<< "x or lower with Secure or Paranoid mode.\n";
			std::cout << "    - Re-check previous faster rips if they lack independent\n"
				<< "      checksum verification.\n";
			std::cout << "    - Verify against AccurateRip; the balance test itself cannot\n"
				<< "      determine whether a rip is bit-perfect.\n";
		}
		else {
			std::cout << "    - Prefer " << safeSpeed
				<< "x or lower with Secure mode.\n";
			std::cout << "    - Verify faster rips with AccurateRip or another independent\n"
				<< "      checksum source; Disc Balance is not a copy-integrity test.\n";
		}
	}

	std::cout << std::string(60, '=') << "\n";
	return true;
}
