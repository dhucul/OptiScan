#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleGraph.h"
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
	std::cout << "  Sectors scanned: " << result.totalSectors << "\n";
	std::cout << "  Disc length:     "
		<< (result.totalSeconds / 60) << ":" << std::setfill('0') << std::setw(2) << (result.totalSeconds % 60)
		<< std::setfill(' ') << " (mm:ss)\n";
	if (hasC1Support)
		std::cout << "  C1 reporting:    Yes (block error bytes 294-295)\n";
	else
		std::cout << "  C1 reporting:    No (drive does not support C1 stats)\n";
	if (!result.measurementMethod.empty())
		std::cout << "  Method:          " << result.measurementMethod << "\n";

	// C1 Error Report
	if (hasC1Support) {
		std::cout << "\n--- C1 Error Statistics ---\n";

		bool c1BlerPass = result.avgC1PerSecond < 220.0;
		std::cout << "  Total C1 errors:      " << result.totalC1Errors << "\n";
		std::cout << "  Sectors with C1:      " << result.totalC1Sectors;
		if (result.totalSectors > 0)
			std::cout << " (" << std::fixed << std::setprecision(3)
			<< (result.totalC1Sectors * 100.0 / result.totalSectors) << "%)";
		std::cout << "\n";
		std::cout << "  Avg C1/sec:           " << std::fixed << std::setprecision(2) << result.avgC1PerSecond;
		std::cout << (c1BlerPass ? "  [PASS]" : "  [FAIL]") << "  (Red Book limit: 220/sec)\n";
		std::cout << "  Max C1/sec:           " << result.maxC1PerSecond;
		if (result.maxC1PerSecond > 0) {
			int worstMin = (result.worstC1SecondLBA / 75) / 60;
			int worstSec = (result.worstC1SecondLBA / 75) % 60;
			std::cout << "  at " << worstMin << ":" << std::setfill('0') << std::setw(2) << worstSec << std::setfill(' ');
		}
		std::cout << "\n";
		std::cout << "  Max C1 in one sector: " << result.maxC1InSingleSector;
		if (result.maxC1InSingleSector > 0) std::cout << "  (LBA " << result.worstC1SectorLBA << ")";
		std::cout << "\n";

		if (result.avgC1PerSecond < 5.0)
			std::cout << "  C1 Assessment:        EXCELLENT - minimal correction needed\n";
		else if (result.avgC1PerSecond < 50.0)
			std::cout << "  C1 Assessment:        GOOD - normal wear\n";
		else if (result.avgC1PerSecond < 220.0)
			std::cout << "  C1 Assessment:        FAIR - elevated but within Red Book limits\n";
		else
			std::cout << "  C1 Assessment:        POOR - exceeds Red Book BLER limit\n";
	}

	if (result.c2Unverified) {
		std::cout << "\n--- C2 Measurement ---\n";
		std::cout << "  Status:           NOT VERIFIED / NOT MEASURED\n";
		std::cout << "  A zero C2 total is not a clean result because this drive did not\n"
			<< "  provide a trustworthy C2 error-pointer measurement.\n";
		if (result.pioneerVendorQuality) {
			std::cout << "\n--- Pioneer E22 Diagnostic (not C2) ---\n";
			std::cout << "  Total E22:        " << result.pioneerE22Total << "\n";
			std::cout << "  Avg E22/sec:      " << std::fixed << std::setprecision(2)
				<< result.pioneerE22AvgPerSecond << "\n";
			std::cout << "  Peak E22/sec:     " << result.pioneerE22Peak << "\n";
			std::cout << "  Rating:           " << result.pioneerE22Rating << " (diagnostic only)\n";
			std::cout << "  Uncorrectable:    ";
			if (!result.pioneerCdCheckRun)
				std::cout << "NOT MEASURED\n";
			else if (result.pioneerCdCheckC2Bytes > 0)
				std::cout << "YES - " << result.pioneerCdCheckC2Bytes
					<< " C2-uncorrectable byte(s), worst window; DATA LOSS\n";
			else
				std::cout << "NO - completed Pioneer CD Check\n";
		}
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

		int trackC1 = 0, trackC2 = 0, trackC2Seconds = 0;
		for (size_t i = 0; i < result.perSecondC2.size(); i++) {
			DWORD secLBA = static_cast<DWORD>(result.perSecondC2[i].first);
			if (secLBA >= tStart && secLBA <= tEnd) {
				if (result.perSecondC2[i].second > 0) {
					trackC2 += result.perSecondC2[i].second;
					trackC2Seconds++;
				}
				if (hasC1Support && i < result.perSecondC1.size()
					&& result.perSecondC1[i].second > 0) {
					trackC1 += result.perSecondC1[i].second;
				}
			}
		}

		double trackAvgC2 = tSeconds > 0 ? static_cast<double>(trackC2) / tSeconds : 0;
		double trackAvgC1 = tSeconds > 0 ? static_cast<double>(trackC1) / tSeconds : 0;
		int trackMin = tSeconds / 60;
		int trackSec = tSeconds % 60;

		const char* status = "Perfect";
		double errorSecPct = tSeconds > 0 ? (trackC2Seconds * 100.0 / tSeconds) : 0;
		if (errorSecPct > 20.0)
			status = "BAD";
		else if (trackC2 > 100 || trackC2Seconds > 10)
			status = "Poor";
		else if (trackC2 > 20 || trackC2Seconds > 3)
			status = "Fair";
		else if (trackC2 > 0)
			status = "Good";
		else if (hasC1Support && trackC1 > 1000)
			status = "Fair";

		if (hasC1Support) {
			std::cout << "  " << std::setw(3) << t.trackNumber << "    "
				<< trackMin << ":" << std::setfill('0') << std::setw(2) << trackSec << std::setfill(' ') << "   "
				<< std::setw(7) << trackC1 << "     "
				<< std::setw(7) << trackC2 << "     "
				<< std::setw(4) << trackC2Seconds << "     "
				<< std::fixed << std::setprecision(1) << std::setw(6) << trackAvgC1 << "    "
				<< std::fixed << std::setprecision(1) << std::setw(6) << trackAvgC2 << "  "
				<< status << "\n";
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
	std::cout << "\n--- C2 Error Proximity ---\n";
	if (result.hasC1Data) {
		std::cout << "  Avg C1 budget used:  " << std::fixed << std::setprecision(1)
			<< result.avgC1PerSecond << " / 220.0 /sec  ("
			<< result.c1UtilizationPct << "% of Red Book limit)\n";
		std::cout << "  Peak C1 budget used: " << result.maxC1PerSecond << " / 220.0 /sec  ("
			<< std::setprecision(1) << result.peakC1UtilizationPct << "% of limit)\n";
		std::cout << "  C2 margin score:     " << result.c2MarginScore
			<< " / 100  [" << result.c2MarginLabel << "]\n";

		// Headroom: fill = score/100, severity inverted (low score = red).
		double scoreFrac = result.c2MarginScore / 100.0;
		double severity = std::min(1.0, std::max(0.0, 1.0 - scoreFrac));
		std::ostringstream suffix;
		suffix << result.c2MarginScore << "%  [" << result.c2MarginLabel << "]";
		Console::DrawScoreBar("Headroom:", scoreFrac, severity, 38, suffix.str());

		if (result.c2MarginLabel == "EXHAUSTED")
			std::cout << "  !! C2 errors already present - correction margin is exceeded.\n";
		else if (result.c2MarginLabel == "WIDE")
			std::cout << "  Strong correction headroom - disc is healthy.\n";
		else if (result.c2MarginLabel == "ADEQUATE")
			std::cout << "  Normal wear. Comfortable headroom for reliable ripping.\n";
		else if (result.c2MarginLabel == "NARROW")
			std::cout << "  Elevated C1 rate. Recommend ripping at 4-8x.\n";
		else
			std::cout << "  !! C1 near Red Book limit. C2 errors likely on re-reads or at higher speed.\n";
	}
	else {
		std::cout << "  C1 data unavailable - C2 proximity/margin cannot be determined.\n";
		std::cout << "  This drive verifies sectors are readable but cannot measure signal quality.\n";
	}
}

void OpticalDrive::PrintBlerQualitySummary(const BlerResult& result) {
	std::cout << "\n" << std::string(60, '-') << "\n";
	std::cout << "  QUALITY: " << result.qualityRating << "\n";

	if (result.qualityRating == "EXCELLENT") {
		if (result.hasC1Data)
			std::cout << "  No C2 errors. C1 rate low - disc has strong error margin.\n";
		else
			std::cout << "  No C2 (uncorrectable) errors detected.\n";
	}
	else if (result.qualityRating == "GOOD") {
		if (result.totalC2Sectors == 0)
			std::cout << "  No C2 errors, but C1 rate is elevated. Margin is narrowing.\n";
		else
			std::cout << "  Minor errors within acceptable limits. Disc is safe to rip.\n";
	}
	else if (result.qualityRating == "ACCEPTABLE") {
		if (result.totalC2Sectors == 0)
			std::cout << "  No C2 errors yet, but C1 rate is high. Rip soon at low speed.\n";
		else
			std::cout << "  Moderate error rate. Recommend secure rip mode.\n";
	}
	else if (result.qualityRating == "FAIR") {
		if (result.totalC2Sectors == 0)
			std::cout << "  C1 rate exceeds Red Book limit. C2 errors may appear under stress.\n";
		else
			std::cout << "  Elevated error rate. Use secure or paranoid rip mode.\n";
	}
	else if (result.qualityRating == "POOR") {
		std::cout << "  High C2 error rate. Use Paranoid rip mode. Consider cleaning disc.\n";
	}
	else {
		std::cout << "  Read failures detected. Some data may be unrecoverable.\n";
	}
	std::cout << std::string(60, '=') << "\n";
}
