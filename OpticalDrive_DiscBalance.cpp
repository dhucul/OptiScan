#define NOMINMAX
#include "OpticalDrive.h"
#include "HardwareSweep.h"
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
// Disc Balance Check - Compares read timing and stability across speed settings
// ============================================================================

bool OpticalDrive::CheckDiscBalance(DiscInfo& disc, int& balanceScore, std::string* savedReport) {
	if (savedReport) savedReport->clear();
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
	DriveCapabilities cacheCapabilities;
	if (hasHwC1) {
		m_drive.DetectCapabilities(cacheCapabilities);
		if (!m_drive.IsOpen()) { std::cout << "ERROR: Drive closed during capability detection. Reopen it.\n"; return false; }
	}
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
	DiscRot::AudioRanges audioRanges;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		const DWORD start = t.trackNumber == 1 ? 0 : t.pregapLBA;
		if (t.endLBA < start) continue;
		audioRanges.emplace_back(start,t.endLBA);
		maxLBA = std::max(maxLBA,t.endLBA);
	}
	audioRanges = DiscRot::NormalizeAudioRanges(std::move(audioRanges));
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
	progress.SetShowTransferRate(false);
	progress.Start();

	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
	// The balance error signal combines READ CD C2 pointer counts (when
	// available) with explicit penalties for unstable or failed reads. Keep the
	// raw C2 rate separate so synthetic timing penalties are never presented as
	// measured C2 data.
	std::vector<double> avgReadErrorSignalPerSpeed(NUM_SPEEDS, 0.0);
	std::vector<double> avgReadCdC2PerSpeed(NUM_SPEEDS, 0.0);
	std::vector<long long> readCdC2Totals(NUM_SPEEDS, 0);
	std::vector<int> successfulC2Reads(NUM_SPEEDS, 0), c2PositiveReads(NUM_SPEEDS, 0);
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

		double totalReadErrorSignal = 0;
		int tested = 0;
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
			Diagnostics::BalanceReadRepeats repeats;

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

				repeats.Record(ok, ms, c2tmp);

				completed++;
				progress.Update(completed, totalTests);
			}

			if (repeats.successfulReads > 0) {
				const double bestMs = repeats.bestMs, worstMs = repeats.worstMs;
				readTimesMs.push_back(bestMs);
				successfulReadLBAs.push_back(lba);
				validReadSamplesPerSpeed[s]++;
				// Use worst/best ratio as a per-sector wobble indicator
				if (bestMs > 0.001 && worstMs / bestMs > 3.0)
					totalReadErrorSignal += 50;  // Synthetic balance penalty, not C2
				readCdC2Totals[s] += repeats.c2Total;
				successfulC2Reads[s] += repeats.successfulReads;
				c2PositiveReads[s] += repeats.c2PositiveReads;
				totalReadErrorSignal += repeats.AverageC2();

				// Track per-sector read stability: worst/best ratio.
				// Wobble causes the same sector to read at wildly different
				// times on successive attempts due to servo hunting.
				if (repeats.successfulReads >= 2 && bestMs > 0.001) {
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
		avgReadCdC2PerSpeed[s] = (successfulC2Reads[s] > 0)
			? static_cast<double>(readCdC2Totals[s]) / successfulC2Reads[s] : 0.0;

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
	Diagnostics::BalanceReadRepeats driftC2;
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

			driftC2.Record(ok, ms, c2tmp);
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
	std::vector<Diagnostics::HardwareSweepPass> hwPasses(NUM_SPEEDS);
	std::vector<double> hwC1PerSpeed(NUM_SPEEDS, 0.0);
	std::vector<double> hwSecondStagePerSpeed(NUM_SPEEDS, 0.0);
	std::vector<int> hwSamplesPerSpeed(NUM_SPEEDS, 0);
	bool hwSweepFailed = false;
	const char* hwSecondStageLabel = hasPioneerHwC1 ? "E22" : "C2";

	if (hasHwC1) {
		const auto window = Diagnostics::HardwareSweepRange(audioRanges,maxLBA*3/4);
		if (!window) {
			hwSweepFailed = true;
			for (auto& pass : hwPasses) pass.limitation = "no contiguous aligned startup plus 15-second target window";
			std::cout << "Hardware sweep unavailable: need an aligned 10-second startup plus 15-second target in contiguous audio.\n";
		}
		else {
            const DWORD scanFirst=window->first-Diagnostics::kQualityStartupSectors;
			std::cout << "\nRunning hardware C1/" << hwSecondStageLabel << " sweep over LBAs "
				<< window->first << "-" << window->second << ".\n"
				<< "Aligned 10-second startup: LBAs " << scanFirst << "-" << window->first-1 << ".\n"
                << "The scan continues into the target without restarting; all startup counts are retained.\n";
			auto cancelled = [] { return g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey(); };
			for (int step=0; step<NUM_SPEEDS; ++step) {
				if (cancelled()) return false;
				m_drive.SetSpeed(speeds[step]);
				Sleep(300);
				auto pass = Diagnostics::MeasureHardwareSweep(window->first,window->second,
					[&] {
						return DiscRot::EvictAudioCacheRange(scanFirst,window->second,audioRanges,
							cacheCapabilities.bufferSizeKB,
							[&](DWORD start,DWORD count,BYTE* data) { return m_drive.ReadSectorsAudioOnly(start,count,data); },
							cancelled);
					},
					[&](DWORD first,DWORD last) {
						return hasPioneerHwC1 ? m_drive.PioneerScanStart(first,last) : m_drive.LiteOnScanStart(first,last);
					},
					[&](Diagnostics::HardwareSweepSample& sample) {
						auto poll = [&] {
							return hasPioneerHwC1
								? m_drive.PioneerScanPoll(sample.c1,sample.secondStage,sample.cu,sample.lba,sample.done,&sample.valid,&sample.sectors)
								: m_drive.LiteOnScanPoll(sample.c1,sample.secondStage,sample.cu,sample.lba,sample.done,&sample.sectors,&sample.valid);
						};
						bool ok=poll();
						if (!ok && hasPioneerHwC1 && !cancelled()) {
							std::this_thread::sleep_for(std::chrono::milliseconds(200));
							sample={}; ok=poll();
						}
						return ok;
					},
					[&] { return hasPioneerHwC1 ? m_drive.PioneerScanStop() : m_drive.LiteOnScanStop(); },
					[&](WORD& read) { WORD write=0; return m_drive.GetActualSpeed(read,write); },
					cancelled,
					[] { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now().time_since_epoch()).count()); },
                    [&](const Diagnostics::HardwareSweepPass& live) {
                        std::cout << "\r  Hardware scan: " << Diagnostics::ScanSpeedText(live.throughput.CurrentX())
                            << " measured throughput; startup " << live.startup.samples << "/10, target " << live.intervals.size() << "/15 samples   " << std::flush;
                    },Diagnostics::kQualityStartupSectors,hasLiteOnHwC1 && m_drive.LiteOnScanMeasuresCu());
                std::cout << "\n";
				hwPasses[step] = std::move(pass);
				const auto& observed = hwPasses[step];
				if (observed.cleanupFailed) {
					std::cout << "ERROR: Hardware scan cleanup failed; drive closed. Reopen it.\n";
					m_drive.Close(); return false;
				}
				if (observed.cancelled || cancelled()) return false;
				if (observed.Qualified()) {
					hwC1PerSpeed[step]=observed.c1.average;
					hwSecondStagePerSpeed[step]=observed.secondStageTotal/observed.c1.MeasuredSeconds();
					hwSamplesPerSpeed[step]=static_cast<int>(observed.intervals.size());
				}
				if (!observed.complete) {
					hwSweepFailed=true;
					std::cout << "Hardware pass incomplete at requested " << speeds[step] << "x: " << observed.limitation << "\n";
					break;
				}
			}
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
			avgReadErrorSignalPerSpeed[s], hwC1PerSpeed[s], hwSecondStagePerSpeed[s], hwSamplesPerSpeed[s],
			hwPasses[s].ActualSpeed(),hwPasses[s].Qualified()});
	}
    Diagnostics::BalanceReadEvidence readEvidence;
    if (hasReadCdC2)
        readEvidence.readCdC2Total = std::accumulate(readCdC2Totals.begin(),readCdC2Totals.end(),driftC2.c2Total);
    for (const auto& pass:hwPasses)
        Diagnostics::AccumulateBalanceHardwareEvidence(readEvidence,pass,hasLiteOnHwC1);
    if (balanceCdCheck.validSamples>0)
        readEvidence.pioneerUncorrectableBytes=balanceCdCheck.worstC2Bytes;
	const auto assessment = Diagnostics::AssessBalance(measurements, requestedSamples,
		minValidSamples, hasHwC1, hasPioneerHwC1,readEvidence);
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

	std::ostringstream report;
	report << "\nDisc layout: " << Diagnostics::ScanDiscIdentity(disc) << "\n";
	report << "Readback is the drive-reported speed; each table row is one pass.\n";
    if (hasHwC1) report << "Sample grid: 75 sectors, origin LBA " << audioRanges.front().first << "; ten seconds of startup audio before each target.\n";
	if (hasHwC1) report << "Hardware scan method: " << (hasPioneerHwC1 ? "Pioneer (0x3B/0x3C)" : m_drive.LiteOnScanMethodName()) << "\n";

	// Keep report padding independent of any earlier time formatting.
	const int reportWidth = 60;
	const char* reportTitle = "DISC BALANCE CHECK";
	report << std::setfill(' ') << std::right;
	report << "\n" << std::string(reportWidth, '=') << "\n";
	report << std::string((reportWidth - static_cast<int>(std::strlen(reportTitle))) / 2, ' ')
		<< reportTitle << "\n";
	report << std::string(reportWidth, '=') << "\n";
	report << "  (Compares read timing and stability across speed settings)\n\n";
	report << "  Timing comparison includes drive-reported speeds through " << assessment.maximumMeasuredSpeed << "x.\n";
	if (assessment.partial) report << "  PARTIAL SPEED COVERAGE - unverified or insufficient timing rows are excluded.\n";
	report << "\n  Balance Score: " << balanceScore << " / 100 ("
        << (balanceScore>=75 ? "GOOD" : balanceScore>=50 ? "FAIR" : "POOR") << ")\n";
    if (assessment.haveFullScore)
        report << "  Wider speed range: " << balanceScoreFull << " / 100\n";
    report << "  Audio extraction: " << Diagnostics::BalanceExtractionGuidance(assessment) << "\n";
    Diagnostics::PrintBalanceRipRecommendation(report,assessment);
    report << "\n  Scores describe sampled read performance at the measured speeds.\n"
        << "  They do not establish physical wobble or verify extraction accuracy.\n\n";
    if (hasHwC1)
        report << "  Hardware counts cover the same 15-second target at each setting.\n"
            << "  Startup counts are separate. Errors elsewhere on the disc can be missed.\n\n";
	if (balanceCdCheckAttempted) {
		report << "--- Pioneer CD Check Data-Loss Cross-Check ---\n";
		if (balanceCdCheck.reliable) {
			report << "  Coverage: Quick radial sampling (0.05 mm), "
				<< balanceCdCheck.validSamples << " / "
				<< balanceCdCheck.plannedSamples << " samples\n";
			report << "  Worst C1 uncorrectable: "
				<< balanceCdCheck.worstC1Frames << " frames\n";
			report << "  Worst C2 uncorrectable: "
				<< balanceCdCheck.worstC2Bytes << " bytes";
			if (balanceCdCheck.worstC2Bytes == 0)
				report << "  (none in sampled windows)\n";
			else
				report << "  ** DATA LOSS DETECTED **\n";
			report << "  This sampled result is reported separately and does not change\n"
				<< "  the mechanical Balance Score; uncorrectable data overrides extraction advice.\n\n";
		}
		else {
            if (readEvidence.pioneerUncorrectableBytes>0)
                report << "  DATA LOSS OBSERVED: " << readEvidence.pioneerUncorrectableBytes << " uncorrectable bytes in a valid partial sample.\n";
			report << "  Full cross-check status: UNMEASURED ("
				<< (balanceCdCheck.failureReason.empty()
					? "no complete valid measurement" : balanceCdCheck.failureReason)
				<< ")\n"
				<< "  A missing measurement is not reported as a clean zero.\n\n";
		}
	}
	auto PrintReadSignalReport = [&]() {
        report << "--- READ CD observations (all successful repeats) ---\n"
            << "  Request  Readback   C2 total  Positive/successful  C2/read  Balance signal\n";
        for (int s=0;s<NUM_SPEEDS;++s) {
            report << std::right << std::setw(8) << (std::to_string(speeds[s])+"x")
                << std::setw(10) << (actualSpeedX[s]>0 ? "~"+std::to_string(actualSpeedX[s])+"x" : "--");
            if (hasReadCdC2 && successfulC2Reads[s]>0) {
                report << std::setw(11) << readCdC2Totals[s] << std::setw(21)
                    << (std::to_string(c2PositiveReads[s])+"/"+std::to_string(successfulC2Reads[s]))
                    << std::fixed << std::setprecision(2) << std::setw(9) << avgReadCdC2PerSpeed[s];
            }
            else report << std::setw(11) << "--" << std::setw(21) << "--" << std::setw(9) << "--";
            report << std::fixed << std::setprecision(2) << std::setw(16) << avgReadErrorSignalPerSpeed[s];
            if (!assessment.compared[s]) report << " (excluded)";
            report << '\n';
        }
        if (hasReadCdC2) {
            report << "  " << sampleLBAs.size()*READS_PER_SAMPLE << " reads attempted per setting; failed reads are not clean zeros.\n"
                << "  Totals count repeated observations, not unique damaged bytes.\n"
                << "  Timing re-test C2: " << driftC2.c2Total << " across " << driftC2.successfulReads << " successful reads.\n"
                << "  READ CD pointers and hardware decoder counters are separate measurements.\n";
        }
        report << "  Balance signal also includes timing/read-failure penalties; it is not a C2 count.\n";
    };
    std::vector<Diagnostics::HardwareSweepReportRow> hardwareRows;

	if (hasHwC1) {
		report << "--- Hardware observations (target totals by pass) ---\n";
		for (int s=0; s<NUM_SPEEDS; ++s)
			hardwareRows.push_back({hwPasses[s],speeds[s],actualSpeedX[s],assessment.compared[s],
				assessment.primaryCompared[s],speedFellBack[s] || eccFellBack[s]});
		Diagnostics::PrintHardwareSweepSummary(report,hardwareRows,hwSecondStageLabel);

		if (hasPioneerHwC1) {
			report << "  NOTE: Pioneer E22 is a raw diagnostic counter, not verified C2/E32\n"
				<< "        or CU. It is reported here but does not affect the Balance Score\n"
				<< "        or suggested rip setting and is not a copy-integrity trigger.\n";
		}
        report << "  C1: lower rates are preferable under comparable scan conditions.\n"
            << "  C1/sec = total C1 / measured audio seconds; longer scans can accumulate more errors.\n"
            << "  Bands: EXCELLENT <5; GOOD 5-<50; FAIR 50-<220; POOR >=220/sec.\n"
            << "  These are OptiScan's descriptive bands for this target, not whole-disc grades.\n";
		if (hwSweepFailed) {
			report << "  ** NOTE: Some hardware speed buckets were incomplete. Only completed\n"
				<< "     buckets at qualified measured speeds can contribute to scoring. **\n";
		}
		else if (!usingHwEcc) {
            report << "  Score basis: READ CD/read stability and timing; hardware rates are not comparable.\n";
        }
		if (!usingHwEcc) {
			report << "\n";
			PrintReadSignalReport();
		}
		// Note which speeds contributed to scoring
		bool anyAboveCeiling = false;
		for (int s = 0; s < NUM_SPEEDS; s++) {
			if (!assessment.compared[s] || assessment.primaryCompared[s] || !hwPasses[s].Qualified() ||
				hwPasses[s].ActualSpeed()!=actualSpeedX[s]) continue;
			if (hwC1PerSpeed[s] > 0.0
				|| (!hasPioneerHwC1 && hwSecondStagePerSpeed[s] > 0.0))
				anyAboveCeiling = true;
		}
		if (anyAboveCeiling) {
			report << "  Note: Hardware counters above " << assessment.primaryMaximumSpeed
				<< "x do not affect the primary measured-range score.\n"
				<< "        Qualified counters at higher measured speeds are included in\n"
				<< "        the wider-range balance score above.\n";
		}
	}
	else {
		PrintReadSignalReport();
	}

	report << "\n--- Read timing ---\n"
        << "  Request  Readback   Mean(ms)   Jitter(CV)  Stability  Sectors read\n";
    for (int s=0;s<NUM_SPEEDS;++s) {
        report << std::right << std::setw(8) << (std::to_string(speeds[s])+"x")
            << std::setw(10) << (actualSpeedX[s]>0 ? "~"+std::to_string(actualSpeedX[s])+"x" : "--");
        if (validReadSamplesPerSpeed[s]>0)
            report << std::fixed << std::setprecision(1) << std::setw(11) << avgReadTimeMs[s]
                << std::setprecision(3) << std::setw(13) << jitterCoeffVar[s];
        else report << std::setw(11) << "--" << std::setw(13) << "--";
        if (stabilityMeasured[s]) report << std::setprecision(2) << std::setw(11) << avgStabilityRatio[s];
        else report << std::setw(11) << "--";
        report << std::setw(14) << (std::to_string(validReadSamplesPerSpeed[s])+"/"+std::to_string(requestedSamples));
        if (!assessment.compared[s]) report << " (excluded)";
        else if (assessment.timingPenalty[s]>0) report << " *";
        report << '\n';
    }
    report << "  Read times use the fastest successful repeat, then a trimmed mean.\n"
        << "  * Limited speed gain or timing variation; the cause is not determined.\n";

	report << "\n  Error Sub-Score:     " << errorScore << " / 100";
	if (usingHwEcc && hasPioneerHwC1)
		report << "  (hardware C1; Pioneer E22 diagnostic-only)";
	else if (usingHwEcc)
		report << "  (hardware C1/C2)";
	else if (hasReadCdC2 && hasHwC1 && !usingHwEcc)
		report << "  (READ CD/read-stability signal; hardware ECC ignored)";
	else if (hasReadCdC2)
		report << "  (READ CD/read-stability signal)";
	else
		report << "  (timing/read-failure signal; C2 unavailable)";
	report << "\n";
	if (assessment.haveFullScore) {
		report << "  Error Sub-Score*:    " << errorScoreFull
			<< " / 100  (* wider verified speed range)\n";
	}
	report << "  Jitter Sub-Score:    " << jitterScore << " / 100\n";
	if (stabilityAvailable)
		report << "  Stability Sub-Score: " << stabilityScore << " / 100  (per-sector read consistency)\n";
	else
		report << "  Stability Sub-Score: N/A (insufficient repeated reads)\n";
	report << "  Scaling Sub-Score:   " << scalingScore << " / 100\n";
	if (scalingScore < 100) {
		report << "  Scaling reflects limited speed gain or timing variation. Drive limits\n"
            << "  and command overhead can contribute; this test does not determine the cause.\n";
	}

	if (std::isfinite(driftRatio) && (driftRatio > 1.3 || driftRatio < 0.7)) {
		report << "\n  ** WARNING: Baseline re-test shows "
			<< std::fixed << std::setprecision(0) << (std::abs(driftRatio - 1.0) * 100)
			<< "% thermal drift. Scores may be affected by disc heating. **\n";
	}

    if (readEvidence.HasUncorrectable()) {
        report << "\n  Uncorrectable data takes priority over the mechanical score.\n"
            << "  A clean reread cannot verify an earlier rip; verify recovered audio independently.\n";
    }

	else if (balanceScore < 75) {
		report << "\n  Recommendation:\n";
		if (balanceScore < 50) {
			if (assessment.recommendationAvailable)
				report << "    - For extraction, request " << safeSpeed << "x with Secure or Paranoid mode; see the drive speed above.\n";
			else
				report << "    - Use Secure or Paranoid extraction and verify independently; no maximum speed is established.\n";
			report << "    - Re-check previous faster rips if they lack independent\n"
				<< "      checksum verification.\n";
			report << "    - Verify against AccurateRip; the balance test itself cannot\n"
				<< "      determine whether a rip is bit-perfect.\n";
		}
		else {
			if (assessment.recommendationAvailable)
				report << "    - Request " << safeSpeed << "x with Secure mode; see the drive speed above.\n";
			else
				report << "    - Use Secure extraction and verify independently; no maximum speed is established.\n";
			report << "    - Verify faster rips with AccurateRip or another independent\n"
				<< "      checksum source; Disc Balance is not a copy-integrity test.\n";
		}
	}

    if (savedReport && hasHwC1)
        report << "\n  Detailed hardware observations and raw samples follow in the saved report.\n";
	report << std::string(60, '=') << "\n";
    std::cout << report.str();
    if (savedReport) {
        if (hasHwC1) {
            report << "\n--- Detailed hardware observations ---\n";
            Diagnostics::PrintHardwareSweepGroups(report,hardwareRows,hwSecondStageLabel);
        }
        report << "\nRaw hardware samples (one row per observation):\n";
        report << "Pass,RequestedX,DriveReportedX,Region,ScanSample,LBA,CoveredSectors,C1," << hwSecondStageLabel
            << ",CU,ElapsedMilliseconds\n";
        for (int step=0;step<NUM_SPEEDS;++step) {
            const auto& pass=hwPasses[step];
            for (size_t i=0;i<pass.observations.size();++i) {
                const auto& sample=pass.observations[i];
                report << step+1 << ',' << speeds[step] << ',';
                if (pass.ActualSpeed()>0) report << pass.ActualSpeed();
                report << ',' << (pass.startup.Contains(sample.lba)?"Startup":"Target") << ',' << i+1 << ',' << sample.lba << ',';
                if (sample.sectors>0) report << sample.sectors;
                report << ',' << sample.c1 << ',' << sample.secondStage << ',';
                if (hasLiteOnHwC1 && m_drive.LiteOnScanMeasuresCu()) report << sample.cu;
                report << ',' << pass.elapsedMs[i] << '\n';
            }
        }
        *savedReport=report.str();
    }
	return true;
}
