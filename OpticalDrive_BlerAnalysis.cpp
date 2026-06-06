#define NOMINMAX
#include "OpticalDrive.h"
#include <algorithm>
#include <cmath>

// ============================================================================
// BLER Analysis - Error Pattern Detection and Quality Rating
// ============================================================================

void OpticalDrive::AnalyzeBlerResults(BlerResult& result, const std::vector<DWORD>& errorLBAs, int scanSpeed) {
	// Calculate averages
	result.avgC2PerSecond = result.totalSeconds > 0
		? static_cast<double>(result.totalC2Errors) / result.totalSeconds : 0;

	if (result.hasC1Data) {
		result.avgC1PerSecond = result.totalSeconds > 0
			? static_cast<double>(result.totalC1Errors) / result.totalSeconds : 0;
	}

	// Find max C2 per second
	for (size_t i = 0; i < result.perSecondC2.size(); i++) {
		if (result.perSecondC2[i].second > result.maxC2PerSecond) {
			result.maxC2PerSecond = result.perSecondC2[i].second;
			result.worstSecondLBA = result.perSecondC2[i].first;
		}
	}

	// Find max C1 per second
	if (result.hasC1Data) {
		for (size_t i = 0; i < result.perSecondC1.size(); i++) {
			if (result.perSecondC1[i].second > result.maxC1PerSecond) {
				result.maxC1PerSecond = result.perSecondC1[i].second;
				result.worstC1SecondLBA = static_cast<DWORD>(result.perSecondC1[i].first);
			}
		}

		// C2 proximity: express C1 rate as a fraction of the Red Book 220/sec limit
		result.c1UtilizationPct = std::min(100.0, result.avgC1PerSecond / 220.0 * 100.0);
		result.peakC1UtilizationPct = std::min(100.0, result.maxC1PerSecond / 220.0 * 100.0);
		double util = result.avgC1PerSecond / 220.0;
		result.c2MarginScore = static_cast<int>(
			std::lround(std::max(0.0, (1.0 - util) * 100.0)));

		// Override: if C2 errors already exist, the margin is exhausted
		if (result.totalC2Sectors > 0) {
			result.c2MarginScore = 0;
			result.c2MarginLabel = "EXHAUSTED";
		}
		else if (result.avgC1PerSecond < 50.0)  result.c2MarginLabel = "WIDE";
		else if (result.avgC1PerSecond < 150.0) result.c2MarginLabel = "ADEQUATE";
		else if (result.avgC1PerSecond < 220.0) result.c2MarginLabel = "NARROW";
		else                                     result.c2MarginLabel = "CRITICAL";
	}

	// Build error clusters using adaptive tolerance
	if (!errorLBAs.empty()) {
		std::vector<DWORD> sortedLBAs = errorLBAs;
		std::sort(sortedLBAs.begin(), sortedLBAs.end());

		DetectErrorClusters(sortedLBAs, result.errorClusters, scanSpeed);

		// Largest cluster size
		for (const auto& c : result.errorClusters) {
			if (c.size() > result.largestClusterSize)
				result.largestClusterSize = c.size();
		}

		// Edge concentration — use rate-based comparison
		double innerRate = result.zoneStats.InnerErrorRate();
		double middleRate = result.zoneStats.MiddleErrorRate();
		double outerRate = result.zoneStats.OuterErrorRate();
		result.hasEdgeConcentration =
			(outerRate > innerRate * 2.0 && outerRate > 1.0) ||
			(innerRate > outerRate * 2.0 && innerRate > 1.0);

		// Progressive pattern — require monotonic increase
		result.hasProgressivePattern =
			innerRate < middleRate &&
			middleRate < outerRate &&
			outerRate > 0.5;
	}

	// Quality rating
	if (result.totalReadFailures > 0) {
		result.qualityRating = "BAD";
	}
	else if (result.totalC2Sectors == 0) {
		if (!result.hasC1Data || result.avgC1PerSecond < 50.0)
			result.qualityRating = "EXCELLENT";
		else if (result.avgC1PerSecond < 150.0)
			result.qualityRating = "GOOD";
		else if (result.avgC1PerSecond < 220.0)
			result.qualityRating = "ACCEPTABLE";
		else
			result.qualityRating = "FAIR";
	}
	else {
		if (result.avgC2PerSecond < 1.0 && result.consecutiveErrorSectors < 3
			&& result.maxC2InSingleSector < 50) result.qualityRating = "GOOD";
		else if (result.avgC2PerSecond < 10.0 && result.consecutiveErrorSectors < 10
			&& result.maxC2InSingleSector < 100) result.qualityRating = "ACCEPTABLE";
		else if (result.avgC2PerSecond < 50.0) result.qualityRating = "FAIR";
		else result.qualityRating = "POOR";
	}
}