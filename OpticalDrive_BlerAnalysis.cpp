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

	result.c1 = ScanQuality::SummarizeC1(result.c1Samples, result.hasC1Data);
	result.avgC1PerSecond = result.c1.average;
	result.peaks.scanSpeedX = scanSpeed;
	result.peaks.sustainedMeasurable = result.c1.fullSeconds.persistenceMeasurable;
	result.peaks.sustainedC1PerSecond = result.c1.fullSeconds.sustainedPeak;
	result.peaks.p95C1PerSecond = result.c1.fullSeconds.p95;
	result.sustainedC1Rating = RateSustainedC1(result.peaks);
	// C2 observations keep their separate decoder/sector interpretation.
	for (const auto& sample : result.perSecondC2) {
		if (sample.second > result.maxC2PerSecond) {
			result.maxC2PerSecond = sample.second;
			result.worstSecondLBA = sample.first;
		}
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
		result.qualityRating = "EXCELLENT"; // read evidence only; apply C1 below
	}
	else {
		if (result.avgC2PerSecond < 1.0 && result.consecutiveErrorSectors < 3
			&& result.maxC2InSingleSector < 50) result.qualityRating = "GOOD";
		else if (result.avgC2PerSecond < 10.0 && result.consecutiveErrorSectors < 10
			&& result.maxC2InSingleSector < 100) result.qualityRating = "ACCEPTABLE";
		else if (result.avgC2PerSecond < 50.0) result.qualityRating = "FAIR";
		else result.qualityRating = "POOR";
	}
	result.qualityRating = ScanQuality::CombineC1Quality(
		result.c1.Rating(), result.qualityRating);

}
