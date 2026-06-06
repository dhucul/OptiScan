// ============================================================================
// ScanResults.h - Disc quality scanning results
// ============================================================================
#pragma once

#include "ErrorTypes.h"
#include "AnalysisTypes.h"
#include <windows.h>
#include <vector>
#include <string>
#include <utility>

// ── Per-second sample from hardware quality scan ────────────────────────────
struct QCheckSample {
	DWORD lba = 0;          // Approximate LBA at this time slice
	int c1 = 0;             // C1 (BLER) error count for this second
	int c2 = 0;             // Verified C2 error count for this second
	int cu = 0;             // CU (uncorrectable) count for this second
	int pioneerE22 = 0;     // Pioneer vendor E22 diagnostic count, not counted as C2
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
	bool supported = false;                    // True if a scan method was available
	std::string scanMethod;                    // E.g. "Plextor Q-Check (0xE9/0xEB)"
	DWORD totalSectors = 0;                    // Disc sectors covered
	DWORD totalSeconds = 0;                    // Scan duration in time slices

	// Aggregate C1 statistics
	int totalC1 = 0;
	double avgC1PerSecond = 0.0;
	int maxC1PerSecond = 0;
	int maxC1SecondIndex = -1;

	// Aggregate C2 statistics
	int totalC2 = 0;
	double avgC2PerSecond = 0.0;
	int maxC2PerSecond = 0;
	int maxC2SecondIndex = -1;

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

	// Per-second time-series data
	std::vector<QCheckSample> samples;

	// Tracks affected by C2 / CU errors, computed from samples + TOC after the
	// scan completes. Sorted by track number; entries with both counts == 0
	// are omitted.
	std::vector<QCheckTrackErrors> errorTracks;

	// Quality assessment
	std::string qualityRating;                 // EXCELLENT / GOOD / FAIR / POOR / BAD

	// Total C1 quality interpretation
	std::string totalC1Quality;                // Exceptional / Very good / Normal / Marginal / Poor

	// Archival audio peak C1 assessment
	std::string archivalC1Rating;              // Ideal / Good / Acceptable / Poor

	// Pioneer E22 diagnostic rating (diagnostic only — not a copy trigger)
	std::string pioneerE22Rating;              // Ideal / Good / Acceptable / Concerning

	// Drive compatibility warning — zero C1 across entire disc suggests
	// the drive accepted the vendor scan command but does not actually
	// populate CIRC error statistics.  Not all drives that pass the
	// initial capability probe produce valid measurement data.
	bool c1Unverified = false;
};

// ── Single jitter/beta time-slice from a LiteOn jitter scan ─────────────────
struct JitterSample {
	DWORD lba = 0;          // Approximate LBA at this time slice
	int jitter = 0;         // Raw jitter value reported by drive
	int beta = 0;           // Signed beta (pit/land asymmetry); ~0 ideal
};

// ── Jitter / beta scan result (LiteOn 0xDF/0x1B vendor command) ─────────────
struct JitterResult {
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

// ── BLER (Block Error Rate) scan result ─────────────────────────────────────
// Captures the output of a detailed error-rate scan.  BLER measures raw error
// frequency before ECC correction.  Red Book spec: < 220 errors/second avg.
struct BlerResult {
	DWORD totalSectors = 0;
	int totalSeconds = 0;

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

	// C1 error statistics
	int totalC1Errors = 0;
	int totalC1Sectors = 0;
	int maxC1InSingleSector = 0;
	DWORD worstC1SectorLBA = 0;
	double avgC1PerSecond = 0.0;
	int maxC1PerSecond = 0;
	DWORD worstC1SecondLBA = 0;

	// C1 utilization / C2 margin analysis
	double c1UtilizationPct = 0.0;         // Avg C1 rate as % of Red Book 220/sec limit
	double peakC1UtilizationPct = 0.0;     // Peak C1 rate as % of 220/sec limit
	int c2MarginScore = 100;               // 0-100: how close C1 is to exhausting C2 margin
	std::string c2MarginLabel;             // WIDE / ADEQUATE / NARROW / CRITICAL / EXHAUSTED

	// Cluster and pattern analysis
	int largestClusterSize = 0;
	bool hasEdgeConcentration = false;
	bool hasProgressivePattern = false;
	bool hasC1Data = false;
	bool c2Unverified = false;             // C2 bitmap may not be functional
	std::string qualityRating;

	// Per-second time-series data: (LBA, error count) per 75-sector bucket
	std::vector<std::pair<DWORD, int>> perSecondC2;
	std::vector<std::pair<DWORD, int>> perSecondC1;

	// Error clusters and zone distribution
	std::vector<ErrorCluster> errorClusters;
	DiscZoneStats zoneStats;

	// Top worst sectors by C2 error count: (LBA, C2 count)
	std::vector<std::pair<DWORD, int>> topWorstC2Sectors;
};

// ── Disc rot analysis results ───────────────────────────────────────────────
// Output of the disc rot detection scan.  Combines zone statistics, error
// cluster data, and heuristic indicators for various rot patterns.
struct DiscRotAnalysis {
	DiscZoneStats zones;                        // Error distribution by radial zone
	std::vector<ErrorCluster> clusters;          // Contiguous error regions
	int inconsistentSectors = 0;                // Sectors that read differently on re-read
	int totalRereadTests = 0;                   // Number of re-read comparison tests run
	double inconsistencyRate = 0.0;             // inconsistentSectors / totalRereadTests
	int maxC2InSingleSector = 0;                // Worst C2 count in a single sector
	int pioneerE22Total = 0;                    // Pioneer vendor diagnostic E22, not counted as C2
	double pioneerE22AvgPerSecond = 0.0;        // Average Pioneer E22 diagnostic count in Phase 0
	int pioneerE22Peak = 0;                     // Peak Pioneer E22 diagnostic count in Phase 0
	std::string pioneerE22Rating;               // Ideal / Good / Acceptable / Concerning

	// Heuristic disc-rot pattern flags
	bool edgeConcentration = false;             // Errors concentrated at inner/outer edges
	bool progressivePattern = false;            // Error rate increases toward the outer edge
	bool pinholePattern = false;                // Many small scattered clusters (pinhole corrosion)
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
	std::string overallRating;      // Letter grade: A, B, C, D, or F
	std::string summary;            // Human-readable summary paragraph
};

// ── Subchannel burn status result ───────────────────────────────────────────
// Determines whether subchannel data was actually mastered/burned onto the
// disc, or is empty filler.  Useful for deciding if subchannel extraction
// during ripping is worthwhile.
struct SubchannelBurnResult {
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
	bool subchannelBurned = false;  // Overall verdict: true = subchannel data is present
	std::string verdict;            // Human-readable summary of the result
	WORD mediaProfile = 0;          // SCSI media profile code (0x0008=CD-ROM, 0x0009=CD-R, etc.)
	std::string mediaTypeName;      // Human-readable media type ("CD-ROM", "CD-R", "CD-RW")
};
