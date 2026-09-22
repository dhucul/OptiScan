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

		// Sustained-level statistics, from the same helper the quality scan,
		// disc rot and hardware paths use. Keep the sustained diagnostic
		// separate from the observed average and raw maximum.
		{
			std::vector<int> c1Series, e22Series;
			std::vector<DWORD> sampleLbas;
			c1Series.reserve(result.perSecondC1.size());
			for (const auto& p : result.perSecondC1) {
				c1Series.push_back(p.second); sampleLbas.push_back(p.first);
			}
			for (const auto& p : result.perSecondPioneerE22) e22Series.push_back(p.second);
			ComputeScanPeakContext(c1Series, e22Series, scanSpeed, result.peaks, sampleLbas);
		}

		// The same observed-rate bands and persistence rule as Q-Check.
		result.sustainedC1Rating = RateSustainedC1(result.peaks);
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
		ScanQuality::RateC1(result.avgC1PerSecond,
			result.hasC1Data && !result.perSecondC1.empty()), result.qualityRating);

}
