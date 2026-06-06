#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
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
	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: C2 support required for disc balance check.\n";
		return false;
	}

	// Probe hardware quality-scan availability early — these probes print
	// diagnostics, so do them before the progress bar starts.  Prefer
	// Pioneer on Pioneer drives to avoid irrelevant LiteOn probe chatter.
	bool hasPioneerHwC1 = m_drive.SupportsPioneerScan();
	bool hasLiteOnHwC1 = !hasPioneerHwC1 && m_drive.SupportsLiteOnScan();
	bool hasHwC1 = hasPioneerHwC1 || hasLiteOnHwC1;

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
		double t = static_cast<double>(i) / SAMPLE_COUNT;
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
	std::vector<double> avgC2PerSpeed(NUM_SPEEDS, 0.0);
	std::vector<double> jitterCoeffVar(NUM_SPEEDS, 0.0);
	std::vector<double> avgReadTimeMs(NUM_SPEEDS, 0.0);
	std::vector<double> avgStabilityRatio(NUM_SPEEDS, 1.0);
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

		int totalC2 = 0, tested = 0;
		std::vector<double> readTimesMs;
		readTimesMs.reserve(sampleLBAs.size());
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
			int bestC2 = 0;
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
				bool ok = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2tmp);
				auto t1 = std::chrono::high_resolution_clock::now();
				double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

				if (ok) {
					anyOk = true;
					okCount++;
					if (ms < bestMs) {
						bestMs = ms;
						bestC2 = c2tmp;
					}
					if (ms > worstMs) worstMs = ms;
				}

				completed++;
				progress.Update(completed, totalTests);
			}

			if (anyOk) {
				readTimesMs.push_back(bestMs);
				// Use worst/best ratio as a per-sector wobble indicator
				if (bestMs > 0.001 && worstMs / bestMs > 3.0)
					totalC2 += 50;  // Synthetic penalty for high intra-sector variance
				totalC2 += bestC2;

				// Track per-sector read stability: worst/best ratio.
				// Wobble causes the same sector to read at wildly different
				// times on successive attempts due to servo hunting.
				if (okCount >= 2 && bestMs > 0.001) {
					stabilitySum += worstMs / bestMs;
					stabilityCount++;
				}
			}
			else {
				totalC2 += 100; // Penalize complete read failures
			}
			tested++;
		}
		avgC2PerSpeed[s] = (tested > 0) ? (double)totalC2 / tested : 0.0;

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
		// readTimesMs[] is already in ascending LBA order.
		if (speeds[s] >= 16 && readTimesMs.size() >= 10) {
			// Build paired (LBA, time) and sort by time to trim outliers
			struct Sample { double lba; double ms; };
			std::vector<Sample> samples(readTimesMs.size());
			for (size_t i = 0; i < readTimesMs.size(); i++) {
				samples[i] = { static_cast<double>(sampleLBAs[i]), readTimesMs[i] };
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
		avgStabilityRatio[s] = (stabilityCount > 0)
			? stabilitySum / stabilityCount : 1.0;
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

			int c2tmp = 0;
			auto t0 = std::chrono::high_resolution_clock::now();
			bool ok = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2tmp);
			auto t1 = std::chrono::high_resolution_clock::now();
			double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

			if (ok) { driftSum += ms; driftValid++; }
		}

		if (driftValid > 0 && avgReadTimeMs[0] > 0.001) {
			double retest = driftSum / driftValid;
			driftRatio = retest / avgReadTimeMs[0];
		}
	}

	m_drive.SetSpeed(0);

	// ── Hardware C1 sweep (Pioneer or LiteOn/MediaTek) ──────────────────
	// If the drive supports a hardware quality scan, collect per-speed C1
	// error rates from the hardware ECC decoder.  This bypasses the broken
	// READ CD C2 bitmap and gives real error data.  Each poll returns one
	// 75-sector time slice, so N polls ~ N seconds of measurement.
	std::vector<double> hwC1PerSpeed(NUM_SPEEDS, 0.0);
	std::vector<double> hwC2PerSpeed(NUM_SPEEDS, 0.0);
	std::vector<int> hwSamplesPerSpeed(NUM_SPEEDS, 0);
	bool hwEccFlat = false;  // True if ECC data has no per-speed discriminating power

	if (hasHwC1) {
		constexpr int HW_SAMPLES_PER_SPEED = 15;

		std::cout << "\nRunning hardware C1/C2 sweep ("
			<< (hasPioneerHwC1 ? "Pioneer" : "LiteOn/MediaTek")
			<< " ECC decoder)...\n";

		// Start scan from the outer 25% of the disc where wobble is worst
		DWORD outerStartLBA = maxLBA * 3 / 4;

		for (int s = 0; s < NUM_SPEEDS; s++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) break;

			m_drive.SetSpeed(speeds[s]);
			Sleep(300);

			bool started = hasPioneerHwC1
				? m_drive.PioneerScanStart(outerStartLBA, maxLBA)
				: m_drive.LiteOnScanStart(outerStartLBA, maxLBA);
			if (!started) continue;

			int totalC1 = 0, totalC2 = 0, validSamples = 0;
			bool cancelled = false;
			DWORD firstLBA = 0, lastLBA = 0;
			for (int i = 0; i < HW_SAMPLES_PER_SPEED + 3; i++) {
				if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
					cancelled = true;
					break;
				}

				int c1 = 0, c2 = 0, cu = 0;
				DWORD lba = 0;
				bool done = false;

				bool pollOk = hasPioneerHwC1
					? m_drive.PioneerScanPoll(c1, c2, cu, lba, done)
					: m_drive.LiteOnScanPoll(c1, c2, cu, lba, done);
				if (!pollOk) break;
				if (done) break;

				// Skip first 3 samples — drive reports accumulated startup errors
				if (i < 3) {
					if (i == 0) firstLBA = lba;
					continue;
				}

				totalC1 += c1;
				totalC2 += c2;
				validSamples++;
				lastLBA = lba;
			}

			// Always called — even on cancel.
			if (hasPioneerHwC1) m_drive.PioneerScanStop();
			else m_drive.LiteOnScanStop();

			// Log actual scan position for diagnostics
			char dbg[128];
			snprintf(dbg, sizeof(dbg), "HW ECC %dx: LBA %lu-%lu, %d samples, C1=%d C2=%d\n",
				speeds[s], (unsigned long)firstLBA, (unsigned long)lastLBA,
				validSamples, totalC1, totalC2);
			OutputDebugStringA(dbg);

			hwC1PerSpeed[s] = validSamples > 0
				? static_cast<double>(totalC1) / validSamples : 0.0;
			hwC2PerSpeed[s] = validSamples > 0
				? static_cast<double>(totalC2) / validSamples : 0.0;
			hwSamplesPerSpeed[s] = validSamples;

			if (cancelled) break;
		}

		// Detect flat ECC data: if the scan firmware ignores SetSpeed, all
		// speed steps return the same C1/C2 values and the per-speed
		// comparison is meaningless.  Check if all C1 values are below a
		// noise floor AND the spread across speeds is negligible.
		double maxErrors = 0.0, minErrors = 1e9;
		int hwValidSpeeds = 0;
		for (int s = 0; s < NUM_SPEEDS; s++) {
			if (hwSamplesPerSpeed[s] == 0) continue;
			hwValidSpeeds++;
			double errors = hwC1PerSpeed[s] + hwC2PerSpeed[s];
			if (errors > maxErrors) maxErrors = errors;
			if (errors < minErrors) minErrors = errors;
		}
		if (hwValidSpeeds < 2 || maxErrors < 0.5 || (maxErrors - minErrors) < 0.5) {
			hwEccFlat = true;
		}
	}

	m_drive.SetSpeed(0);
	m_drive.SpinDown();

	// ── Scoring: combine error signal, jitter, and scaling ──────────────

	// Baseline detection: skip clamped speeds at the bottom
	int baselineIdx = 0;
	for (int s = 1; s < NUM_SPEEDS; s++) {
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
			double prevErrors = hwC1PerSpeed[s - 1] + hwC2PerSpeed[s - 1];
			double curErrors = hwC1PerSpeed[s] + hwC2PerSpeed[s];
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
		int errorScore = 0;

		if (hasHwC1 && !hwEccFlat) {
			// Hardware C2 score: direct uncorrectable error rate from ECC decoder.
			// C2 climbs with speed on virtually every disc (it's a property of the
			// drive's high-speed read margin, not the disc), so a bare ratio against
			// a near-zero baseline cried wolf constantly. Two guards against that:
			//   1. A meaningful baseline floor (1.0/sec) so a normal climb from
			//      ~0 to a few C2/sec doesn't produce an enormous ratio.
			//   2. An absolute gate: only treat C2 as a wobble signal once the peak
			//      in-range rate clears a noise floor. A handful of C2/sec at 16x is
			//      normal drive behavior, not a warped disc.
			// AccurateRip (checked externally in dBpoweramp) is the real correctness
			// gate; this score is advisory, so it errs toward NOT flagging.
			constexpr double HW_C2_NOISE_FLOOR = 5.0;   // C2/sec below this = ignore
			double hwC2Baseline = std::max(hwC2PerSpeed[baselineIdx], 1.0);
			double peakHwC2Ratio = 0.0;
			double peakHwC2Abs = 0.0;
			for (int s = baselineIdx + 1; s <= ceilIdx; s++) {
				if (hwSamplesPerSpeed[s] == 0) continue;
				if (speedFellBack[s]) continue;
				if (hwC2PerSpeed[s] > peakHwC2Abs) peakHwC2Abs = hwC2PerSpeed[s];
				double ratio = hwC2PerSpeed[s] / hwC2Baseline;
				if (ratio > peakHwC2Ratio) peakHwC2Ratio = ratio;
			}
			bool anyHwC2 = (peakHwC2Abs >= HW_C2_NOISE_FLOOR);

			// If hardware C2 errors appear at higher speeds, use that signal
			// directly — it's the strongest indicator of balance problems. Widened
			// breakpoints: a normal drive climb (2-4x over baseline) stays in the
			// 90s; only a steep climb (10x+) starts seriously penalizing.
			if (anyHwC2 && peakHwC2Ratio > 1.0) {
				errorScore = ContinuousScore(peakHwC2Ratio, {
					{1.0, 100}, {4.0, 90}, {10.0, 70}, {25.0, 45}, {60.0, 15}, {120.0, 0}
					});
			}
			else {
				// No significant hardware C2 — fall back to C1 ratio analysis. C1 is
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
			// READ CD C2-bitmap path (no hardware ECC scan available). Same
			// philosophy: C2/sector climbs with speed on healthy discs, so only a
			// large climb over baseline is treated as a balance problem.
			double baseline = std::max(avgC2PerSpeed[baselineIdx], 1.0);
			double peakC2Ratio = 0.0;
			for (int s = baselineIdx + 1; s <= ceilIdx; s++) {
				if (speedFellBack[s]) continue;
				double ratio = avgC2PerSpeed[s] / baseline;
				if (ratio > peakC2Ratio) peakC2Ratio = ratio;
			}

			errorScore = ContinuousScore(peakC2Ratio, {
				{1.0, 100}, {3.0, 100}, {6.0, 80}, {15.0, 55}, {40.0, 25}, {80.0, 0}
				});
		}
		return errorScore;
	};

	// Primary error sub-score: audio-relevant speeds only (<=16x).
	int errorScore = ComputeErrorScore(errorCeilingIdx);
	// Secondary error sub-score: full sweep, including 24/32/40x. Clamped to the
	// primary score so the wider range can only reveal MORE degradation, never
	// less. (The scorer can switch between its C2-priority and C1-fallback paths
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
	double baselineStability = std::max(avgStabilityRatio[baselineIdx], 1.001);
	double peakStabilityRatio = 0.0;
	for (int s = baselineIdx + 1; s < NUM_SPEEDS; s++) {
		double ratio = avgStabilityRatio[s] / baselineStability;
		if (ratio > peakStabilityRatio) peakStabilityRatio = ratio;
	}

	int stabilityScore = ContinuousScore(peakStabilityRatio, {
		{1.0, 100}, {1.5, 100}, {2.5, 75}, {4.0, 50}, {8.0, 25}, {16.0, 0}
		});

	// Also check absolute stability at top speed — cap score based on
	// raw worst/best ratio regardless of baseline comparison.
	double maxStability = *std::max_element(avgStabilityRatio.begin(),
		avgStabilityRatio.end());
	int absoluteStabilityCap = ContinuousScore(maxStability, {
		{1.0, 100}, {3.0, 75}, {5.0, 50}, {10.0, 25}, {20.0, 0}
		});
	stabilityScore = std::min(stabilityScore, absoluteStabilityCap);

	// Detect drive speed ceiling — mirror of the baseline detection but from
	// the top.  If the drive caps its speed, adjacent high-speed steps will
	// have nearly identical read times.  Only collapse steps where the higher
	// requested speed is genuinely faster (or equal) — a regression where the
	// "faster" setting produces equal or slower times is a wobble signal, NOT
	// a speed cap.  Use strict less-than to avoid collapsing ties.
	int ceilingIdx = NUM_SPEEDS - 1;
	for (int s = NUM_SPEEDS - 2; s > baselineIdx; s--) {
		if (avgReadTimeMs[s] > 0.001 && avgReadTimeMs[ceilingIdx] > 0.001) {
			double ratio = avgReadTimeMs[s] / avgReadTimeMs[ceilingIdx];
			if (ratio < 1.15 && avgReadTimeMs[ceilingIdx] < avgReadTimeMs[s]) {
				ceilingIdx = s;
			}
			else {
				break;
			}
		}
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
	// Rationale (B): the error axis (C1/C2) climbs with speed on essentially
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
		if (avgStabilityRatio[s] > 2.0 * avgStabilityRatio[baselineIdx])
			break;

		// Check for ECC error spike (if available)
		if (usingHwEcc) {
			double baseC1 = std::max(hwC1PerSpeed[baselineIdx], 1.0);
			if (hwSamplesPerSpeed[s] == 0) break;
			if (hwC1PerSpeed[s] / baseC1 > 3.0) break;
			if (hwC2PerSpeed[s] > 0.5) break;
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

	if (hasHwC1) {
		std::cout << "--- Hardware Error Rates by Speed (ECC decoder) ---\n";
		for (int s = 0; s < NUM_SPEEDS; s++) {
			std::cout << "  " << std::setw(3) << speeds[s] << "x:  C1 "
				<< std::fixed << std::setprecision(1) << std::setw(6) << hwC1PerSpeed[s]
				<< "/sec   C2 " << std::setprecision(1) << std::setw(6) << hwC2PerSpeed[s]
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
		if (hwEccFlat) {
			std::cout << "  ** NOTE: C1/C2 rates are flat across all speed settings.\n"
				<< "     The hardware scan firmware likely ignores SetSpeed - per-speed\n"
				<< "     comparison is not meaningful. Relying on timing-based\n"
				<< "     scoring (Jitter/Scaling) instead of ECC for wobble. **\n";
		}
		// Note which speeds contributed to scoring
		bool anyAboveCeiling = false;
		for (int s = errorCeilingIdx + 1; s < NUM_SPEEDS; s++) {
			if (hwC1PerSpeed[s] > 0.0 || hwC2PerSpeed[s] > 0.0)
				anyAboveCeiling = true;
		}
		if (anyAboveCeiling) {
			std::cout << "  Note: Errors above " << MAX_AUDIO_RELEVANT_SPEED
				<< "x do not affect the primary (audio) score - no audio player\n"
				<< "        operates at those speeds. They are scored separately in\n"
				<< "        the full-speed balance score below.\n";
		}
	}
	else {
		std::cout << "--- C2 Errors by Speed ---\n";
		for (int s = 0; s < NUM_SPEEDS; s++) {
			std::cout << "  " << std::setw(3) << speeds[s] << "x:  "
				<< std::fixed << std::setprecision(2) << avgC2PerSpeed[s]
				<< " avg C2/sector\n";
		}

		// Warn when C2 reports zero but timing-based metrics found problems.
		// This combination strongly suggests the drive accepts C2 commands
		// without actually populating the error pointer data.
		bool allC2Zero = true;
		for (int s = 0; s < NUM_SPEEDS; s++) {
			if (avgC2PerSpeed[s] > 0.0) { allC2Zero = false; break; }
		}
		if (allC2Zero && scalingScore < 100) {
			std::cout << "  ** NOTE: C2 reports 0 errors at all speeds, but timing\n"
				<< "     detected wobble. C2 data may not be functional on this\n"
				<< "     drive. Rely on Scaling/Jitter scores instead. **\n";
		}
	}

	std::cout << "\n--- Read Time Jitter by Speed ---\n";
	for (int s = 0; s < NUM_SPEEDS; s++) {
		std::cout << "  " << std::setw(3) << speeds[s] << "x:  CV "
			<< std::fixed << std::setprecision(3) << jitterCoeffVar[s]
			<< "  (avg " << std::setprecision(1) << avgReadTimeMs[s] << " ms)"
			<< "  stability " << std::setprecision(2) << avgStabilityRatio[s] << "x";
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
	if (usingHwEcc) std::cout << "  (hardware ECC)";
	else if (hasHwC1 && hwEccFlat) std::cout << "  (C2 bitmap - ECC flat, ignored)";
	else            std::cout << "  (C2 bitmap)";
	std::cout << "\n";
	if (errorCeilingIdx < NUM_SPEEDS - 1) {
		std::cout << "  Error Sub-Score*:    " << errorScoreFull
			<< " / 100  (* full speed - includes 24/32/40x)\n";
	}
	std::cout << "  Jitter Sub-Score:    " << jitterScore << " / 100\n";
	std::cout << "  Stability Sub-Score: " << stabilityScore << " / 100  (per-sector read consistency)\n";
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

	std::cout << "\n  Max Safe Rip Speed: " << safeSpeed << "x\n";

	if (balanceScore < 75) {
		std::cout << "\n  Recommendation:\n";
		if (balanceScore < 50) {
			std::cout << "    - Re-rip at " << safeSpeed
				<< "x or lower with Secure or Paranoid mode.\n";
			std::cout << "    - Any previous rip above " << safeSpeed
				<< "x should be considered suspect.\n";
			std::cout << "    - Verify against AccurateRip -- if CRCs match,\n"
				<< "      the existing rip is bit-perfect despite wobble.\n";
		}
		else {
			std::cout << "    - Rips at or below " << safeSpeed
				<< "x are reliable.\n";
			std::cout << "    - Rips above " << safeSpeed
				<< "x without Secure mode should be re-verified.\n";
			std::cout << "    - AccurateRip match = no re-copy needed.\n";
		}
	}

	std::cout << std::string(60, '=') << "\n";
	return true;
}
