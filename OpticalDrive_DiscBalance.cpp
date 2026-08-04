#define NOMINMAX
#include "OpticalDrive.h"
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

// Continuous score from a ratio using log-linear interpolation.
// Breakpoints are {ratio, score} pairs in ascending ratio order.
// Interpolates linearly in log-space between adjacent breakpoints.
static int ContinuousScore(double ratio,
	std::initializer_list<std::pair<double, int>> bpList) {
	std::vector<std::pair<double, int>> bp(bpList);
	if (ratio <= bp.front().first) return bp.front().second;
	if (ratio >= bp.back().first) return bp.back().second;

	double logR = std::log(std::max(ratio, 1e-9));
	for (size_t i = 1; i < bp.size(); i++) {
		if (ratio <= bp[i].first) {
			double logLo = std::log(std::max(bp[i - 1].first, 1e-9));
			double logHi = std::log(std::max(bp[i].first, 1e-9));
			double t = (logHi > logLo)
				? (logR - logLo) / (logHi - logLo) : 0.0;
			double score = bp[i - 1].second
				+ t * (bp[i].second - bp[i - 1].second);
			return std::clamp(static_cast<int>(score + 0.5), 0, 100);
		}
	}
	return bp.back().second;
}

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
		double t = SAMPLE_COUNT > 1
			? static_cast<double>(i) / static_cast<double>(SAMPLE_COUNT - 1)
			: 0.0;
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
	// E22 here; LiteOn supplies C2. Each poll returns one
	// 75-sector time slice, so N polls ~ N seconds of measurement.
	std::vector<double> hwC1PerSpeed(NUM_SPEEDS, 0.0);
	std::vector<double> hwSecondStagePerSpeed(NUM_SPEEDS, 0.0);
	std::vector<int> hwSamplesPerSpeed(NUM_SPEEDS, 0);
	bool hwEccFlat = false;  // True if ECC data has no per-speed discriminating power
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

			bool started = hasPioneerHwC1
				? m_drive.PioneerScanStart(outerStartLBA, maxLBA)
				: m_drive.LiteOnScanStart(outerStartLBA, maxLBA);
			if (!started) {
				hwSweepFailed = true;
				break;
			}

			int totalC1 = 0, totalSecondStage = 0, validSamples = 0;
			bool cancelled = false;
			bool communicationLost = false;
			DWORD firstLBA = 0, lastLBA = 0;
			bool haveFirstLBA = false;
			DWORD lastReportedLBA = DWORD(-1);
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
				bool done = false;
				bool pioneerSampleValid = true;

				bool pollOk = hasPioneerHwC1
					? m_drive.PioneerScanPoll(c1, secondStage, cu, lba, done,
						&pioneerSampleValid)
					: m_drive.LiteOnScanPoll(c1, secondStage, cu, lba, done);
				if (!pollOk && hasPioneerHwC1) {
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
					pollOk = m_drive.PioneerScanPoll(
						c1, secondStage, cu, lba, done, &pioneerSampleValid);
				}
				if (!pollOk) {
					communicationLost = true;
					break;
				}

				if (!hasPioneerHwC1 && lba >= maxLBA)
					done = true;
				if (hasPioneerHwC1 && !pioneerSampleValid)
					continue;

				const auto pollTime = std::chrono::steady_clock::now();
				if (done || lba != lastReportedLBA)
					lastLBAProgress = pollTime;
				else if (pollTime - lastLBAProgress >= QCHECK_STALL_TIMEOUT) {
					communicationLost = true;
					break;
				}

				if (!hasPioneerHwC1 && lba == 0 && c1 == 0 &&
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
				if (startupSamples < 3 && !done) {
					startupSamples++;
					continue;
				}

				totalC1 += c1;
				totalSecondStage += secondStage;
				validSamples++;
				lastLBA = lba;
				if (done) break;
			}

			// Always called — even on cancel.
			if (hasPioneerHwC1) m_drive.PioneerScanStop();
			else m_drive.LiteOnScanStop();

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
					<< (communicationLost ? "lost communication or stalled" : "ended early")
					<< " after " << validSamples << "/" << HW_SAMPLES_PER_SPEED
					<< " usable samples; partial bucket discarded.\n";
				validSamples = 0;
				totalC1 = 0;
				totalSecondStage = 0;
			}

			hwC1PerSpeed[s] = validSamples > 0
				? static_cast<double>(totalC1) / validSamples : 0.0;
			hwSecondStagePerSpeed[s] = validSamples > 0
				? static_cast<double>(totalSecondStage) / validSamples : 0.0;
			hwSamplesPerSpeed[s] = validSamples;

			if (cancelled) {
				m_drive.SetSpeed(0);
				m_drive.SpinDown();
				std::cout << "\n*** Balance check cancelled by user ***\n";
				return false;
			}
			if (hwSweepFailed) break;
		}

		// Detect flat ECC data: if the scan firmware ignores SetSpeed, all
		// speed steps return the same scored values and the per-speed comparison
		// is meaningless. Pioneer E22 is deliberately excluded here because it
		// is a raw diagnostic, not a C2-equivalent balance or copy trigger.
		double maxErrors = 0.0, minErrors = 1e9;
		int hwValidSpeeds = 0;
		for (int s = 0; s < NUM_SPEEDS; s++) {
			if (hwSamplesPerSpeed[s] == 0) continue;
			hwValidSpeeds++;
			double errors = hwC1PerSpeed[s]
				+ (hasPioneerHwC1 ? 0.0 : hwSecondStagePerSpeed[s]);
			if (errors > maxErrors) maxErrors = errors;
			if (errors < minErrors) minErrors = errors;
		}
		if (hwSweepFailed || hwValidSpeeds < 2 || maxErrors < 0.5
			|| (maxErrors - minErrors) < 0.5) {
			hwEccFlat = true;
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

	// Baseline detection: skip clamped speeds at the bottom
	int baselineIdx = 0;
	while (baselineIdx < NUM_SPEEDS
		&& validReadSamplesPerSpeed[baselineIdx] < minValidSamples)
		baselineIdx++;
	if (baselineIdx >= NUM_SPEEDS) {
		std::cout << "ERROR: No adequately sampled timing baseline is available.\n";
		return false;
	}
	for (int s = baselineIdx + 1; s < NUM_SPEEDS; s++) {
		if (validReadSamplesPerSpeed[s] < minValidSamples)
			break;
		if (avgReadTimeMs[s] > 0.001 && avgReadTimeMs[baselineIdx] > 0.001) {
			double ratio = avgReadTimeMs[baselineIdx] / avgReadTimeMs[s];
			if (ratio < 1.15) {
				baselineIdx = s;
			}
			else {
				break;
			}
		}
	}
	if (hasHwC1 && hwSamplesPerSpeed[baselineIdx] == 0)
		hwEccFlat = true;

	// Audio-relevant speed ceiling for error scoring.
	// Professional CD players run at 1x; quality rippers at 4x-8x.
	// ECC errors that only appear at 24x+ are likely drive/mechanical
	// artifacts, not disc balance problems.  Cap error scoring at 16x
	// (index 2) so high-speed errors are reported but don't tank the score.
	constexpr int MAX_AUDIO_RELEVANT_SPEED = 16;
	int errorCeilingIdx = NUM_SPEEDS - 1;
	for (int s = 0; s < NUM_SPEEDS; s++) {
		if (speeds[s] >= MAX_AUDIO_RELEVANT_SPEED) {
			errorCeilingIdx = s;
			break;
		}
	}
	errorCeilingIdx = std::max(errorCeilingIdx,
		std::min(baselineIdx + 1, NUM_SPEEDS - 1));

	// Detect speed fallback: if a higher speed step has read times that
	// match or exceed a much lower speed, the drive silently fell back.
	// Mark those steps so they don't confuse error or scaling analysis.
	std::vector<bool> speedFellBack(NUM_SPEEDS, false);
	for (int s = 2; s < NUM_SPEEDS; s++) {
		if (avgReadTimeMs[s] > 0.001 && avgReadTimeMs[s - 2] > 0.001) {
			// If read time at speed[s] is >= speed[s-2], it fell back
			if (avgReadTimeMs[s] >= avgReadTimeMs[s - 2] * 0.95 &&
				avgReadTimeMs[s - 1] < avgReadTimeMs[s - 2] * 0.85) {
				speedFellBack[s] = true;
			}
		}
	}

	// ECC-specific fallback: if HW errors spike at speed[s-1] but drop to
	// near-zero at speed[s], the drive likely couldn't sustain that speed
	// during the ECC scan and silently fell back to a lower speed.
	// This complements timing-based detection — the two sweeps use
	// different disc regions and the drive may behave differently.
	std::vector<bool> eccFellBack(NUM_SPEEDS, false);
	if (hasHwC1 && !hwEccFlat) {
		for (int s = 1; s < NUM_SPEEDS; s++) {
			if (hwSamplesPerSpeed[s - 1] == 0 || hwSamplesPerSpeed[s] == 0)
				continue;
			double prevErrors = hwC1PerSpeed[s - 1]
				+ (hasPioneerHwC1 ? 0.0 : hwSecondStagePerSpeed[s - 1]);
			double curErrors = hwC1PerSpeed[s]
				+ (hasPioneerHwC1 ? 0.0 : hwSecondStagePerSpeed[s]);
			// Previous speed had significant errors but this speed dropped
			// to near-zero — strong sign the drive fell back for the scan
			if (prevErrors > 20.0 && curErrors < 1.0) {
				eccFellBack[s] = true;
			}
		}
	}

	// Error score: compare low-speed baseline to high-speed errors.
	// Prefer hardware C1 data (ECC decoder) over READ CD C2 bitmap —
	// the bitmap is non-functional on many MediaTek-based drives.
	// BUT if the ECC data is flat (firmware ignores SetSpeed), fall through
	// to timing-based scoring — the per-speed comparison is meaningless.
	bool usingHwEcc = (hasHwC1 && !hwEccFlat);

	// Error sub-score, factored out so it can be evaluated over two speed
	// ranges: the audio-relevant ceiling (<=16x) for the primary score, and
	// the full sweep (incl. 24/32/40x) for the secondary "full-speed" score.
	// 'ceilIdx' is the highest speed index that contributes to the score.
	auto ComputeErrorScore = [&](int ceilIdx) -> int {
		if (ceilIdx <= baselineIdx) return 100;
		int errorScore = 0;

		if (hasHwC1 && !hwEccFlat) {
			bool scoredSecondStage = false;

			// LiteOn's second-stage counter is a C2 measurement and can contribute
			// to this advisory physical-balance score. Pioneer reports raw E22 in
			// the same response slot; Q-Check keeps E22 diagnostic-only, so Disc
			// Balance must not run it through C2 thresholds or let it change the
			// score, suggested speed, or copy guidance.
			if (!hasPioneerHwC1) {
				constexpr double HW_C2_NOISE_FLOOR = 5.0; // C2/sec below this = ignore
				double hwC2Baseline = std::max(hwSecondStagePerSpeed[baselineIdx], 1.0);
				double peakHwC2Ratio = 0.0;
				double peakHwC2Abs = 0.0;
				for (int s = baselineIdx + 1; s <= ceilIdx; s++) {
					if (hwSamplesPerSpeed[s] == 0) continue;
					if (speedFellBack[s]) continue;
					peakHwC2Abs = std::max(peakHwC2Abs, hwSecondStagePerSpeed[s]);
					double ratio = hwSecondStagePerSpeed[s] / hwC2Baseline;
					peakHwC2Ratio = std::max(peakHwC2Ratio, ratio);
				}

				if (peakHwC2Abs >= HW_C2_NOISE_FLOOR && peakHwC2Ratio > 1.0) {
					errorScore = ContinuousScore(peakHwC2Ratio, {
						{1.0, 100}, {4.0, 90}, {10.0, 70}, {25.0, 45}, {60.0, 15}, {120.0, 0}
						});
					scoredSecondStage = true;
				}
			}

			if (!scoredSecondStage) {
				// No scored second-stage signal — use C1 ratio analysis. C1 is
				// corrected in-drive and rises with speed on every disc, so these
				// breakpoints are deliberately tolerant: a 2-4x climb over baseline is
				// normal and stays at/near 100; only a large climb (8x+) is penalized.
				double hwBaseline = std::max(hwC1PerSpeed[baselineIdx], 1.0);
				double peakHwRatio = 0.0;
				for (int s = baselineIdx + 1; s <= ceilIdx; s++) {
					if (hwSamplesPerSpeed[s] == 0) continue;
					if (speedFellBack[s]) continue;
					double ratio = hwC1PerSpeed[s] / hwBaseline;
					if (ratio > peakHwRatio) peakHwRatio = ratio;
				}

				errorScore = ContinuousScore(peakHwRatio, {
					{1.0, 100}, {4.0, 100}, {8.0, 80}, {16.0, 55}, {32.0, 30}, {64.0, 0}
					});

				// C1 trend detection (only within the scored range). Also widened:
				// the first-half vs second-half climb has to be substantial before it
				// caps the score, since some climb is expected.
				int numActive = (ceilIdx + 1) - baselineIdx;
				if (numActive >= 4) {
					int half = numActive / 2;
					double lowSum = 0.0, highSum = 0.0;
					int lowN = 0, highN = 0;
					for (int i = 0; i < half; i++) {
						int idx = baselineIdx + i;
						if (hwSamplesPerSpeed[idx] == 0) continue;
						lowSum += hwC1PerSpeed[idx];
						lowN++;
					}
					for (int i = half; i < numActive; i++) {
						int idx = baselineIdx + i;
						if (hwSamplesPerSpeed[idx] == 0) continue;
						highSum += hwC1PerSpeed[idx];
						highN++;
					}

					double lowAvg = lowN > 0 ? lowSum / lowN : 0.0;
					double highAvg = highN > 0 ? highSum / highN : 0.0;
					double trendRatio = (lowAvg > 0.1 && highN > 0) ? highAvg / lowAvg : 0.0;

					if (lowN > 0 && highN > 0) {
						int trendCap = ContinuousScore(trendRatio, {
							{1.0, 100}, {3.0, 90}, {6.0, 65}, {12.0, 35}, {24.0, 0}
							});
						errorScore = std::min(errorScore, trendCap);
					}
				}
			}
		}
		else {
			// READ CD/timing path (no usable hardware ECC speed comparison).
			// This composite contains C2 pointer counts when supported plus explicit
			// unstable/failed-read penalties; it is a balance signal, not a claim of
			// measured C2 or copy corruption.
			double baseline = std::max(avgReadErrorSignalPerSpeed[baselineIdx], 1.0);
			double peakReadErrorRatio = 0.0;
			for (int s = baselineIdx + 1; s <= ceilIdx; s++) {
				if (speedFellBack[s]) continue;
				double ratio = avgReadErrorSignalPerSpeed[s] / baseline;
				if (ratio > peakReadErrorRatio) peakReadErrorRatio = ratio;
			}

			errorScore = ContinuousScore(peakReadErrorRatio, {
				{1.0, 100}, {3.0, 100}, {6.0, 80}, {15.0, 55}, {40.0, 25}, {80.0, 0}
				});
		}
		// Relative ratios can hide absolute failures when every speed fails at a
		// similar rate. Cap the error score by successful-read coverage.
		double worstFailureRate = 0.0;
		for (int s = baselineIdx; s <= ceilIdx; s++) {
			if (speedFellBack[s]) continue;
			double successRate = requestedSamples > 0
				? static_cast<double>(validReadSamplesPerSpeed[s]) / requestedSamples : 0.0;
			worstFailureRate = std::max(worstFailureRate, 1.0 - successRate);
		}
		int coverageCap = ContinuousScore(worstFailureRate, {
			{0.0, 100}, {0.02, 90}, {0.10, 60}, {0.25, 30}, {0.50, 0}
			});
		return std::min(errorScore, coverageCap);
	};

	// Primary error sub-score: audio-relevant speeds only (<=16x).
	int errorScore = ComputeErrorScore(errorCeilingIdx);
	// Secondary error sub-score: full sweep, including 24/32/40x. Clamped to the
	// primary score so the wider range can only reveal MORE degradation, never
	// less. (The scorer can switch between its second-stage and C1 fallback paths
	// as the range widens; without this clamp that path switch could rarely make
	// the full-speed score read higher than the audio one, contradicting it.)
	int errorScoreFull = std::min(ComputeErrorScore(NUM_SPEEDS - 1), errorScore);

	// Jitter score
	double baselineCV = std::max(jitterCoeffVar[baselineIdx], 0.01);
	double peakJitterRatio = 0.0;
	for (int s = baselineIdx + 1; s < NUM_SPEEDS; s++) {
		double ratio = jitterCoeffVar[s] / baselineCV;
		if (ratio > peakJitterRatio) peakJitterRatio = ratio;
	}

	int jitterScore = ContinuousScore(peakJitterRatio, {
		{1.0, 100}, {2.0, 100}, {4.0, 75}, {8.0, 50}, {16.0, 25}, {32.0, 0}
		});

	// Read stability score: per-sector worst/best ratio increase with speed.
	// This directly measures wobble — a balanced disc reads the same sector
	// in the same time regardless of attempt, while wobble causes the servo
	// to hunt, producing large read-time spread for individual sectors.
	int stabilityBaselineIdx = baselineIdx;
	while (stabilityBaselineIdx < NUM_SPEEDS && !stabilityMeasured[stabilityBaselineIdx])
		stabilityBaselineIdx++;
	const bool stabilityAvailable = stabilityBaselineIdx < NUM_SPEEDS;
	double baselineStability = stabilityAvailable
		? std::max(avgStabilityRatio[stabilityBaselineIdx], 1.001) : 1.001;
	double peakStabilityRatio = 0.0;
	for (int s = stabilityBaselineIdx + 1; stabilityAvailable && s < NUM_SPEEDS; s++) {
		if (!stabilityMeasured[s]) continue;
		double ratio = avgStabilityRatio[s] / baselineStability;
		if (ratio > peakStabilityRatio) peakStabilityRatio = ratio;
	}

	int stabilityScore = stabilityAvailable ? ContinuousScore(peakStabilityRatio, {
		{1.0, 100}, {1.5, 100}, {2.5, 75}, {4.0, 50}, {8.0, 25}, {16.0, 0}
		}) : 100;

	// Also check absolute stability at top speed — cap score based on
	// raw worst/best ratio regardless of baseline comparison.
	if (stabilityAvailable) {
		double maxStability = 0.0;
		for (int s = 0; s < NUM_SPEEDS; s++)
			if (stabilityMeasured[s]) maxStability = std::max(maxStability, avgStabilityRatio[s]);
		int absoluteStabilityCap = ContinuousScore(maxStability, {
			{1.0, 100}, {3.0, 75}, {5.0, 50}, {10.0, 25}, {20.0, 0}
			});
		stabilityScore = std::min(stabilityScore, absoluteStabilityCap);
	}

	// Detect drive speed ceiling — mirror of the baseline detection but from
	// the top.  If the drive caps its speed, adjacent high-speed steps will
	// have nearly identical read times.  Only collapse steps where the higher
	// requested speed is genuinely faster (or equal) — a regression where the
	// "faster" setting produces equal or slower times is a wobble signal, NOT
	// a speed cap.  Use strict less-than to avoid collapsing ties.
	int ceilingIdx = NUM_SPEEDS - 1;
	while (ceilingIdx > baselineIdx && avgReadTimeMs[ceilingIdx] < 0.001)
		ceilingIdx--;
	for (int s = ceilingIdx - 1; s > baselineIdx; s--) {
		if (avgReadTimeMs[s] < 0.001 || avgReadTimeMs[ceilingIdx] < 0.001) break;
		double ratio = avgReadTimeMs[s] / avgReadTimeMs[ceilingIdx];
		if (ratio < 1.15 && avgReadTimeMs[ceilingIdx] < avgReadTimeMs[s]) ceilingIdx = s;
		else break;
	}

	// Speed scaling score: detect when the drive fails to go faster at higher
	// speed settings.  Wobble causes the servo to struggle, so the drive
	// plateaus or even regresses — read times stop decreasing or increase.
	// Compare adjacent speed pairs between baseline and ceiling only.
	int scalingScore = 100;
	int scalingPenalties = 0;
	for (int s = baselineIdx + 1; s <= ceilingIdx; s++) {
		int prev = (s == baselineIdx + 1) ? baselineIdx : s - 1;
		if (avgReadTimeMs[s] < 0.001 || avgReadTimeMs[prev] < 0.001) continue;

		double timeRatio = avgReadTimeMs[s] / avgReadTimeMs[prev];

		if (timeRatio >= 1.0) {
			// Regression: higher speed is actually slower (or equal)
			scalingPenalties += 2;
		}
		else if (timeRatio > 0.95) {
			// Plateau: < 5% improvement despite a speed step increase
			scalingPenalties += 1;
		}
	}

	// After collecting avgReadTimeMs[], detect if the drive throttled.
	// If the requested speed doubled but read time barely changed, the drive
	// refused to go faster — a strong wobble indicator.

	// Estimate the drive's actual speed at the baseline index.
	// If the drive ignored lower requested speeds and ran at its own minimum,
	// use the first differentiated speed step to back-calculate from read
	// times (time is inversely proportional to speed).
	double actualBaselineSpeed = static_cast<double>(speeds[baselineIdx]);
	if (baselineIdx + 1 < NUM_SPEEDS
		&& avgReadTimeMs[baselineIdx] > 0.001
		&& avgReadTimeMs[baselineIdx + 1] > 0.001) {
		double estimated = speeds[baselineIdx + 1]
			* avgReadTimeMs[baselineIdx + 1] / avgReadTimeMs[baselineIdx];
		// Clamp: actual speed is between requested and next step
		actualBaselineSpeed = std::clamp(estimated,
			static_cast<double>(speeds[baselineIdx]),
			static_cast<double>(speeds[baselineIdx + 1]));
	}

	int throttlePenalties = 0;
	for (int s = baselineIdx + 1; s <= ceilingIdx; s++) {
		if (avgReadTimeMs[s] < 0.001 || avgReadTimeMs[baselineIdx] < 0.001)
			continue;

		// Expected time ratio if speed scaling were perfect
		double expectedRatio = actualBaselineSpeed / speeds[s];
		// Actual time ratio
		double actualRatio = avgReadTimeMs[s] / avgReadTimeMs[baselineIdx];

		// If actual is more than 1.5× the expected, drive is throttling
		if (actualRatio > expectedRatio * 1.5)
			throttlePenalties++;
	}
	if (throttlePenalties >= 2 && scalingScore > 75) scalingScore = 75;
	if (throttlePenalties >= 3 && scalingScore > 50) scalingScore = 50;

	if (scalingPenalties == 0)      scalingScore = std::min(scalingScore, 100);
	else if (scalingPenalties == 1) scalingScore = std::min(scalingScore, 75);
	else if (scalingPenalties == 2) scalingScore = std::min(scalingScore, 50);
	else if (scalingPenalties <= 4) scalingScore = std::min(scalingScore, 25);
	else                            scalingScore = 0;

	// Final score: concordance-adjusted combination, weighted toward the
	// signals that actually track wobble.
	//
	// Rationale (B): the scored error axis (C1 plus LiteOn C2 or the READ CD
	// balance signal) climbs with speed on essentially
	// every disc through a given drive, so it's a weak wobble discriminator and
	// a strong false-positive source. Stability (per-sector servo hunting) and
	// scaling (drive failing to speed up / falling back) are the signals that
	// genuinely separate a warped disc from a flat one. So the verdict is driven
	// by the worst of {stability, scaling, jitter}; the error axis can pull the
	// score down only when it is severe, and is otherwise capped to an advisory
	// nudge. AccurateRip (external, in dBpoweramp) remains the real correctness
	// gate — this score only advises on physical disc condition.

	// Primary signals: the wobble-specific trio.
	int primaryScores[] = { stabilityScore, scalingScore, jitterScore };
	std::sort(std::begin(primaryScores), std::end(primaryScores));
	int worst = primaryScores[0];
	int secondWorst = primaryScores[1];

	// Blend worst and second-worst of the primary trio to soften a single
	// noisy metric.
	int blendedBase = (worst * 7 + secondWorst * 3) / 10;
	blendedBase = std::min(blendedBase, worst + 10);

	// Concordance penalty: only among the primary wobble signals. Each
	// additional degraded primary signal (<75) adds confidence the wobble is
	// real rather than measurement noise.
	int degradedCount = 0;
	for (int sc : primaryScores) {
		if (sc < 75) degradedCount++;
	}
	int concordancePenalty = (degradedCount >= 2) ? (degradedCount - 1) * 5 : 0;

	// Apply the error-axis cap, then the concordance penalty. The error axis
	// only matters when it's clearly bad: a mild error climb (errScore >= 60)
	// is treated as normal drive behavior and ignored for the verdict. Below
	// that it caps the score, preserving severity down to a floor of 35. A low
	// errScore means the disc errors at a speed in the scored range. The 35
	// floor keeps "error alone" from reaching the deepest POOR scores, which
	// still require a primary wobble signal (stability/scaling/jitter) to agree.
	// AccurateRip (external, in dBpoweramp) remains the real correctness gate.
	auto FinalizeScore = [&](int errScore) -> int {
		int blended = blendedBase;
		if (errScore < 60) {
			int errorCap = std::max(35, errScore);
			blended = std::min(blended, errorCap);
		}
		return std::max(0, blended - concordancePenalty);
	};

	// Primary score: error axis limited to audio-relevant speeds (<=16x). This
	// is the value returned to the caller and used for the rip recommendation.
	balanceScore = FinalizeScore(errorScore);
	// Secondary score: error axis includes the full speed sweep (24/32/40x).
	// Reported alongside the primary score as a mechanical-health view; it does
	// not change the audio rip recommendation.
	int balanceScoreFull = FinalizeScore(errorScoreFull);

	// Determine the highest speed that showed no wobble degradation.
	// Walk up from baseline; stop at the first speed with a regression,
	// plateau, fallback, or significant error/stability increase.
	int safeSpeedIdx = baselineIdx;
	for (int s = baselineIdx + 1; s < NUM_SPEEDS; s++) {
		if (validReadSamplesPerSpeed[s] < minValidSamples) break;
		if (speedFellBack[s] || eccFellBack[s]) break;

		// Check for timing regression or plateau
		int prev = (s == baselineIdx + 1) ? baselineIdx : s - 1;
		if (avgReadTimeMs[s] > 0.001 && avgReadTimeMs[prev] > 0.001) {
			double timeRatio = avgReadTimeMs[s] / avgReadTimeMs[prev];
			if (timeRatio >= 0.95) break;  // plateau or regression

			// Also check against expected scaling from baseline
			if (avgReadTimeMs[baselineIdx] > 0.001) {
				double expectedRatio = actualBaselineSpeed / speeds[s];
				double actualRatio = avgReadTimeMs[s] / avgReadTimeMs[baselineIdx];
				if (actualRatio > expectedRatio * 1.5) break;  // throttled
			}
		}

		// Check for stability degradation
		if (!stabilityMeasured[s] || !stabilityMeasured[baselineIdx]) break;
		if (avgStabilityRatio[s] > 2.0 * avgStabilityRatio[baselineIdx])
			break;

		// Check for ECC error spike (if available)
		if (usingHwEcc) {
			double baseC1 = std::max(hwC1PerSpeed[baselineIdx], 1.0);
			if (hwSamplesPerSpeed[s] == 0) break;
			if (hwC1PerSpeed[s] / baseC1 > 3.0) break;
			// Pioneer E22 is diagnostic-only and cannot lower the suggested speed.
			if (!hasPioneerHwC1 && hwSecondStagePerSpeed[s] > 0.5) break;
		}

		safeSpeedIdx = s;
	}
	int safeSpeed = speeds[safeSpeedIdx];

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
				<< avgReadErrorSignalPerSpeed[s] << "\n";
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
			std::cout << "  " << std::setw(3) << speeds[s] << "x:  C1 "
				<< std::fixed << std::setprecision(1) << std::setw(6) << hwC1PerSpeed[s]
				<< "/sec   " << hwSecondStageLabel << " "
				<< std::setprecision(1) << std::setw(6) << hwSecondStagePerSpeed[s]
				<< "/sec";
			if (hwSamplesPerSpeed[s] == 0)
				std::cout << "  (no samples)";
			if (speedFellBack[s] || eccFellBack[s])
				std::cout << "  ** FALLBACK (drive can't sustain this speed) **";
			else if (clampedActualX[s] > 0)
				std::cout << "  (ran at ~" << clampedActualX[s] << "x - drive floor)";
			else if (speeds[s] > MAX_AUDIO_RELEVANT_SPEED)
				std::cout << "  (full-speed score only - above audio-relevant range)";
			std::cout << "\n";
		}
		if (hasPioneerHwC1) {
			std::cout << "  NOTE: Pioneer E22 is a raw diagnostic counter, not verified C2/E32\n"
				<< "        or CU. It is reported here but does not affect the Balance Score\n"
				<< "        or Suggested Max Rip Speed and is not a copy-integrity trigger.\n";
		}
		// Consistency with the quality / BLER / rot reports: absolute C1 and E22
		// rates are only archivally meaningful at or below the archival speed
		// ceiling. Above it the figures still compare against each other across
		// the sweep — which is exactly what this check needs, and is valid at
		// any speed — but they are not a disc-quality verdict.
		ScanQuality::PrintWrapped(std::cout,
			std::string("Rates above ") +
			std::to_string(ScanQuality::kArchivalScanSpeedMax) +
			"x are comparative only. This check reads them as a trend across the "
			"sweep, which stays valid at any speed; the same figures are not a "
			"quality verdict about the disc. The CD quality scan gives that.",
			"  ");
		if (hwSweepFailed) {
			std::cout << "  ** NOTE: The hardware ECC sweep failed; partial hardware data\n"
				<< "     was discarded from scoring. Using the READ CD/read-stability\n"
				<< "     signal plus Jitter/Scaling for the verdict. **\n";
		}
		else if (hwEccFlat) {
			std::cout << "  ** NOTE: Scored hardware ECC rates are flat, incomplete, or unavailable\n"
				<< "     across the requested speed settings.\n"
				<< "     The hardware scan firmware likely ignores SetSpeed - per-speed\n"
				<< "     comparison is not meaningful. Relying on READ CD/read-stability\n"
				<< "     plus Jitter/Scaling instead of hardware ECC for wobble. **\n";
		}
		if (hwEccFlat) {
			std::cout << "\n";
			PrintReadSignalReport();
		}
		// Note which speeds contributed to scoring
		bool anyAboveCeiling = false;
		for (int s = errorCeilingIdx + 1; s < NUM_SPEEDS; s++) {
			if (hwC1PerSpeed[s] > 0.0
				|| (!hasPioneerHwC1 && hwSecondStagePerSpeed[s] > 0.0))
				anyAboveCeiling = true;
		}
		if (anyAboveCeiling) {
			std::cout << "  Note: Hardware counters above " << MAX_AUDIO_RELEVANT_SPEED
				<< "x do not affect the primary (audio) score - no audio player\n"
				<< "        operates at those speeds. Scored counters are included in\n"
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
		// Annotate clamped low rows so duplicate-looking times are explained.
		if (clampedActualX[s] > 0)
			std::cout << "  (ran at ~" << clampedActualX[s] << "x)";
		// Flag speed regression/plateau inline
		if (s > baselineIdx && s <= ceilingIdx) {
			int prev = s - 1;
			if (avgReadTimeMs[prev] > 0.001 && avgReadTimeMs[s] >= avgReadTimeMs[prev])
				std::cout << "  ** REGRESSION **";
			else if (avgReadTimeMs[prev] > 0.001 && avgReadTimeMs[s] > avgReadTimeMs[prev] * 0.95)
				std::cout << "  * plateau *";
		}
		std::cout << "\n";
	}

	std::cout << "\n  Error Sub-Score:     " << errorScore << " / 100";
	if (usingHwEcc && hasPioneerHwC1)
		std::cout << "  (hardware C1; Pioneer E22 diagnostic-only)";
	else if (usingHwEcc)
		std::cout << "  (hardware C1/C2)";
	else if (hasReadCdC2 && hasHwC1 && hwEccFlat)
		std::cout << "  (READ CD/read-stability signal; hardware ECC ignored)";
	else if (hasReadCdC2)
		std::cout << "  (READ CD/read-stability signal)";
	else
		std::cout << "  (timing/read-failure signal; C2 unavailable)";
	std::cout << "\n";
	if (errorCeilingIdx < NUM_SPEEDS - 1) {
		std::cout << "  Error Sub-Score*:    " << errorScoreFull
			<< " / 100  (* full speed - includes 24/32/40x)\n";
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
	if (balanceScore >= 75)      std::cout << "  (GOOD - disc is well balanced)\n";
	else if (balanceScore >= 50) std::cout << "  (FAIR - some wobble detected, reduce rip speed)\n";
	else                         std::cout << "  (POOR - significant balance problem, use 4x-8x max)\n";

	// Full-speed (mechanical / full-RPM) score. Absent only if no speed above
	// the audio-relevant ceiling was swept (with the current fixed speed table
	// it is always present). When absent, the high-speed rows below report
	// "Not tested" rather than guessing from the audio score.
	bool haveFullScore = (errorCeilingIdx < NUM_SPEEDS - 1);
	int fullScore = balanceScoreFull;
	if (haveFullScore) {
		std::cout << "  Balance Score (full speed): " << balanceScoreFull << " / 100";
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
	InterpRow("Fast extraction >16x:",
		!haveFullScore ? "Not tested"
		: fullScore >= 75 ? "Fine"
		: fullScore >= 50 ? "Caution"
		: "Avoid");
	InterpRow("Full-speed drive scan:",
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
