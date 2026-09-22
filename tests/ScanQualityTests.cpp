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
				sample.measuredSectors = 75;
				sample.c1 = value;
				hardware.push_back(sample);
				bler.perSecondC1.push_back({sample.lba, value});
				bler.c1Samples.push_back({sample.lba, 75, value});
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
			sample.measuredSectors = interval.sectors;
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
	composite.bler.c1Samples = {{0,75,1}, {75,75,1}, {150,75,1}};
	composite.rot.rotRiskLevel = "LOW";
	struct GradeCase { double rate; int cap; const char* grade; };
	for (const auto& g : {GradeCase{0,100,"A"}, {4.999,100,"A"}, {5,89,"B"},
		{49.999,89,"B"}, {50,79,"C"}, {219.999,79,"C"}, {220,59,"F"}, {300,59,"F"}}) {
		composite.bler.avgC1PerSecond = g.rate;
		composite.bler.c1 = ScanQuality::SummarizeC1(composite.bler.c1Samples, composite.bler.hasC1Data);
	// Grading fixture: isolate policy with a known duration and the configured mean.
	composite.bler.c1.average = composite.bler.avgC1PerSecond;
	ComprehensiveQuality::Finalize(composite);
		check(composite.overallScore == g.cap && composite.overallRating == g.grade,
			"Comprehensive grade respects every shared C1 boundary");
	}
	composite.rot.rotRiskLevel = "HIGH";
	composite.bler.avgC1PerSecond = 1;
	composite.bler.c1 = ScanQuality::SummarizeC1(composite.bler.c1Samples, composite.bler.hasC1Data);
	// Grading fixture: isolate policy with a known duration and the configured mean.
	composite.bler.c1.average = composite.bler.avgC1PerSecond;
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallScore <= 40 && composite.overallRating == "F",
		"Low C1 cannot erase stronger rot evidence");
	composite.rot.rotRiskLevel = "NONE";
	for (bool c1Missing : {false, true}) {
		for (bool c2Missing : {false, true}) {
			composite.bler.hasC1Data = !c1Missing;
			composite.bler.c2Unverified = c2Missing;
			composite.bler.c1 = ScanQuality::SummarizeC1(composite.bler.c1Samples, composite.bler.hasC1Data);
	// Grading fixture: isolate policy with a known duration and the configured mean.
	composite.bler.c1.average = composite.bler.avgC1PerSecond;
	ComprehensiveQuality::Finalize(composite);
			check(composite.overallRating == (c1Missing || c2Missing ? "INCOMPLETE" : "A"),
				"Missing channels produce an incomplete grade instead of a clean letter");
		}
	}
	composite.bler.pioneerCdCheckRun = true;
	composite.bler.pioneerCdCheckC2Bytes = 8;
	composite.bler.c1 = ScanQuality::SummarizeC1(composite.bler.c1Samples, composite.bler.hasC1Data);
	// Grading fixture: isolate policy with a known duration and the configured mean.
	composite.bler.c1.average = composite.bler.avgC1PerSecond;
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallRating == "F" && composite.overallScore <= 35 &&
		composite.summary.find("incomplete") == std::string::npos,
		"Confirmed Pioneer loss overrides an earlier incomplete assessment");
	composite.bler.pioneerCdCheckRun = false;
	composite.bler.pioneerCdCheckC2Bytes = 0;
	composite.bler.totalReadFailures = 1;
	composite.bler.c1 = ScanQuality::SummarizeC1(composite.bler.c1Samples, composite.bler.hasC1Data);
	// Grading fixture: isolate policy with a known duration and the configured mean.
	composite.bler.c1.average = composite.bler.avgC1PerSecond;
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallRating == "F" && composite.overallScore <= 59,
		"Confirmed read failure also takes precedence over missing measurements");
	composite.bler.totalReadFailures = 0;
	composite.rot.pioneerCdCheckRun = true;
	composite.rot.pioneerCdCheckC2Bytes = 8;
	composite.bler.c1 = ScanQuality::SummarizeC1(composite.bler.c1Samples, composite.bler.hasC1Data);
	// Grading fixture: isolate policy with a known duration and the configured mean.
	composite.bler.c1.average = composite.bler.avgC1PerSecond;
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallRating == "F" && composite.overallScore <= 35,
		"Disc Rot's independent confirmed loss reaches the composite assessment");
	composite.rot.pioneerCdCheckRun = false;
	composite.rot.pioneerCdCheckC2Bytes = 0;
	composite.bler.hasC1Data = true;
	composite.bler.c2Unverified = false;
	composite.bler.c1 = ScanQuality::SummarizeC1(composite.bler.c1Samples, composite.bler.hasC1Data);
	// Grading fixture: isolate policy with a known duration and the configured mean.
	composite.bler.c1.average = composite.bler.avgC1PerSecond;
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallScore == 100 && composite.overallRating == "A" &&
		composite.summary.find("confirmed") == std::string::npos &&
		composite.summary.find("incomplete") == std::string::npos,
		"Finalizing replaced evidence clears stale failure and incomplete states");
	composite.bler.perSecondC1.clear();
	composite.bler.c1Samples.clear();
	composite.bler.c1 = ScanQuality::SummarizeC1(composite.bler.c1Samples, composite.bler.hasC1Data);
	// Grading fixture: isolate policy with a known duration and the configured mean.
	composite.bler.c1.average = composite.bler.avgC1PerSecond;
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallRating == "INCOMPLETE", "A capability flag without C1 samples is not measured C1");
	composite.bler.perSecondC1 = {{0,0}};
	composite.bler.c1Samples = {{0,75,0}};
	composite.bler.avgC1PerSecond = std::numeric_limits<double>::quiet_NaN();
	composite.bler.c1 = ScanQuality::SummarizeC1(composite.bler.c1Samples, composite.bler.hasC1Data);
	// Grading fixture: isolate policy with a known duration and the configured mean.
	composite.bler.c1.average = composite.bler.avgC1PerSecond;
	ComprehensiveQuality::Finalize(composite);
	check(composite.overallRating == "INCOMPLETE", "Invalid C1 rates cannot give a comprehensive clean grade");
	const auto partial = SummarizeC1({{0,30,4}});
	check(partial.Rating() == C1Rating::Good && partial.average == 10.0 &&
		partial.MeasuredSeconds() == 0.4 && partial.total == 4,
		"Short final sample uses actual audio duration instead of one poll = one second");
	const auto weighted = SummarizeC1({{0,75,10}, {75,150,10}});
	check(std::abs(weighted.average - 20.0 / 3.0) < 1e-9,
		"Average divides all counts by measured seconds, not mean of interval rates");
	const auto gap = SummarizeC1({{0,75,50}, {150,75,50}});
	check(gap.average == 50 && gap.measuredSectors == 150,
		"Skipped audio does not dilute the measured C1 average");
	check(!SummarizeC1({{0,0,50}, {75,75,50}}).RateAvailable() &&
		!SummarizeC1({{0,75,50}, {0,75,50}}).RateAvailable() &&
		!SummarizeC1({{75,75,50}, {0,75,50}}).RateAvailable(),
		"Unknown, duplicate and reversed coverage cannot produce a timed rating");
	check(!SummarizeC1({{0,75,0}}, false).RateAvailable() &&
		!SummarizeC1({{0,75,-1}}).RateAvailable(),
		"Unverified counters and invalid counts remain unrated");
	std::vector<C1Interval> hotSpot;
	for (unsigned i=0; i<20; ++i) hotSpot.push_back({i*75,75,i<10 ? 0 : 200});
	auto local = SummarizeC1(hotSpot);
	check(local.average == 100 && local.tenSecondWindowAvailable &&
		local.worstTenSecondAverage == 200 && local.fullSeconds.sustainedPeak == 200,
		"Ten-second diagnostic reveals a local region hidden by the whole-scan average");
	hotSpot.erase(hotSpot.begin()+5);
	hotSpot.erase(hotSpot.begin()+14);
	local = SummarizeC1(hotSpot);
	check(!local.tenSecondWindowAvailable,
		"Ten-second windows do not bridge missing intervals");
	std::vector<C1Interval> sectors;
	for (unsigned i=0; i<780; ++i) if (i!=100) AppendC1Sector(sectors,i,1);
	const auto readCd = SummarizeC1(sectors);
	check(readCd.total==779 && readCd.measuredSectors==779 && readCd.average==75 &&
		!readCd.tenSecondWindowAvailable,
		"READ CD counts successful sectors and preserves gaps after read failures");
	const auto invalidated = SummarizeC1({});
	check(!invalidated.RateAvailable() && invalidated.total==0 &&
		!invalidated.tenSecondWindowAvailable && !invalidated.fullSeconds.persistenceMeasurable,
		"Empty replacement observations cannot retain a prior rate or local window");
	std::ostringstream c1Report;
	PrintC1Summary(c1Report, gap, 225);
	check(c1Report.str().find("100 (ungraded)")!=std::string::npos &&
		c1Report.str().find("0:02.000")!=std::string::npos &&
		c1Report.str().find("66.67%")!=std::string::npos &&
		c1Report.str().find("50.00/sec - FAIR")!=std::string::npos,
		"Shared report associates the ungraded total with measured duration, coverage and one average grade");
	check(BuildTimedCounterGraph({{0,30,4}},0,30,1).values == std::vector<int>{10},
		"C1 graph uses the same duration normalization as the numeric report");
	QCheckResult timedHardware;
	for (DWORD i=0; i<3; ++i) {
		QCheckSample sample;
		sample.lba=i*75; sample.c1=250; sample.measuredSectors=i==2 ? 30 : 75;
		timedHardware.samples.push_back(sample);
	}
	ComputeScanPeakContext(timedHardware.samples, 8, timedHardware.peaks);
	ComputeTimedC1(timedHardware);
	check(timedHardware.c1.average == 312.5 &&
		!timedHardware.peaks.sustainedMeasurable,
		"Hardware adapter corrects the mean and does not count a partial third sample as a full second");
	auto unverified = SummarizeC1({{0,750,0}});
	unverified.verified = false;
	std::ostringstream unverifiedReport;
	PrintC1Summary(unverifiedReport, unverified, 750);
	check(unverifiedReport.str().find("Average C1: NOT RATED") != std::string::npos &&
		unverifiedReport.str().find("10-second average: unavailable") != std::string::npos,
		"Late counter invalidation withholds local-rate values as well as the average grade");
	std::vector<std::pair<unsigned,unsigned>> attempted;
	const bool headComplete = ReadCdScanChunks(225,75,[&](unsigned start,unsigned count) {
		attempted.push_back({start,count});
		return start != 257;
	});
	check(!headComplete && attempted.size()==5 && attempted.back().first==289 &&
		attempted.back().second==11,
		"LiteOn head scan continues after a bad chunk but does not claim complete coverage");
	check(!SummarizeC1({{225,headComplete ? 75u : 0u,100}}).RateAvailable(),
		"Incomplete drive-head coverage cannot turn a retained counter into a per-second rate");
	attempted.clear();
	check(ReadCdScanChunks(375,30,[&](unsigned start,unsigned count) {
		attempted.push_back({start,count}); return true;
	}) && attempted.size()==2 && attempted.back()==std::pair<unsigned,unsigned>{391,14},
		"LiteOn final partial read has exact coverage and never extends beyond the requested end");
	check(!ReadCdScanChunks(0,0,[](unsigned,unsigned) { return true; }),
		"Empty head-read ranges are not verified coverage");
	// A completed quiet region is valid zero activity, not a transport failure.
	std::vector<C1Interval> quietRegion, activeRegion;
	for (unsigned i=0; i<15; ++i) {
		quietRegion.push_back({1000+i*75,75,0});
		activeRegion.push_back({1000+i*75,75,20});
	}
	unsigned measuredSpeeds = 0;
	for (const auto& observations : {quietRegion,activeRegion}) {
		const auto capture = SummarizeCompletedC1(observations,15,true);
		if (!capture.RateAvailable()) break;
		++measuredSpeeds;
	}
	check(measuredSpeeds==2 && SummarizeCompletedC1(quietRegion,15,true).average==0,
		"A valid zero-error speed sample does not stop the subsequent balance sweep");
	check(!SummarizeCompletedC1(quietRegion,15,false).RateAvailable() &&
		!SummarizeCompletedC1(quietRegion,16,true).RateAvailable(),
		"Failed and incomplete speed captures remain excluded even when their counters are zero");
	quietRegion.back().sectors=0;
	check(!SummarizeCompletedC1(quietRegion,15,true).RateAvailable(),
		"Known sample count cannot substitute for verified timing in the balance sweep");

	const auto gappedGraph = BuildTimedCounterGraph({{0,75,0},{75,75,0},{7500,75,100}},0,7575,60);
	check(gappedGraph.valid && gappedGraph.values[59]==100 && gappedGraph.values[40]==-1 &&
		gappedGraph.peakLba==7500,
		"A peak at 100 seconds stays at the end of the disc-position graph after missing samples");
	check(gappedGraph.values[0]==0 && !gappedGraph.partialCoverage[0] &&
		gappedGraph.values[1]==0 && gappedGraph.partialCoverage[1] && gappedGraph.values[2]==-1,
		"Measured zero, partial zero and unmeasured columns remain distinct");
	Console::GraphOptions timedOptions;
	Console::ConfigureC1Graph(timedOptions);
	Console::ConfigureTimedGraph(timedOptions,gappedGraph);
	std::ostringstream accessible;
	auto* savedOutput = std::cout.rdbuf(accessible.rdbuf());
	Console::detail::AccessibleBarGraph(gappedGraph.values,250,timedOptions,101);
	std::cout.rdbuf(savedOutput);
	check(accessible.str().find("at 1:40")!=std::string::npos &&
		accessible.str().find("Average 33.33/sec over measured audio")!=std::string::npos &&
		accessible.str().find("57 unmeasured columns, 2 partially measured columns")!=std::string::npos,
		"Accessible chart uses the true peak position, weighted mean and coverage rather than pixel averages");
	const auto offsetGraph=BuildTimedCounterGraph({{4575,75,10}},4500,150,2);
	check(offsetGraph.valid && offsetGraph.values==std::vector<int>({-1,10}) &&
		offsetGraph.firstLba==4500 && offsetGraph.peakLba==4575,
		"A nonzero requested start retains its missing prefix and absolute disc positions");
	const auto partialGraph=BuildTimedCounterGraph({{0,75,3},{75,30,4}},0,105,7);
	check(partialGraph.valid && partialGraph.values==std::vector<int>({3,3,3,3,3,10,10}) &&
		std::none_of(partialGraph.partialCoverage.begin(),partialGraph.partialCoverage.end(),[](bool v){return v;}),
		"Unequal interval durations occupy their correct widths without inventing missing coverage");
	check(Console::detail::FormatDiscTime(30)=="0:00.400",
		"Subsecond disc positions are not rounded down to a zero-length timeline");
	check(!BuildTimedCounterGraph({{0,0,10}},0,75,3).valid &&
		!BuildTimedCounterGraph({{0,75,10},{0,75,10}},0,75,3).valid &&
		!BuildTimedCounterGraph({{0,75,10}},75,75,3).valid &&
		!BuildTimedCounterGraph({{0,75,10}},0,0,3).valid &&
		!BuildTimedCounterGraph({{0,75,10}},0,75,0).valid,
		"Untimed, overlapping, out-of-range and empty-axis inputs cannot produce a measured chart");
	const auto noGraph=BuildTimedCounterGraph({},0,75,3);
	Console::ConfigureTimedGraph(timedOptions,noGraph);
	accessible.str(""); accessible.clear();
	savedOutput=std::cout.rdbuf(accessible.rdbuf());
	Console::detail::AccessibleBarGraph(noGraph.values,250,timedOptions,1);
	std::cout.rdbuf(savedOutput);
	check(accessible.str().find("No measured values")!=std::string::npos &&
		accessible.str().find("Peak ")==std::string::npos && !timedOptions.peakLba,
		"Reusing graph options with missing data clears previous peak and average metadata");
	Console::HeatmapRow spatialRow;
	spatialRow.label="C1"; spatialRow.values=gappedGraph.values;
	spatialRow.lowThresh=50; spatialRow.highThresh=220;
	spatialRow.coverageAware=true; spatialRow.partialCoverage=gappedGraph.partialCoverage;
	accessible.str(""); accessible.clear();
	savedOutput=std::cout.rdbuf(accessible.rdbuf());
	Console::detail::AccessibleHeatmap({spatialRow},"","",101,0,7575);
	std::cout.rdbuf(savedOutput);
	check(accessible.str().find("57 unmeasured, 2 partial columns")!=std::string::npos,
		"Combined heatmaps preserve the same missing and partial regions as C1 bar graphs");
	std::ostringstream absentPeak, measuredZeroPeak;
	PrintC1Summary(absentPeak,SummarizeC1({}),75);
	PrintC1Summary(measuredZeroPeak,SummarizeC1({{0,75,0}}),75);
	check(absentPeak.str().find("Raw peak C1 count: unavailable")!=std::string::npos &&
		absentPeak.str().find("0 in one sample")==std::string::npos &&
		measuredZeroPeak.str().find("Raw peak C1 count: 0 in one sample")!=std::string::npos,
		"An absent raw peak is unavailable while a measured zero remains a numeric zero");
	// Exercise the regular renderer as well as its screen-reader alternative.
	const bool previousAccessibility = Accessibility::IsEnabled();
	Accessibility::SetEnabled(false);
	Console::ConfigureTimedGraph(timedOptions,gappedGraph);
	accessible.str(""); accessible.clear();
	savedOutput=std::cout.rdbuf(accessible.rdbuf());
	Console::DrawBarGraph(gappedGraph.values,250,timedOptions,101);
	std::cout.rdbuf(savedOutput);
	const std::string expectedCoverage = " ~" + std::string(57,'?') + "~";
	check(accessible.str().find(expectedCoverage)!=std::string::npos &&
		accessible.str().find("? unmeasured   ~ partial coverage")!=std::string::npos,
		"Visual C1 graphs render the same explicit gaps and partial coverage as their accessible output");
	Accessibility::SetEnabled(true);
	accessible.str(""); accessible.clear();
	savedOutput=std::cout.rdbuf(accessible.rdbuf());
	Console::DrawBarGraph(gappedGraph.values,250,timedOptions,101);
	std::cout.rdbuf(savedOutput);
	Accessibility::SetEnabled(previousAccessibility);
	check(accessible.str().find("at 1:40")!=std::string::npos &&
		accessible.str().find("Average 33.33/sec")!=std::string::npos,
		"The actual accessibility switch selects the corrected graph summary");
	return failed;
}
