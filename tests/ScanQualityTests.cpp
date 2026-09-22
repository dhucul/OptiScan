#define NOMINMAX
#include "../OpticalDrive.h"
#include "../ConsoleGraph.h"
#include "../CdScanInterval.h"
#include "../ComprehensiveQuality.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <numeric>

// This suite exercises rating analysis without opening a drive. Cluster
// discovery is outside these cases (all calls below pass no error LBAs).
void OpticalDrive::DetectErrorClusters(const std::vector<DWORD>&,
	std::vector<ErrorCluster>&, int) {}

int RunScanQualityTests() {
	int failed = 0;
	auto check = [&](bool ok, const char* label) {
		std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << "\n";
		if (!ok) ++failed;
	};
	using namespace ScanQuality;
	struct Boundary { double rate; C1Rating expected; };
	for (const auto& b : {Boundary{0, C1Rating::Excellent}, {4.999, C1Rating::Excellent},
		{5, C1Rating::Good}, {49.999, C1Rating::Good}, {50, C1Rating::Fair},
		{219.999, C1Rating::Fair}, {220, C1Rating::Poor}, {500, C1Rating::Poor}})
		check(RateC1(b.rate) == b.expected, "C1 average boundary is unambiguous");
	check(RateC1(0, false) == C1Rating::Unrated && RateC1(-1) == C1Rating::Unrated &&
		RateC1(std::numeric_limits<double>::quiet_NaN()) == C1Rating::Unrated &&
		RateC1(std::numeric_limits<double>::infinity()) == C1Rating::Unrated,
		"Missing and invalid rates cannot be rated clean");
	const auto spike = Analyze({2, 2, 1000, 2, 2});
	check(spike.peak == 1000 && spike.total == 1008 && spike.sustainedPeak == 2 &&
		spike.peakIsTransient, "Isolated spike remains in peak, total and average");
	check(TransientNote("C1", spike).find("cause unconfirmed") != std::string::npos,
		"Short spike is not diagnosed as a drive fault");
	check(RateSustainedC1(Analyze({220, 220, 220})) == C1Rating::Poor,
		"Sustained and average ratings agree at 220");
	check(RateSustainedC1(Analyze({300, 300})) == C1Rating::Unrated &&
		RateSustainedC1(Analyze({})) == C1Rating::Unrated,
		"Short and empty captures have no sustained verdict");
	check(!Analyze({300, 300, 300}, 3, {0, 75, 225}).persistenceMeasurable &&
		!Analyze({300, 300, 300}, 3, {0, 75, 75}).persistenceMeasurable &&
		!Analyze({300, 300, 300}, 3, {150, 75, 0}).persistenceMeasurable,
		"Gaps, duplicates and reversed positions break persistence");
	check(Analyze({300, 300, 10, 10, 10}, 3, {0, 75, 225, 300, 375}).sustainedPeak == 10,
		"Only contiguous windows contribute to sustained level");
	std::vector<int> ranks(100);
	std::iota(ranks.begin(), ranks.end(), 1);
	check(Analyze(ranks).p95 == 95 && Analyze(ranks).p99 == 99,
		"Percentiles use nearest rank without a one-sample offset");
	check(CombineC1Quality(C1Rating::Poor, "GOOD") == "POOR" &&
		CombineC1Quality(C1Rating::Fair, "ACCEPTABLE") == "FAIR" &&
		CombineC1Quality(C1Rating::Excellent, "BAD") == "BAD",
		"Combined rating preserves the worse C1 or read evidence");
	check(CombineC1Quality(C1Rating::Unrated, "EXCELLENT") == "NOT RATED" &&
		CombineC1Quality(C1Rating::Unrated, "BAD") == "BAD",
		"Unknown C1 cannot create a clean result or erase read failures");
	Console::GraphOptions graph;
	Console::ConfigureC1Graph(graph);
	check(Console::detail::HasAbsoluteSeverity(graph) &&
		Console::detail::AbsoluteSeverity(49, graph) == 0.20 &&
		Console::detail::AbsoluteSeverity(50, graph) == 0.55 &&
		Console::detail::AbsoluteSeverity(219, graph) == 0.55 &&
		Console::detail::AbsoluteSeverity(220, graph) == 0.90,
		"All C1 graphs use absolute 50/220 bands independent of scale");
	OpticalDrive drive;
	for (int value : {0, 4, 5, 49, 50, 219, 220, 1000}) {
		for (int speed : {0, 4, 8, 16, 48}) {
			std::vector<QCheckSample> hardware;
			BlerResult bler;
			bler.hasC1Data = true;
			bler.totalSeconds = 12;
			bler.totalC1Errors = value * 12;
			for (DWORD i = 0; i < 12; ++i) {
				QCheckSample sample;
				sample.lba = i * 75;
				sample.c1 = value;
				hardware.push_back(sample);
				bler.perSecondC1.push_back({sample.lba, value});
			}
			ScanPeakContext peaks;
			ComputeScanPeakContext(hardware, speed, peaks);
			drive.AnalyzeBlerResults(bler, {}, speed);
			const std::string expected = C1RatingName(RateC1(value));
			check(bler.qualityRating == expected && bler.sustainedC1Rating == expected &&
				::RateSustainedC1(peaks) == expected,
				"Hardware and BLER adapters agree at every C1 boundary and scan speed");
			bler.totalC2Sectors = 1;
			bler.totalC2Errors = 1;
			drive.AnalyzeBlerResults(bler, {}, speed);
			check(bler.qualityRating == CombineC1Quality(RateC1(value), "GOOD"),
				"BLER C2 branch cannot hide the C1 rating");
		}
	}
	BlerResult missing;
	drive.AnalyzeBlerResults(missing, {}, 8);
	check(missing.qualityRating == "NOT RATED", "Empty BLER capture is not excellent");
	std::ostringstream policy;
	PrintC1Policy(policy);
	check(policy.str().find("compliance is not evaluated") != std::string::npos &&
		policy.str().find("10-second") != std::string::npos,
		"Shared report policy states the actual standards-measurement limitation");

	// Exercise the interval geometry used by the actual LiteOn reader through
	// the terminal transition, then feed those positions into shared analysis.
	for (DWORD endLba : {449UL, 414UL}) {
		std::vector<QCheckSample> terminalSamples;
		DWORD cursor = 0;
		unsigned covered = 0;
		for (;;) {
			const auto interval = CdScanInterval::At(cursor, endLba);
			check(interval.sectors > 0 && interval.startLba == cursor &&
				interval.startLba + interval.sectors - 1 <= endLba,
				"LiteOn interval covers only requested sectors and reports its start");
			QCheckSample sample;
			sample.lba = interval.startLba;
			sample.c1 = cursor >= 225 ? 250 : 2;
			terminalSamples.push_back(sample);
			covered += interval.sectors;
			if (interval.final) break;
			cursor += interval.sectors;
		}
		ScanPeakContext terminalPeaks;
		ComputeScanPeakContext(terminalSamples, 8, terminalPeaks);
		check(covered == endLba + 1 && terminalSamples.back().lba == 375,
			"Full and partial final intervals finish without moving the sample coordinate");
		check(terminalPeaks.sustainedC1PerSecond == 250 &&
			::RateSustainedC1(terminalPeaks) == "POOR" && !terminalPeaks.peakC1Transient,
			"Last three elevated LiteOn intervals remain a sustained poor reading");
	}
	check(CdScanInterval::At(450, 449).sectors == 0 &&
		CdScanInterval::At(450, 449).final,
		"Polling an exhausted interval range produces no extra measurement");
	check(CdScanInterval::At(0, std::numeric_limits<std::uint32_t>::max()).sectors == 75,
		"Interval length arithmetic cannot wrap to an empty range");

	// Test the real CSV writer: an independent failure wins over missing C1/C2.
	const auto failureLog = std::filesystem::temp_directory_path() /
		("OptiScan-quality-" + std::to_string(GetCurrentProcessId()) + "-" +
			std::to_string(GetTickCount64()) + ".csv");
	for (bool c1Present : {false, true}) {
		for (bool c2Unverified : {false, true}) {
			for (int failure : {0, 1, 2, 3}) {
				BlerResult exported;
				exported.hasC1Data = c1Present;
				exported.c2Unverified = c2Unverified;
				exported.qualityRating = failure == 3 ? "BAD" : "UNVERIFIED";
				exported.totalReadFailures = failure == 1 ? 1 : 0;
				exported.pioneerVendorQuality = true;
				exported.pioneerCdCheckRun = failure == 2;
				exported.pioneerCdCheckC2Bytes = 8; // unusable unless the run flag is true
				check(drive.SaveBlerLog(exported, failureLog.wstring()), "CSV regression output saved");
				std::ifstream saved(failureLog);
				const std::string csv{std::istreambuf_iterator<char>(saved),
					std::istreambuf_iterator<char>()};
				const std::string expected = failure != 0 ? "BAD" :
					(c2Unverified && !c1Present ? "INCOMPLETE" : "UNVERIFIED");
				check(csv.find("# Quality Rating:        " + expected) != std::string::npos,
					"CSV failure precedence survives every missing-channel combination");
				check(failure == 0 || csv.find("C1 quality only") == std::string::npos,
					"Confirmed failure is not relabelled as a C1-only assessment");
			}
		}
	}
	std::error_code cleanup;
	std::filesystem::remove(failureLog, cleanup);
	check(!cleanup, "CSV regression output removed");

	ComprehensiveScanResult composite;
	composite.bler.hasC1Data = true;
	composite.bler.perSecondC1 = {{0, 1}, {75, 1}, {150, 1}};
	composite.rot.rotRiskLevel = "LOW";
	struct GradeCase { double rate; int cap; const char* grade; };
	for (const auto& g : {GradeCase{0,100,"A"}, {4.999,100,"A"}, {5,89,"B"},
		{49.999,89,"B"}, {50,79,"C"}, {219.999,79,"C"}, {220,59,"F"}, {300,59,"F"}}) {
		composite.bler.avgC1PerSecond = g.rate;
		ComprehensiveQuality::Finalize(composite);
		check(composite.overallScore == g.cap && composite.overallRating == g.grade,
			"Comprehensive grade respects every shared C1 boundary");
	}
	composite.rot.rotRiskLevel = "HIGH";
	composite.bler.avgC1PerSecond = 1;
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallScore <= 40 && composite.overallRating == "F",
		"Low C1 cannot erase stronger rot evidence");
	composite.rot.rotRiskLevel = "NONE";
	for (bool c1Missing : {false, true}) {
		for (bool c2Missing : {false, true}) {
			composite.bler.hasC1Data = !c1Missing;
			composite.bler.c2Unverified = c2Missing;
			ComprehensiveQuality::Finalize(composite);
			check(composite.overallRating == (c1Missing || c2Missing ? "INCOMPLETE" : "A"),
				"Missing channels produce an incomplete grade instead of a clean letter");
		}
	}
	composite.bler.pioneerCdCheckRun = true;
	composite.bler.pioneerCdCheckC2Bytes = 8;
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallRating == "F" && composite.overallScore <= 35 &&
		composite.summary.find("incomplete") == std::string::npos,
		"Confirmed Pioneer loss overrides an earlier incomplete assessment");
	composite.bler.pioneerCdCheckRun = false;
	composite.bler.pioneerCdCheckC2Bytes = 0;
	composite.bler.totalReadFailures = 1;
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallRating == "F" && composite.overallScore <= 59,
		"Confirmed read failure also takes precedence over missing measurements");
	composite.bler.totalReadFailures = 0;
	composite.rot.pioneerCdCheckRun = true;
	composite.rot.pioneerCdCheckC2Bytes = 8;
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallRating == "F" && composite.overallScore <= 35,
		"Disc Rot's independent confirmed loss reaches the composite assessment");
	composite.rot.pioneerCdCheckRun = false;
	composite.rot.pioneerCdCheckC2Bytes = 0;
	composite.bler.hasC1Data = true;
	composite.bler.c2Unverified = false;
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallScore == 100 && composite.overallRating == "A" &&
		composite.summary.find("confirmed") == std::string::npos &&
		composite.summary.find("incomplete") == std::string::npos,
		"Finalizing replaced evidence clears stale failure and incomplete states");
	composite.bler.perSecondC1.clear();
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallRating == "INCOMPLETE", "A capability flag without C1 samples is not measured C1");
	composite.bler.perSecondC1 = {{0,0}};
	composite.bler.avgC1PerSecond = std::numeric_limits<double>::quiet_NaN();
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallRating == "INCOMPLETE", "Invalid C1 rates cannot give a comprehensive clean grade");
	return failed;
}
