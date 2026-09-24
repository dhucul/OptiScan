#pragma once
#include "DiagnosticAssessment.h"
#include <numeric>
#include <ostream>

namespace Diagnostics {
struct BalanceSpeedSample {
    int requestedSpeed = 0;
    int actualSpeed = 0;
    int validReads = 0;
    double readTimeMs = 0;
    double jitterCV = 0;
    double stabilityRatio = 0;
    bool stabilityMeasured = false;
    double readErrorSignal = 0;
    double c1Rate = 0;
    double secondStageRate = 0;
    int hardwareSamples = 0;
    int hardwareActualSpeed = 0;
    bool hardwareVerified = false;
};

struct BalanceReadEvidence {
    long long hardwareCuTotal = 0;
    int pioneerUncorrectableBytes = 0;
    bool HasUncorrectable() const { return hardwareCuTotal>0 || pioneerUncorrectableBytes>0; }
};

struct BalanceAssessment {
    BalanceReadEvidence readEvidence;
    bool available = false;
    bool partial = false;
    bool usingHwEcc = false;
    bool stabilityAvailable = false;
    bool haveFullScore = false;
    bool recommendationAvailable = false;
    int score = 0, fullScore = 0;
    int errorScore = 0, fullErrorScore = 0;
    int jitterScore = 0, stabilityScore = 0, scalingScore = 0;
    int suggestedSpeed = 0, suggestedActualSpeed = 0, maximumMeasuredSpeed = 0, primaryMaximumSpeed = 0;
    int firstC2WarningSpeed = 0;
    std::vector<bool> compared, primaryCompared, speedFellBack, eccFellBack;
    std::vector<int> previousRow, timingPenalty;
};

// This is the production balance policy, shared with regression tests. Unknown
// speed rows never enter a comparison or recommendation. Known-speed failures
// still constrain coverage even if they supplied too few timings to compare.
inline BalanceAssessment AssessBalance(const std::vector<BalanceSpeedSample>& input,
    int requestedSamples, int minValidSamples, bool hasHwC1, bool hasPioneerHwC1,
    const BalanceReadEvidence& readEvidence = {}) {
    BalanceAssessment result;
    result.readEvidence = readEvidence;
    result.compared.assign(input.size(), false);
    result.primaryCompared.assign(input.size(), false);
    result.speedFellBack.assign(input.size(), false);
    result.eccFellBack.assign(input.size(), false);
    result.previousRow.assign(input.size(), -1);
    result.timingPenalty.assign(input.size(), 0);
    if (requestedSamples <= 0 || minValidSamples <= 0) return result;
    // Absolute, independently verified C2 warnings do not require a relative
    // C1 baseline or usable timing at the same request. They retain their own
    // hardware-phase speed. Pioneer E22 remains diagnostic-only.
    if (!hasPioneerHwC1) {
        for (const auto& r : input) {
            if (!r.hardwareVerified || r.hardwareActualSpeed <= 0 || r.hardwareSamples <= 0 ||
                !std::isfinite(r.secondStageRate) || r.secondStageRate <= 0.5) continue;
            if (result.firstC2WarningSpeed == 0 || r.hardwareActualSpeed < result.firstC2WarningSpeed)
                result.firstC2WarningSpeed = r.hardwareActualSpeed;
        }
    }
    std::vector<size_t> rows;
    std::set<int> distinct;
    for (size_t i=0; i<input.size(); ++i) {
        const auto& r = input[i];
        if (r.actualSpeed <= 0 || r.validReads < minValidSamples ||
            !std::isfinite(r.readTimeMs) || r.readTimeMs <= 0.001 ||
            !std::isfinite(r.jitterCV) || !std::isfinite(r.stabilityRatio) ||
            !std::isfinite(r.readErrorSignal)) continue;
        rows.push_back(i);
        distinct.insert(r.actualSpeed);
    }
    if (distinct.size() < 2) return result;
    std::stable_sort(rows.begin(), rows.end(), [&](size_t left, size_t right) {
        return input[left].actualSpeed < input[right].actualSpeed;
    });
    const int NUM_SPEEDS = static_cast<int>(rows.size());
    std::vector<int> speeds, validReadSamplesPerSpeed, hwSamplesPerSpeed;
    std::vector<double> avgReadTimeMs, jitterCoeffVar, avgStabilityRatio,
        avgReadErrorSignalPerSpeed, hwC1PerSpeed, hwSecondStagePerSpeed;
    std::vector<bool> stabilityMeasured;
    double minHardware = 1e9, maxHardware = 0;
    int validHardware = 0;
    std::set<int> hardwareSpeeds;
    for (size_t i : rows) {
        const auto& r=input[i];
        result.compared[i]=true;
        speeds.push_back(r.actualSpeed);
        validReadSamplesPerSpeed.push_back(r.validReads);
        const bool hwUsable=r.hardwareVerified && r.hardwareActualSpeed==r.actualSpeed &&
            r.hardwareSamples>0 && std::isfinite(r.c1Rate) && std::isfinite(r.secondStageRate);
        hwSamplesPerSpeed.push_back(hwUsable ? r.hardwareSamples : 0);
        avgReadTimeMs.push_back(r.readTimeMs);
        jitterCoeffVar.push_back(r.jitterCV);
        avgStabilityRatio.push_back(r.stabilityRatio);
        stabilityMeasured.push_back(r.stabilityMeasured);
        avgReadErrorSignalPerSpeed.push_back(r.readErrorSignal);
        hwC1PerSpeed.push_back(hwUsable ? r.c1Rate : 0.0);
        hwSecondStagePerSpeed.push_back(hwUsable ? r.secondStageRate : 0.0);
        if (hwUsable) {
            ++validHardware;
            hardwareSpeeds.insert(r.actualSpeed);
            const double level=r.c1Rate+(hasPioneerHwC1 ? 0 : r.secondStageRate);
            minHardware=std::min(minHardware,level);
            maxHardware=std::max(maxHardware,level);
        }
    }
    bool hwEccFlat = validHardware < 2 || hardwareSpeeds.size() < 2 ||
        maxHardware < 0.5 || maxHardware-minHardware < 0.5;
    auto coverageCapForSpeed = [&](int maximumSpeed) {
        std::vector<int> coverage;
        for (const auto& r : input)
            if (r.actualSpeed > 0 && r.actualSpeed <= maximumSpeed) coverage.push_back(r.validReads);
        return BalanceCoverageCap(coverage, requestedSamples, 0, static_cast<int>(coverage.size())-1);
    };
    int balanceScore = 0;
	const int baselineIdx = 0;
	if (hasHwC1 && hwSamplesPerSpeed[baselineIdx] == 0)
		hwEccFlat = true;

	// Audio-relevant speed ceiling for error scoring.
	// Professional CD players run at 1x; quality rippers at 4x-8x.
	// ECC errors that only appear at 24x+ are likely drive/mechanical
	// artifacts, not disc balance problems.  Cap error scoring at 16x
	// (index 2) so high-speed errors are reported but don't tank the score.
	constexpr int MAX_AUDIO_RELEVANT_SPEED = 16;
	int errorCeilingIdx = 0;
	for (int s = 0; s < NUM_SPEEDS; s++) {
		if (speeds[s] <= MAX_AUDIO_RELEVANT_SPEED) errorCeilingIdx = s;
	}
	// If the drive's floor is above the preferred range, use its lowest two
	// distinct measured speeds and report that actual range. Include all
	// repeated measurements at the ceiling, not just the first matching row.
	while (errorCeilingIdx + 1 < NUM_SPEEDS && speeds[errorCeilingIdx] == speeds[baselineIdx]) ++errorCeilingIdx;
	while (errorCeilingIdx + 1 < NUM_SPEEDS && speeds[errorCeilingIdx+1] == speeds[errorCeilingIdx]) ++errorCeilingIdx;

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
		if (ceilIdx <= baselineIdx)
			return coverageCapForSpeed(speeds[ceilIdx]);
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
		const int coverageCap = coverageCapForSpeed(speeds[ceilIdx]);
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

	const int ceilingIdx = NUM_SPEEDS - 1;

	// Speed scaling score: detect when the drive fails to go faster at higher
	// speed settings.  Wobble causes the servo to struggle, so the drive
	// plateaus or even regresses — read times stop decreasing or increase.
	// Compare adjacent speed pairs between baseline and ceiling only.
	int scalingScore = 100;
	int scalingPenalties = 0;
	std::vector<int> timingPenalty(NUM_SPEEDS, 0);
	for (int s = baselineIdx + 1; s <= ceilingIdx; s++) {
		int prev = (s == baselineIdx + 1) ? baselineIdx : s - 1;
		if (avgReadTimeMs[s] < 0.001 || avgReadTimeMs[prev] < 0.001) continue;

		double timeRatio = avgReadTimeMs[s] / avgReadTimeMs[prev];

		if (speeds[s] == speeds[prev] && timeRatio <= 1.15) continue;
		if (timeRatio >= 1.0) {
			// Regression: higher speed is actually slower (or equal)
			scalingPenalties += 2;
			timingPenalty[s] = 2;
		}
		else if (timeRatio > 0.95) {
			// Plateau: < 5% improvement despite a speed step increase
			scalingPenalties += 1;
			timingPenalty[s] = 1;
		}
	}

	// After collecting avgReadTimeMs[], detect if the drive throttled.
	// If the requested speed doubled but read time barely changed, the drive
	// refused to go faster — a strong wobble indicator.

	const double actualBaselineSpeed = speeds[baselineIdx];

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
	int safeSpeedIdx = result.firstC2WarningSpeed > 0 && speeds[baselineIdx] >= result.firstC2WarningSpeed
		? -1 : baselineIdx;
	for (int s = baselineIdx + 1; safeSpeedIdx >= 0 && s < NUM_SPEEDS; s++) {
		if (result.firstC2WarningSpeed > 0 && speeds[s] >= result.firstC2WarningSpeed) break;
		if (validReadSamplesPerSpeed[s] < minValidSamples) break;
		if (speedFellBack[s] || eccFellBack[s]) break;

		// Check for timing regression or plateau
		int prev = (s == baselineIdx + 1) ? baselineIdx : s - 1;
		if (avgReadTimeMs[s] > 0.001 && avgReadTimeMs[prev] > 0.001) {
			double timeRatio = avgReadTimeMs[s] / avgReadTimeMs[prev];
			if (speeds[s] > speeds[prev] ? timeRatio >= 0.95 : timeRatio > 1.15) break;

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
		}

		safeSpeedIdx = s;
	}
	int safeSpeed = safeSpeedIdx >= 0
		? std::min(speeds[safeSpeedIdx], input[rows[safeSpeedIdx]].requestedSpeed) : 0;

    result.available = true;
    result.partial = rows.size() != input.size();
    result.usingHwEcc = usingHwEcc;
    result.stabilityAvailable = stabilityAvailable;
    result.maximumMeasuredSpeed = speeds.back();
    result.primaryMaximumSpeed = speeds[errorCeilingIdx];
    result.haveFullScore = speeds.back() > result.primaryMaximumSpeed;
    result.score = balanceScore;
    result.fullScore = balanceScoreFull;
    result.errorScore = errorScore;
    result.fullErrorScore = errorScoreFull;
    result.jitterScore = jitterScore;
    result.stabilityScore = stabilityScore;
    result.scalingScore = scalingScore;
    result.suggestedSpeed = safeSpeed;
    result.suggestedActualSpeed = safeSpeedIdx>=0 ? speeds[safeSpeedIdx] : 0;
    result.recommendationAvailable = safeSpeed > 0;
    // Positive uncorrectable observations survive partial/unrated passes.
    // Keep the mechanical score, but do not recommend an extraction speed.
    if (readEvidence.HasUncorrectable()) {
        result.recommendationAvailable = false;
        result.suggestedSpeed = result.suggestedActualSpeed = 0;
    }
    for (size_t i=0; i<rows.size(); ++i) {
        result.primaryCompared[rows[i]] = i <= static_cast<size_t>(errorCeilingIdx);
        result.speedFellBack[rows[i]] = speedFellBack[i];
        result.eccFellBack[rows[i]] = eccFellBack[i];
        result.timingPenalty[rows[i]] = timingPenalty[i];
        if (i > 0) result.previousRow[rows[i]] = static_cast<int>(rows[i-1]);
    }
    return result;
}
inline std::string BalanceExtractionGuidance(const BalanceAssessment& assessment) {
    if (assessment.readEvidence.HasUncorrectable())
        return "Uncorrectable data observed - use recovery and verify independently";
    if (assessment.firstC2WarningSpeed>0)
        return "Caution - C2 warning at ~"+std::to_string(assessment.firstC2WarningSpeed)+"x";
    return assessment.score>=75 ? "No additional warning from mechanical score; verify the rip"
        : "Caution - use the suggested setting and verify the rip";
}

inline void PrintBalanceRipRecommendation(std::ostream& out,const BalanceAssessment& assessment) {
    const auto flags=out.flags();
    out<<std::dec;
    if (assessment.readEvidence.HasUncorrectable()) {
        out<<"  Suggested rip setting: NOT ESTABLISHED - uncorrectable data observed.\n"
            <<"  Use Secure/Paranoid recovery and independently verify the recovered audio.\n";
        if (assessment.readEvidence.hardwareCuTotal>0)
            out<<"  Hardware CU observed: "<<assessment.readEvidence.hardwareCuTotal<<".\n";
        if (assessment.readEvidence.pioneerUncorrectableBytes>0)
            out<<"  Pioneer CD Check uncorrectable bytes: "<<assessment.readEvidence.pioneerUncorrectableBytes
                <<" (worst observed window).\n";
        out.flags(flags);
        return;
    }
    if (assessment.recommendationAvailable)
        out<<"  Suggested rip setting: request "<<assessment.suggestedSpeed
            <<"x (drive-reported speed ~"<<assessment.suggestedActualSpeed<<"x).\n"
            <<"  Lower requested settings may run at the same drive speed.\n";
    else out<<"  Suggested rip setting: NOT ESTABLISHED - no lower measured speed passed the C2 warning limit.\n";
    if (assessment.firstC2WarningSpeed>0)
        out<<"  Hardware C2 warning at ~"<<assessment.firstC2WarningSpeed
            <<"x; this limits the recommendation independently of the mechanical score.\n";
    out.flags(flags);
}
} // namespace Diagnostics
