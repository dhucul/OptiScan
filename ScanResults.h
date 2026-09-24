// ============================================================================
// ScanResults.h - Disc quality scanning results
// ============================================================================
#pragma once

#include "ErrorTypes.h"
#include "AnalysisTypes.h"
#include "ScanQualityRating.h"
#include "ScanTelemetry.h"
#include <windows.h>
#include <vector>
#include <string>
#include <utility>

// ── Shared peak / confidence block ──────────────────────────────────────────
// Every scan mode that rates a disc from a per-time-slice error series carries
// this block, so the rules stay identical between them. Raw peaks are still
// kept for graphs; sustained and average rates use the same descriptive bands.
// scanSpeedX supplies measurement context, not proof of a peak's cause.
struct ScanPeakContext {
	int scanSpeedX = 0;                     // Speed the scan actually ran at

	// False when no contiguous group of kDefaultMinRunSamples was recorded, so
	// "held for N consecutive slices" cannot be evaluated at all. Both series
	// come from the same samples, so one flag covers C1 and E22. Every tier
	// judged from a sustained level must come back Unrated when this is false.
	bool sustainedMeasurable = false;

	// C1 / BLER series
	int  sustainedC1PerSecond = 0;          // Level held >= kDefaultMinRunSamples
	int  p95C1PerSecond = 0;
	int  peakC1RunLength = 0;
	bool peakC1Transient = false;

	// Pioneer E22 diagnostic series
	int  sustainedPioneerE22PerSecond = 0;
	int  peakPioneerE22RunLength = 0;
	bool peakPioneerE22Transient = false;
	bool pioneerE22PeakTracksC1 = false;    // Peaks at the same slice as C1

	ScanQuality::Confidence PeakConfidence() const {
		return ScanQuality::ConfidenceForSpeed(scanSpeedX);
	}
	bool PeaksAdmissible() const {
		return ScanQuality::PeakEvidenceAdmissible(PeakConfidence());
	}
};

// ── Counter sample from hardware quality scan ────────────────────────────
struct QCheckSample {
	DWORD lba = 0;          // Approximate LBA at this time slice
	int c1 = 0;             // Raw C1 (BLER) count; duration is supplied separately
	int c2 = 0;             // Verified C2 error count for this second
	int cu = 0;             // CU (uncorrectable) count for this second
	int pioneerE22 = 0;     // Pioneer vendor E22 diagnostic count, not counted as C2
	DWORD measuredSectors = 0; // Exact disc coverage of these counters; zero = unknown
	std::uint64_t elapsedMs = 0; // Elapsed wall time within this pass
};

// ── Per-track aggregation of C2 / CU events for the report ──────────────────
struct QCheckTrackErrors {
	int trackNumber = 0;
	int c2Count = 0;        // Sum of C2 errors observed in samples within this track
	int cuCount = 0;        // Sum of CU errors observed in samples within this track
};

// ── Hardware quality scan result ────────────────────────────────────────────
// Populated by the hardware-driven quality scan using Plextor Q-Check
// (0xE9/0xEB), Pioneer (0x3B/0x3C), or LiteOn/MediaTek (0xDF/0xF3)
// vendor commands.  Aggregate CIRC decoder statistics per time slice.
struct QCheckResult {
	std::string discIdentity;
	int requestedSpeed = 0;
	Diagnostics::HardwareSpeedEvidence speed, recheckSpeed;
	Diagnostics::ScanThroughput throughput, recheckThroughput;
	bool supported = false;                    // True if a scan method was available
	std::string scanMethod;                    // E.g. "Plextor Q-Check (0xE9/0xEB)"
	DWORD totalSectors = 0;                    // Requested scan range length
	DWORD totalSeconds = 0;                    // Requested audio length, rounded up; not measured duration

	DWORD graphStartLba = 0;
	std::uint64_t graphSectors = 0;
	ScanQuality::C1Statistics c1;

	// Aggregate C1 statistics
	int totalC1 = 0;
	double avgC1PerSecond = 0.0;
	int maxC1PerSample = 0;
	int maxC1SampleIndex = -1;

	// Aggregate C2 statistics
	int totalC2 = 0;
	double avgC2PerSecond = 0.0;
	int maxC2PerSecond = 0;
	int maxC2SecondIndex = -1;

	// Verification re-scan evidence. When the primary pass reports C2, Q-Check
	// runs a second full-disc pass. Keep that pass separate instead of replacing
	// or zeroing the primary measurements: a clean recheck means the activity was
	// intermittent, not that the first observation never happened.
	bool c2RecheckAttempted = false;
	bool c2RecheckCompleted = false;
	int c2RecheckTotalC1 = 0;
	int c2RecheckTotal = 0;
	int c2RecheckTotalCU = 0;
	double c2RecheckAvgC2PerSecond = 0.0;
	int c2RecheckMaxPerSecond = 0;
	int c2RecheckMaxSecondIndex = -1;
	std::vector<QCheckSample> c2RecheckSamples;
	std::vector<QCheckTrackErrors> c2RecheckErrorTracks;

	// Pioneer vendor E22 diagnostics.  These are intentionally separate
	// from C2 because Pioneer 0x3B/0x3C E22 does not match LiteOn/Plextor
	// C2 totals closely enough to drive the copy/no-copy decision.
	int totalPioneerE22 = 0;
	double avgPioneerE22PerSecond = 0.0;
	int maxPioneerE22PerSecond = 0;
	int maxPioneerE22SecondIndex = -1;

	// Aggregate CU statistics
	int totalCU = 0;
	int maxCUPerSecond = 0;

	// Whether the scan backend actually measures CU (uncorrectable) errors.
	// Plextor Q-Check and host-driven LiteOn report E32/CU. LiteOn F3 and the
	// Pioneer 0x3B/0x3C vendor scan does NOT — it only exposes C1 (BLER) and
	// the E22 second-stage counter, so its CU is always 0 by omission, not by
	// measurement. When false, the report must not present CU == 0 as a clean
	// bill of health (that would imply an uncorrectable check that never ran).
	bool cuMeasured = true;

	// Pioneer CD Check (0xE6) uncorrectable cross-check. The 0x3B/0x3C vendor
	// quality scan exposes no CU, so on Pioneer drives that implement the CD
	// Check protocol we run it over the same audio range to obtain a genuine
	// uncorrectable measurement. pioneerCdCheckRun is true only when the CD
	// Check produced valid data (older Pioneer drives support it; the BDR-S13U
	// appears to have dropped it, in which case this stays false).
	bool pioneerCdCheckRun = false;
	int  pioneerCdCheckC1Frames = 0;   // worst-window C1 uncorrectable frame count
	int  pioneerCdCheckC2Bytes  = 0;   // worst-window C2 uncorrectable byte count (real data loss)
	// Per-second time-series data
	std::vector<QCheckSample> samples;

	// Tracks affected by C2 / CU errors, computed from samples + TOC after the
	// scan completes. Sorted by track number; entries with both counts == 0
	// are omitted.
	std::vector<QCheckTrackErrors> errorTracks;

	// Quality assessment
	std::string qualityRating;                 // EXCELLENT / GOOD / FAIR / POOR / BAD

	// Average C1 rate interpretation; total remains ungraded
	std::string averageC1Rating;                // EXCELLENT / GOOD / FAIR / POOR / NOT RATED

	// Sustained C1 observed-rate assessment. Driven by peaks.sustainedC1PerSecond,
	// separate from maxC1PerSample and the whole-scan average.
	std::string sustainedC1Rating;              // EXCELLENT / GOOD / FAIR / POOR / NOT RATED

	// Pioneer E22 diagnostic rating (diagnostic only — not a copy trigger)
	std::string pioneerE22Rating;              // Ideal / Good / Acceptable / Concerning / NOT RATED

	// Sustained-level statistics and scan-speed confidence shared with every
	// other scan mode.
	ScanPeakContext peaks;

	// Drive compatibility warning — zero C1 across entire disc suggests
	// the drive accepted the vendor scan command but does not actually
	// populate CIRC error statistics.  Not all drives that pass the
	// initial capability probe produce valid measurement data.
	bool c1Unverified = false;
};

enum class QCheckC2Stability {
	NoActivity,
	Intermittent,
	Reproducible,
	Unrecoverable,
	RecheckIncomplete
};

inline bool HasCompleteQCheckCoverage(const std::vector<QCheckSample>& samples,
	std::uint32_t firstLba, std::uint64_t sectorCount) {
	if (samples.empty() || sectorCount == 0 || sectorCount > 0x100000000ULL - firstLba) return false;
	const std::uint64_t end = std::uint64_t{firstLba} + sectorCount;
	std::uint64_t next = firstLba;
	for (const auto& sample : samples) {
		if (sample.measuredSectors == 0 || sample.lba != next ||
			sample.measuredSectors > end - next) return false;
		next += sample.measuredSectors;
	}
	return next == end;
}

inline QCheckC2Stability ClassifyQCheckC2Stability(const QCheckResult& result) {
	// Positive evidence remains valid even if the verification pass was
	// interrupted. Only a clean verdict requires a completed pass with at
	// least one usable sample.
	if (result.c2RecheckTotalCU > 0)
		return QCheckC2Stability::Unrecoverable;
	if (result.c2RecheckTotal > 0)
		return QCheckC2Stability::Reproducible;
	if (result.c2RecheckCompleted && HasCompleteQCheckCoverage(result.c2RecheckSamples,
		result.graphStartLba, result.graphSectors) &&
		result.totalC2 > 0)
		return QCheckC2Stability::Intermittent;
	if (result.totalC2 > 0)
		return QCheckC2Stability::RecheckIncomplete;
	return QCheckC2Stability::NoActivity;
}

// ── Fill a ScanPeakContext from a per-slice series ──────────────────────────
// Single implementation shared by the quality scan, BLER/C2 scan, Disc Rot and
// Disc Balance so no scan mode can drift into its own peak handling.
inline void ComputeScanPeakContext(const std::vector<int>& c1Series,
	const std::vector<int>& e22Series, int scanSpeedX, ScanPeakContext& out,
	const std::vector<DWORD>& sampleLbas = {}) {
	out.scanSpeedX = scanSpeedX;

	const ScanQuality::SeriesStats c1 = ScanQuality::Analyze(c1Series, ScanQuality::kDefaultMinRunSamples, sampleLbas);
	out.sustainedMeasurable = c1.persistenceMeasurable;
	out.sustainedC1PerSecond = c1.sustainedPeak;
	out.p95C1PerSecond = c1.p95;
	out.peakC1RunLength = c1.peakRunLength;
	out.peakC1Transient = c1.peakIsTransient;

	if (e22Series.empty()) {
		// No E22 series this pass. Clear rather than leave the previous scan's
		// figures in place — this context is reused across passes.
		out.sustainedPioneerE22PerSecond = 0;
		out.peakPioneerE22RunLength = 0;
		out.peakPioneerE22Transient = false;
		out.pioneerE22PeakTracksC1 = false;
	}
	else {
		const ScanQuality::SeriesStats e22 = ScanQuality::Analyze(e22Series, ScanQuality::kDefaultMinRunSamples, sampleLbas);
		out.sustainedPioneerE22PerSecond = e22.sustainedPeak;
		out.peakPioneerE22RunLength = e22.peakRunLength;
		out.peakPioneerE22Transient = e22.peakIsTransient;
		out.pioneerE22PeakTracksC1 = ScanQuality::PeaksCorrelated(c1, e22);
	}
}

inline void ComputeScanPeakContext(const std::vector<QCheckSample>& samples,
	int scanSpeedX, ScanPeakContext& out) {
	std::vector<int> c1, e22;
	std::vector<DWORD> sampleLbas;
	c1.reserve(samples.size());
	e22.reserve(samples.size());
	bool anyE22 = false;
	for (const auto& s : samples) {
		c1.push_back(s.c1);
		sampleLbas.push_back(s.lba);
		e22.push_back(s.pioneerE22);
		if (s.pioneerE22 != 0) anyE22 = true;
	}
	if (!anyE22) e22.clear();
	ComputeScanPeakContext(c1, e22, scanSpeedX, out, sampleLbas);
}

inline std::vector<ScanQuality::C1Interval> QCheckCounterIntervals(
	const std::vector<QCheckSample>& samples, int QCheckSample::*counter) {
	std::vector<ScanQuality::C1Interval> intervals;
	intervals.reserve(samples.size());
	for (const auto& sample : samples)
		intervals.push_back({sample.lba, sample.measuredSectors, sample.*counter});
	return intervals;
}

inline std::vector<ScanQuality::C1Interval> C1Intervals(const std::vector<QCheckSample>& samples) {
	return QCheckCounterIntervals(samples, &QCheckSample::c1);
}

// Reports, exports and menu graphs use identical values, units and peak positions.
inline ScanQuality::TimedCounterGraph BuildQCheckCounterGraph(const QCheckResult& result,
	int QCheckSample::*counter, int width = 60, bool verificationPass = false) {
	return ScanQuality::BuildObservedCounterGraph(QCheckCounterIntervals(
		verificationPass ? result.c2RecheckSamples : result.samples, counter),
		result.graphStartLba, result.graphSectors, width, !result.c1Unverified);
}

inline void ComputeTimedC1(QCheckResult& result) {
	result.c1 = ScanQuality::SummarizeC1(C1Intervals(result.samples), !result.c1Unverified);
	result.avgC1PerSecond = result.c1.average;
	result.peaks.sustainedMeasurable = result.c1.fullSeconds.persistenceMeasurable;
	result.peaks.sustainedC1PerSecond = result.c1.fullSeconds.sustainedPeak;
	result.peaks.p95C1PerSecond = result.c1.fullSeconds.p95;
	const auto e22 = ScanQuality::SummarizeC1(
		QCheckCounterIntervals(result.samples, &QCheckSample::pioneerE22), !result.c1Unverified);
	result.peaks.sustainedPioneerE22PerSecond = e22.fullSeconds.sustainedPeak;
}

// ── Shared rating entry points ──────────────────────────────────────────────
// Both the quality scan and the disc-rot scan previously kept private copies of
// these thresholds, which drifted. They now share one implementation, and both
// judge the *sustained* level rather than the raw peak.
inline std::string RateSustainedC1(const ScanPeakContext& peaks) {
	// Only sustainedPeak and persistenceMeasurable feed the tier; the transient
	// test already happened when the context was computed.
	ScanQuality::SeriesStats c1;
	c1.sustainedPeak = peaks.sustainedC1PerSecond;
	c1.persistenceMeasurable = peaks.sustainedMeasurable;
	return ScanQuality::C1RatingName(ScanQuality::RateSustainedC1(c1));
}

inline std::string RatePioneerE22(long long total, double avgPerSecond,
	const ScanPeakContext& peaks) {
	// Note this takes persistence from the shared sample count, not from the
	// E22 vector: an all-zero E22 series is cleared by ComputeScanPeakContext
	// as an optimisation, and that means "no E22 activity over a full scan",
	// not "too few slices to tell".
	ScanQuality::SeriesStats e22;
	e22.sustainedPeak = peaks.sustainedPioneerE22PerSecond;
	e22.persistenceMeasurable = peaks.sustainedMeasurable;
	return ScanQuality::TierNameDiagnostic(
		ScanQuality::RatePioneerE22(total, avgPerSecond, e22,
			peaks.PeakConfidence(), peaks.pioneerE22PeakTracksC1));
}

inline const char* SustainedC1RatingDescription(const std::string& rating) {
	if (rating == "EXCELLENT") return "sustained C1 below 5/sec";
	if (rating == "GOOD") return "sustained C1 5-<50/sec";
	if (rating == "FAIR") return "sustained C1 50-<220/sec";
	if (rating == "POOR") return "sustained C1 at least 220/sec";
	return "unavailable, unverified or too few consecutive samples";
}

inline const char* PioneerE22RatingDescription(const std::string& rating) {
	if (rating == "Ideal")      return "no E22 reported";
	if (rating == "Good")       return "low background, normal for Pioneer scans";
	if (rating == "Acceptable") return "elevated diagnostic activity";
	if (rating == "Concerning") return "sustained heavy diagnostic activity";
	return "not measurable at the speed this drive honours";
}

// ── Single jitter/beta time-slice from a LiteOn jitter scan ─────────────────
struct JitterSample {
	DWORD lba = 0;          // Approximate LBA at this time slice
	int jitter = 0;         // Raw jitter value reported by drive
	int beta = 0;           // Signed beta (pit/land asymmetry); ~0 ideal
};

// ── Jitter / beta scan result (LiteOn 0xDF/0x1B vendor command) ─────────────
struct JitterResult {
    bool completed = false;
	bool supported = false;
	DWORD totalSectors = 0;
	DWORD totalSeconds = 0;

	// Aggregate jitter statistics
	long long totalJitter = 0;
	double    avgJitter = 0.0;
	int       maxJitter = 0;
	int       maxJitterSampleIndex = -1;

	// Aggregate beta statistics (signed; min/max + abs-mean)
	int    minBeta = 0;
	int    maxBeta = 0;
	double avgAbsBeta = 0.0;

	std::vector<JitterSample> samples;
};

// ── Single focus/tracking-error time-slice from a LiteOn FE/TE scan ─────────
struct FeTeSample {
	DWORD lba = 0;          // Approximate LBA at this time slice
	int fe = 0;             // Focus-error amplitude (raw, tentative units)
	int te = 0;             // Tracking-error amplitude (raw, tentative units)
};

// ── Focus/Tracking-error scan result (LiteOn 0xDF/0x08 vendor command) ──────
// Servo-level physical measurement. Field interpretation is tentative — see
// ScsiDrive.LiteOnFeTe.cpp. Magnitudes are relative, not absolute.
struct FeTeResult {
    bool completed = false;
	bool supported = false;
	DWORD totalSectors = 0;
	DWORD totalSeconds = 0;

	long long totalFe = 0, totalTe = 0;
	double avgFe = 0.0, avgTe = 0.0;
	int maxFe = 0, maxTe = 0;
	int maxFeSampleIndex = -1, maxTeSampleIndex = -1;

	std::vector<FeTeSample> samples;
};

// ── BLER (Block Error Rate) scan result ─────────────────────────────────────
// Captures the output of a detailed error-rate scan.  BLER measures raw error
// frequency before ECC correction.  Whole-scan averages are not a Red Book compliance test.
struct BlerResult {
	DWORD totalSectors = 0;
	int totalSeconds = 0;
	std::string measurementMethod;             // READ CD C2 or vendor quality backend

	// C2 error statistics
	int totalC2Errors = 0;
	int totalC2Sectors = 0;
	int maxC2InSingleSector = 0;
	int consecutiveErrorSectors = 0;
	int totalReadFailures = 0;
	int recoveredC2Errors = 0;             // C2 errors the drive corrected internally (sense 0x01)
	int recoveredC2Sectors = 0;            // Sectors with recovered C2 errors
	int maxC2PerSecond = 0;
	double avgC2PerSecond = 0.0;
	DWORD worstSectorLBA = 0;
	DWORD worstSecondLBA = 0;

	DWORD graphStartLba = 0;
	std::uint64_t graphSectors = 0;
	ScanQuality::C1Statistics c1;
	std::vector<ScanQuality::C1Interval> c1Samples;

	// C1 error statistics
	int totalC1Errors = 0;
	int totalC1Sectors = 0;
	int maxC1InSingleSector = 0;
	DWORD worstC1SectorLBA = 0;
	double avgC1PerSecond = 0.0;
	int maxC1PerSample = 0;
	DWORD worstC1SecondLBA = 0;

	// Cluster and pattern analysis
	int largestClusterSize = 0;
	bool hasEdgeConcentration = false;
	bool hasProgressivePattern = false;
	bool hasC1Data = false;
	bool c2Unverified = false;             // C2 cannot support a verified clean-disc conclusion
	bool c2PointerDataRecorded = false;    // The sector-reading method returned C2 pointer data, including zero
	std::string qualityRating;

	// Sustained-level statistics and scan-speed confidence, shared with the
	// Q-Check, Disc Rot and Balance paths.
	ScanPeakContext peaks;

	// Sustained C1 tier, computed the same way as QCheckResult's.
	std::string sustainedC1Rating;

	// Pioneer 0x3B/0x3C vendor-quality provenance. E22 is a diagnostic
	// second-stage counter, not a verified READ CD C2 pointer or E32/CU count.
	bool pioneerVendorQuality = false;
	int pioneerE22Total = 0;
	double pioneerE22AvgPerSecond = 0.0;
	int pioneerE22Peak = 0;
	ScanQuality::TimedCounterGraph pioneerE22Observations;
	std::string pioneerE22Rating;

	// Pioneer CD Check (0xE6) cross-check. This is the only Pioneer path here
	// that supplies a genuine uncorrectable-data measurement. The flag is true
	// only after the requested audio range completed with valid measurements.
	bool pioneerCdCheckRun = false;
	int pioneerCdCheckC1Frames = 0;
	int pioneerCdCheckC2Bytes = 0;

	// Per-second time-series data: (LBA, error count) per 75-sector bucket
	std::vector<std::pair<DWORD, int>> perSecondC2;
	std::vector<std::pair<DWORD, int>> perSecondReadFailures;
	std::vector<std::pair<DWORD, int>> perSecondC1;
	std::vector<std::pair<DWORD, int>> perSecondPioneerE22;

	// Error clusters and zone distribution
	std::vector<ErrorCluster> errorClusters;
	DiscZoneStats zoneStats;

	// Confirmed failure takes precedence over missing C1/C2 channels in every
	// downstream summary, including independent Pioneer CD Check evidence.
	bool HasConfirmedFailure() const {
		return totalReadFailures > 0 || qualityRating == "BAD" ||
			(pioneerCdCheckRun && pioneerCdCheckC2Bytes > 0);
	}

	bool HasC1Observations() const { return c1.samples > 0; }
	bool CanAssessC2() const { return c2PointerDataRecorded && !c2Unverified; }

	const char* C1MeasurementLabel() const {
		if (!HasC1Observations()) return "NOT MEASURED - NO C1 READINGS RECORDED";
		if (!hasC1Data || !c1.verified) return "RECORDED - MEASUREMENT UNVERIFIED";
		if (!c1.RateAvailable()) return "RECORDED - RATE UNAVAILABLE";
		return "MEASURED";
	}

	const char* C2MeasurementLabel() const {
		if (!c2PointerDataRecorded) return "NOT MEASURED - NO C2 READINGS RECORDED";
		if (c2Unverified) return "REPORTED - DETECTION NOT INDEPENDENTLY VERIFIED";
		return "MEASURED";
	}

	// Top worst sectors by C2 error count: (LBA, C2 count)
	std::vector<std::pair<DWORD, int>> topWorstC2Sectors;
};

// ── Disc rot analysis results ───────────────────────────────────────────────
// Output of the disc rot detection scan.  Combines zone statistics, error
// cluster data, and heuristic indicators for various rot patterns.
struct DiscRotAnalysis {
	std::string discIdentity;
	int qualityRequestedSpeed = 0;
	Diagnostics::HardwareSpeedEvidence qualitySpeed;
	Diagnostics::ScanThroughput qualityThroughput;
	std::vector<QCheckSample> qualitySamples;
	int totalReadFailures = 0;                  // Phase 1 primary reads still failed after retry
	int recoveredReadFailures = 0;             // Initial Phase 1 failure, then successful retry
	int verificationReadFailures = 0;          // C2-positive sector's verification read failed
	int phase1C2Sectors = 0;                    // Positive C2 observations, retained across retries
	int consistencyReadFailures = 0;           // Failed sampled reads, not byte mismatches
	int consistencyUnverifiedSamples = 0;      // Cache eviction could not be established
	int qualityC2Count = 0;                    // Non-Pioneer Phase 0 observations
	int qualityCUCount = 0;
	bool qualityCountersRecorded = false;
	bool qualityCuMeasured = false;
	bool qualityScanComplete = false;
	std::string qualityScanMethod;
	std::string readabilityStatus;
	DiscZoneStats zones;                        // Error distribution by radial zone
	std::vector<ErrorCluster> clusters;          // Contiguous error regions
	int inconsistentSectors = 0;                // Samples with differing data or failed reads
	int totalRereadTests = 0;                   // Number of re-read comparison tests run
	double inconsistencyRate = 0.0;             // inconsistentSectors / totalRereadTests
	int maxC2InSingleSector = 0;                // Worst C2 count in a single sector
	int pioneerE22Total = 0;                    // Pioneer vendor diagnostic E22, not counted as C2
	double pioneerE22AvgPerSecond = 0.0;        // Average Pioneer E22 diagnostic count in Phase 0
	int pioneerE22Peak = 0;                     // Peak Pioneer E22 diagnostic count in Phase 0
	ScanQuality::TimedCounterGraph pioneerE22Observations;
	std::string pioneerE22Rating;               // Ideal / Good / Acceptable / Concerning / NOT RATED
	bool pioneerDrive = false;                   // Enables explicit CU-unmeasured reporting
	bool pioneerQualityScanRun = false;          // Phase 0 completed with valid Pioneer samples

	// Sustained-level statistics and scan-speed confidence for Phase 0. Risk
	// escalation driven by absolute peak thresholds is gated on this; zone
	// *ratios* stay admissible at any speed because they are speed-robust.
	ScanPeakContext peaks;

	// Pioneer CD Check (0xE6) uncorrectable cross-check. On Pioneer drives the
	// vendor scan (Phase 0) and the per-sector READ CD C2 area (Phase 1) are both
	// blind to uncorrectable (E32/CU) data, so this measures it directly. Data
	// loss is the strongest rot signal, so a non-zero C2-uncorrectable count
	// escalates the rot-risk verdict. pioneerCdCheckRun is true only when the CD
	// Check produced valid data (older Pioneer drives support it; the BDR-S13U
	// appears to have dropped it, in which case this stays false).
	bool pioneerCdCheckRun = false;
	int  pioneerCdCheckC1Frames = 0;            // worst-window C1 uncorrectable frame count
	int  pioneerCdCheckC2Bytes  = 0;            // worst-window C2 uncorrectable byte count (real data loss)

	ScanQuality::C1Statistics c1;
	DWORD c1RequestedSectors = 0;

	// Heuristic disc-rot pattern flags
	bool edgeConcentration = false;             // Errors concentrated at inner/outer edges
	bool progressivePattern = false;            // Error rate increases toward the outer edge
	bool pinholePattern = false;                // Many small scattered clusters; physical cause unconfirmed
	bool readInstability = false;               // High re-read inconsistency rate

	std::string rotRiskLevel;                   // "NONE", "LOW", "MODERATE", "HIGH", "CRITICAL"
	std::string recommendation;                 // Human-readable advice
};

// ── Comprehensive scan result ───────────────────────────────────────────────
// Aggregates results from every individual scan type into a single report
// with an overall 0–100 score and letter grade.
struct ComprehensiveScanResult {
	BlerResult bler;                                   // Block error rate data
	DiscRotAnalysis rot;                               // Disc rot detection data
	AudioAnalysisResult audio;                         // Audio anomaly data
	std::vector<SpeedComparisonResult> speedComparison;// Speed-dependent error data
	std::vector<MultiPassResult> multiPass;            // Multi-pass consistency data
	std::vector<SeekTimeResult> seekTimes;             // Drive seek latency data

	int overallScore = 0;           // Composite quality score (0–100)
	std::string overallRating;      // Letter grade: A, B, C, D, F, or INCOMPLETE
	std::string summary;            // Human-readable summary paragraph
};

// ── Subchannel burn status result ───────────────────────────────────────────
// Determines whether subchannel data was actually mastered/burned onto the
// disc, or is empty filler.  Useful for deciding if subchannel extraction
// during ripping is worthwhile.
struct SubchannelBurnResult {
	bool complete = false; // Raw sample coverage and CRC validity sufficient for assessment
	bool formattedQVerified = false; // Timing fallback does not measure optional R-W content
	int totalSampled = 0;           // Total sectors sampled
	int readFailures = 0;           // Sectors where ReadSector failed entirely
	int validQCrc = 0;              // Sectors with valid Q-channel CRC-16
	int invalidQCrc = 0;            // Sectors with invalid / absent Q CRC
	int emptySubchannel = 0;        // Sectors where all 96 subchannel bytes are zero
	int rwDataPresent = 0;          // Sectors with non-zero R-W channel data (CD-G, etc.)
	int cdgPacketsFound = 0;        // Sectors with valid CD-G command (0x09) in pack structure
	int validMsfTiming = 0;         // Sectors with correctly incrementing MSF addresses
	int pChannelCorrect = 0;        // Sectors with expected P-channel state (pause/play)
	double qCrcValidPercent = 0.0;  // Percentage of CRC-tested sectors with valid Q CRC
	bool subchannelBurned = false;  // Substantial R-W content observed in a completed sample assessment
	std::string verdict;            // Human-readable summary of the result
	WORD mediaProfile = 0;          // SCSI media profile code (0x0008=CD-ROM, 0x0009=CD-R, etc.)
	std::string mediaTypeName;      // Human-readable media type ("CD-ROM", "CD-R", "CD-RW")
};
