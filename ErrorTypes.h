// ============================================================================
// ErrorTypes.h - Error tracking and statistics structures
// ============================================================================
#pragma once

#include <windows.h>
#include <vector>
#include <string>

// ── Contiguous run of errors ────────────────────────────────────────────────
// Represents a consecutive block of sectors that all had read errors.
struct ErrorCluster {
	DWORD startLBA = 0;              // First bad sector
	DWORD endLBA = 0;               // Last bad sector
	int errorCount = 0;             // Total errors within the cluster
	int size() const { return static_cast<int>(endLBA - startLBA + 1); }
};

// ── Error distribution across disc zones ────────────────────────────────────
// Splits the disc into three radial zones (inner / middle / outer) and counts
// errors in each.  Useful for detecting edge-concentrated damage.
struct DiscZoneStats {
	int innerErrors = 0;      // Errors in the inner 0–33% of the disc
	int middleErrors = 0;     // Errors in the middle 33–66%
	int outerErrors = 0;      // Errors in the outer 66–100%
	int innerSectors = 0;     // Total sectors in each zone
	int middleSectors = 0;
	int outerSectors = 0;

	// Convenience: per-zone error rate as a percentage.
	double InnerErrorRate() const { return innerSectors > 0 ? (double)innerErrors / innerSectors * 100.0 : 0; }
	double MiddleErrorRate() const { return middleSectors > 0 ? (double)middleErrors / middleSectors * 100.0 : 0; }
	double OuterErrorRate() const { return outerSectors > 0 ? (double)outerErrors / outerSectors * 100.0 : 0; }
};

// ── Extended SCSI error info for a single sector ────────────────────────────
// Captures the full SCSI sense data (key / ASC / ASCQ) returned when a read
// fails, enabling precise error classification.
struct SectorError {
	DWORD lba = 0;                  // Sector address
	int trackNumber = 0;            // Track the sector belongs to
	DWORD scsiSenseKey = 0;         // SCSI sense key (0x03 = medium error, etc.)
	DWORD scsiASC = 0;              // Additional Sense Code
	DWORD scsiASCQ = 0;             // Additional Sense Code Qualifier
	int retryCount = 0;             // Number of retries before giving up
	std::string errorType;          // Classified label: "READ_ERROR", "MEDIUM_ERROR", etc.
	double readTimeMs = 0;          // Time spent on this sector's read attempts
};

// ── Enhanced error statistics ───────────────────────────────────────────────
// Disc-wide error counters, statistical measures, and cluster analysis.
struct ErrorStatistics {
	int totalReadErrors = 0;
	int totalSeekErrors = 0;
	int totalMediumErrors = 0;
	int totalTimeoutErrors = 0;
	int totalCRCErrors = 0;
	int recoveredErrors = 0;               // Errors the drive corrected internally
	int unrecoverableErrors = 0;           // Errors that could not be corrected
	std::vector<SectorError> detailedErrors;// Full SCSI sense data per failed sector

	double errorRatePer1000 = 0.0;         // Errors per 1000 sectors
	int peakErrorBurst = 0;                // Longest consecutive error run
	DWORD peakBurstStartLBA = 0;           // Where that burst started

	// Statistical measures
	double avgErrorsPerSector = 0.0;
	double errorStdDeviation = 0.0;
	double errorDensity = 0.0;             // Errors per megabyte of disc data
	std::vector<int> errorDistribution;    // Histogram of error counts (bucket per sector range)

	bool hasClusteredErrors = false;       // true if errors form spatial clusters
	int clusterCount = 0;
	std::vector<ErrorCluster> errorClusters;
};

// ── C2 per-sector error classification ──────────────────────────────────────
// Used during C2 scans to classify each sector's error status.
struct C2SectorError {
	DWORD lba = 0;
	int c2Errors = 0;       // -1 = total read failure, 0 = clean, >0 = error count
	BYTE senseKey = 0x00;   // SCSI sense key: 0x00 = OK, 0x01 = recovered, 0x03 = medium error
	BYTE asc = 0x00;        // Additional Sense Code
	BYTE ascq = 0x00;       // Additional Sense Code Qualifier

	bool IsRecovered() const { return c2Errors > 0 && senseKey == 0x01; }    // Drive corrected the error
	bool IsUnrecoverable() const { return c2Errors < 0 || senseKey == 0x03; }// Unrecoverable read error
	bool IsClean() const { return c2Errors == 0; }                            // No errors at all
};

// ── CRC verification result ─────────────────────────────────────────────────
// Compares a locally calculated CRC against an expected value (e.g. from
// AccurateRip) and lists any sectors where the data diverged.
struct CRCVerification {
	uint32_t calculatedCRC = 0;              // CRC computed from ripped data
	uint32_t expectedCRC = 0;               // CRC from the AccurateRip database
	bool matches = false;                    // true if they are equal
	std::vector<DWORD> mismatchedSectors;    // Sectors that contributed to the mismatch
};