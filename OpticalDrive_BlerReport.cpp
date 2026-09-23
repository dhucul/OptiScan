#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleGraph.h"
#include "ScanMeasurementReporting.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>

// ============================================================================
// BLER Reporting - Text Report Generation
// ============================================================================

void OpticalDrive::PrintBlerReport(const DiscInfo& disc, const BlerResult& result) {
	bool hasC1Support = result.hasC1Data;

	std::cout << "\n" << std::string(60, '=') << "\n";
	if (hasC1Support)
		std::cout << "              BLER QUALITY SCAN REPORT\n";
	else
		std::cout << "              DISC INTEGRITY REPORT (C2)\n";
	std::cout << std::string(60, '=') << "\n";

	std::cout << "\n--- Scan Configuration ---\n";
	std::cout << "  Requested sectors: " << result.totalSectors << "\n";
	std::cout << "  Disc length:     "
		<< (result.totalSeconds / 60) << ":" << std::setfill('0') << std::setw(2) << (result.totalSeconds % 60)
		<< std::setfill(' ') << " (mm:ss)\n";
	std::cout << "  C1 reporting:    " << result.C1MeasurementLabel() << "\n";
	if (!result.measurementMethod.empty())
		std::cout << "  Method:          " << result.measurementMethod << "\n";

	std::cout << "\n--- C1 Observations ---\n";
	PrintMethodC1Summary(std::cout, result);
	if (hasC1Support) ScanQuality::PrintC1Policy(std::cout);
	ScanQuality::PrintConfidenceCaveat(std::cout, result.peaks.PeakConfidence());

	if (!result.CanAssessC2()) {
		std::cout << "\n--- C2 Measurement ---\n";
		PrintUnverifiedC2Summary(std::cout, result);
		if (result.pioneerVendorQuality) {
			std::cout << "\n--- Pioneer E22 Diagnostic (not C2) ---\n";
			std::cout << "  Total E22:        " << result.pioneerE22Total << "\n";
			ScanQuality::PrintCounterSummary(std::cout, "E22", result.pioneerE22Observations);
			if (result.pioneerE22Observations.RateAvailable() && result.peaks.sustainedMeasurable)
				std::cout << "  Sustained E22/sec: " << result.peaks.sustainedPioneerE22PerSecond << "\n";
			else std::cout << "  Sustained E22: unavailable\n";
			std::cout << "  Rating:           " << result.pioneerE22Rating
				<< " (" << PioneerE22RatingDescription(result.pioneerE22Rating)
				<< "; diagnostic only)\n";
			if (result.pioneerE22Observations.RateAvailable() &&
				std::all_of(result.c1Samples.begin(), result.c1Samples.end(),
					[](const ScanQuality::C1Interval& s) { return s.sectors == 75; }) &&
				ScanQuality::TransientNoteWarranted(result.pioneerE22Peak,
					result.peaks.peakPioneerE22Transient,
					ScanQuality::kMinE22PeakWorthExplaining)) {
				ScanQuality::SeriesStats shown;
				shown.peak = result.pioneerE22Peak;
				shown.peakRunLength = result.peaks.peakPioneerE22RunLength;
				shown.sustainedPeak = result.peaks.sustainedPioneerE22PerSecond;
				ScanQuality::PrintWrapped(std::cout,
					ScanQuality::TransientNote("E22", shown), "  ");
			}
			if (result.peaks.pioneerE22PeakTracksC1)
				ScanQuality::PrintWrapped(std::cout,
					"E22 and C1 peak at the same time slice - one event counted by "
					"two decoder stages, not two independent findings.", "  ");
			std::cout << "  Uncorrectable:    ";
			if (!result.pioneerCdCheckRun)
				std::cout << "NOT MEASURED\n";
			else if (result.pioneerCdCheckC2Bytes > 0)
				std::cout << "YES - " << result.pioneerCdCheckC2Bytes
					<< " C2-uncorrectable byte(s), worst window; DATA LOSS\n";
			else
				std::cout << "NO - completed Pioneer CD Check\n";
		}
		PrintBlerGraph(result);
		std::cout << "\n" << std::string(60, '=') << "\n";
		return;
	}

	std::cout << "\n--- C2 Error Statistics ---\n";
	std::cout << "  Avg C2/sec:       " << std::fixed << std::setprecision(2) << result.avgC2PerSecond;
	if (result.avgC2PerSecond == 0.0)
		std::cout << "  [PASS]  (no uncorrectable errors)\n";
	else if (result.avgC2PerSecond < 1.0)
		std::cout << "  [WARN]  (minor uncorrectable errors)\n";
	else
		std::cout << "  [FAIL]  (significant uncorrectable errors)\n";
	std::cout << "  Max C2/sec:       " << result.maxC2PerSecond;
	if (result.maxC2PerSecond > 0) {
		int worstMin = (result.worstSecondLBA / 75) / 60;
		int worstSec = (result.worstSecondLBA / 75) % 60;
		std::cout << "  at " << worstMin << ":" << std::setfill('0') << std::setw(2) << worstSec << std::setfill(' ');
	}
	std::cout << "\n";

	std::cout << "\n--- Error Statistics ---\n";
	std::cout << "  Total C2 errors:      " << result.totalC2Errors << "\n";
	std::cout << "  Sectors with C2:      " << result.totalC2Sectors;
	if (result.totalSectors > 0)
		std::cout << " (" << std::fixed << std::setprecision(3)
		<< (result.totalC2Sectors * 100.0 / result.totalSectors) << "%)";
	std::cout << "\n";
	std::cout << "  Read failures:        " << result.totalReadFailures << "\n";
	std::cout << "  Recovered errors:     " << result.recoveredC2Sectors << " sectors, "
		<< result.recoveredC2Errors << " C2 errors (drive-corrected)\n";
	std::cout << "  Max C2 in one sector: " << result.maxC2InSingleSector;
	if (result.maxC2InSingleSector > 0) std::cout << "  (LBA " << result.worstSectorLBA << ")";
	std::cout << "\n";
	std::cout << "  Longest error run:    " << result.consecutiveErrorSectors << " sectors\n";

	PrintBlerZoneStats(result);
	PrintBlerClusters(result);
	PrintBlerWorstSectors(result);
	PrintBlerDensityDistribution(result);
	PrintBlerPerTrackSummary(disc, result);
	PrintBlerGraph(result);
	PrintBlerMarginAnalysis(result);
	PrintBlerQualitySummary(result);
}

void OpticalDrive::PrintBlerZoneStats(const BlerResult& result) {
	std::cout << "\n--- Zone Error Distribution ---\n";
	std::cout << "  Inner  (0-33%):   " << std::fixed << std::setprecision(2)
		<< result.zoneStats.InnerErrorRate() << "% ("
		<< result.zoneStats.innerErrors << "/" << result.zoneStats.innerSectors << ")\n";
	std::cout << "  Middle (33-66%):  " << std::fixed << std::setprecision(2)
		<< result.zoneStats.MiddleErrorRate() << "% ("
		<< result.zoneStats.middleErrors << "/" << result.zoneStats.middleSectors << ")\n";
	std::cout << "  Outer  (66-100%): " << std::fixed << std::setprecision(2)
		<< result.zoneStats.OuterErrorRate() << "% ("
		<< result.zoneStats.outerErrors << "/" << result.zoneStats.outerSectors << ")\n";
	if (result.hasEdgeConcentration)
		std::cout << "  ** Edge concentration detected **\n";
	if (result.hasProgressivePattern)
		std::cout << "  ** Progressive degradation pattern detected **\n";
}

void OpticalDrive::PrintBlerClusters(const BlerResult& result) {
	if (result.errorClusters.empty()) return;

	std::cout << "\n--- Error Clusters ---\n";
	std::cout << "  Cluster count:    " << result.errorClusters.size() << "\n";
	std::cout << "  Largest cluster:  " << result.largestClusterSize << " sectors\n";

	auto sortedClusters = result.errorClusters;
	std::sort(sortedClusters.begin(), sortedClusters.end(),
		[](const ErrorCluster& a, const ErrorCluster& b) { return a.size() > b.size(); });
	int clusterShow = std::min(10, static_cast<int>(sortedClusters.size()));
	std::cout << "\n  Top " << clusterShow << " cluster" << (clusterShow > 1 ? "s" : "") << ":\n";
	std::cout << "  #    Start LBA    End LBA     Size   Position\n";
	std::cout << "  " << std::string(50, '-') << "\n";
	for (int i = 0; i < clusterShow; i++) {
		const auto& c = sortedClusters[i];
		int posMin = (c.startLBA / 75) / 60;
		int posSec = (c.startLBA / 75) % 60;
		std::cout << "  " << std::setw(2) << (i + 1) << "   "
			<< std::setw(10) << c.startLBA << "   "
			<< std::setw(10) << c.endLBA << "   "
			<< std::setw(4) << c.size() << "   "
			<< posMin << ":" << std::setfill('0') << std::setw(2) << posSec
			<< std::setfill(' ') << "\n";
	}
}

void OpticalDrive::PrintBlerWorstSectors(const BlerResult& result) {
	if (result.topWorstC2Sectors.empty()) return;

	std::cout << "\n--- Top " << result.topWorstC2Sectors.size() << " Worst C2 Sectors ---\n";
	if (result.totalReadFailures > 0)
		std::cout << "  Note: " << result.totalReadFailures
		<< " read-failure sector(s) are not listed here (no C2 count available).\n";
	std::cout << "  #    LBA         C2 Count   Position\n";
	std::cout << "  " << std::string(44, '-') << "\n";
	for (size_t i = 0; i < result.topWorstC2Sectors.size(); i++) {
		auto [lba, cnt] = result.topWorstC2Sectors[i];
		int posMin = (lba / 75) / 60;
		int posSec = (lba / 75) % 60;
		std::cout << "  " << std::setw(2) << (i + 1) << "   "
			<< std::setw(9) << lba << "   "
			<< std::setw(8) << cnt << "   "
			<< posMin << ":" << std::setfill('0') << std::setw(2) << posSec
			<< std::setfill(' ') << "\n";
	}
}

void OpticalDrive::PrintBlerDensityDistribution(const BlerResult& result) {
	std::cout << "\n--- C2 Error Density Distribution ---\n";
	int tier0 = 0, tier1 = 0, tier2 = 0, tier3 = 0, tier4 = 0, tier5 = 0;
	int totalSec = static_cast<int>(result.totalSeconds);
	for (size_t i = 0; i < result.totalSeconds; i++) {
		int v = result.perSecondC2[i].second;
		if (v == 0)        tier0++;
		else if (v <= 5)   tier1++;
		else if (v <= 20)  tier2++;
		else if (v <= 50)  tier3++;
		else if (v <= 100) tier4++;
		else               tier5++;
	}
	auto pct = [&](int n) -> double {
		return totalSec > 0 ? n * 100.0 / totalSec : 0.0;
		};
	std::cout << std::fixed << std::setprecision(1);
	std::cout << "  0 errors:      " << std::setw(5) << tier0 << " sec  (" << pct(tier0) << "%)\n";
	std::cout << "  1-5 errors:    " << std::setw(5) << tier1 << " sec  (" << pct(tier1) << "%)\n";
	std::cout << "  6-20 errors:   " << std::setw(5) << tier2 << " sec  (" << pct(tier2) << "%)\n";
	std::cout << "  21-50 errors:  " << std::setw(5) << tier3 << " sec  (" << pct(tier3) << "%)\n";
	std::cout << "  51-100 errors: " << std::setw(5) << tier4 << " sec  (" << pct(tier4) << "%)\n";
	std::cout << "  100+ errors:   " << std::setw(5) << tier5 << " sec  (" << pct(tier5) << "%)\n";
}

void OpticalDrive::PrintBlerPerTrackSummary(const DiscInfo& disc, const BlerResult& result) {
	bool hasC1Support = result.hasC1Data;

	std::cout << "\n--- Per-Track Summary ---\n";
	if (hasC1Support)
		std::cout << "  Track  Length     C1 Errors  C2 Errors  Err Secs  Avg C1/s  Avg C2/s  Status\n";
	else
		std::cout << "  Track  Length     C2 Errors  Err Secs  Avg/sec   Status\n";
	std::cout << "  " << std::string(hasC1Support ? 78 : 58, '-') << "\n";

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD tStart = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		DWORD tEnd = t.endLBA;
		DWORD tSectors = tEnd - tStart + 1;
		DWORD tSeconds = (tSectors + 74) / 75;

		int trackC2 = 0, trackC2Seconds = 0;
		std::vector<ScanQuality::C1Interval> trackC1Samples;
		for (size_t i = 0; i < result.perSecondC2.size(); i++) {
			DWORD secLBA = static_cast<DWORD>(result.perSecondC2[i].first);
			if (secLBA >= tStart && secLBA <= tEnd) {
				if (result.perSecondC2[i].second > 0) {
					trackC2 += result.perSecondC2[i].second;
					trackC2Seconds++;
				}

			}
		}

		for (const auto& sample : result.c1Samples) {
			// An aggregate interval straddling a track boundary cannot be split
			// into invented counts. Attribute only wholly contained intervals.
			if (sample.lba >= tStart && sample.lba <= tEnd &&
				(sample.sectors == 0 || std::uint64_t{sample.lba} + sample.sectors <= std::uint64_t{tEnd} + 1))
				trackC1Samples.push_back(sample);
		}
		const auto trackC1 = ScanQuality::SummarizeC1(trackC1Samples, result.c1.verified);

		double trackAvgC2 = tSeconds > 0 ? static_cast<double>(trackC2) / tSeconds : 0;
		double trackAvgC1 = trackC1.average;
		int trackMin = tSeconds / 60;
		int trackSec = tSeconds % 60;

		std::string status = "EXCELLENT";
		double errorSecPct = tSeconds > 0 ? (trackC2Seconds * 100.0 / tSeconds) : 0;
		if (errorSecPct > 20.0)
			status = "BAD";
		else if (trackC2 > 100 || trackC2Seconds > 10)
			status = "POOR";
		else if (trackC2 > 20 || trackC2Seconds > 3)
			status = "FAIR";
		else if (trackC2 > 0)
			status = "GOOD";
		status = ScanQuality::CombineC1Quality(
			trackC1.Rating(), status);

		if (hasC1Support) {
			std::cout << "  " << std::setw(3) << t.trackNumber << "    "
				<< trackMin << ":" << std::setfill('0') << std::setw(2) << trackSec << std::setfill(' ') << "   "
				<< std::setw(7) << trackC1.total << "     "
				<< std::setw(7) << trackC2 << "     "
				<< std::setw(4) << trackC2Seconds << "     "
				<< std::setw(6) << (trackC1.RateAvailable() ? std::to_string(trackAvgC1) : "N/A") << "    "
				<< std::fixed << std::setprecision(1) << std::setw(6) << trackAvgC2 << "  "
				<< status << "\n";
			if (hasC1Support) {
				if (trackC1.timingKnown)
					std::cout << "         C1 measured audio: " << trackC1.MeasuredSeconds() << " sec; coverage "
						<< (tSectors > 0 ? trackC1.measuredSectors * 100.0 / tSectors : 0.0) << "%\n";
				else std::cout << "         C1 measured audio / coverage: unavailable\n";
			}
		}
		else {
			std::cout << "  " << std::setw(3) << t.trackNumber << "    "
				<< trackMin << ":" << std::setfill('0') << std::setw(2) << trackSec << std::setfill(' ') << "   "
				<< std::setw(7) << trackC2 << "     "
				<< std::setw(4) << trackC2Seconds << "     "
				<< std::fixed << std::setprecision(1) << std::setw(6) << trackAvgC2 << "  "
				<< status << "\n";
		}
	}
}

void OpticalDrive::PrintBlerMarginAnalysis(const BlerResult& result) {
	std::cout << "\n--- C1 Measurement Limits ---\n";
	if (!result.hasC1Data)
		std::cout << "  C1 was not measured; no C1 quality rating is available.\n";
	std::cout << "  C1 counts do not measure remaining C2 correction capacity.\n"
		<< "  Copy integrity requires independent read/verification evidence.\n";
}

void OpticalDrive::PrintBlerQualitySummary(const BlerResult& result) {
	std::cout << "\n" << std::string(60, '-') << "\n";
	std::cout << "  QUALITY: " << result.qualityRating << "\n";

	if (result.totalReadFailures > 0)
		std::cout << "  Read failures detected; some data may be unrecoverable.\n";
	else if (!result.CanAssessC2())
		std::cout << "  C2 was not verified; a zero count does not establish copy integrity.\n";
	else if (result.totalC2Sectors > 0)
		std::cout << "  C2 activity detected. Use secure extraction and verify the result.\n";
	else
		std::cout << "  No C2 activity observed in this pass.\n";
	if (result.hasC1Data)
		std::cout << "  C1 labels describe observed rates using OptiScan's shared bands.\n";
	else
		std::cout << "  C1 quality was not measured.\n";
	std::cout << std::string(60, '=') << "\n";
}
