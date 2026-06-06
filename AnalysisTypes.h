// ============================================================================
// AnalysisTypes.h - Analysis and testing result structures
// ============================================================================
#pragma once

#include <windows.h>
#include <vector>

// ── Speed comparison test result ────────────────────────────────────────────
// One entry per tested LBA.  Compares C2 error counts at low vs. high speed
// to detect surface degradation that is speed-sensitive.
struct SpeedComparisonResult {
	DWORD lba = 0;               // Tested sector address
	int lowSpeedC2 = 0;          // C2 errors at low speed
	int highSpeedC2 = 0;         // C2 errors at high speed
	bool inconsistent = false;   // true if results differ significantly
};

// ── Multi-pass verification result ──────────────────────────────────────────
// Per-sector result from reading the disc N times and comparing hashes.
struct MultiPassResult {
	DWORD lba = 0;               // Sector address
	int passesMatched = 0;       // Number of passes that returned the same data
	int totalPasses = 0;         // Total number of read passes
	bool allMatch = false;       // true if every pass returned identical data
	uint32_t majorityHash = 0;   // Hash of the most-common read result
};

// ── Seek time analysis result ───────────────────────────────────────────────
// Measures the drive's mechanical seek latency between two LBAs.
struct SeekTimeResult {
	DWORD fromLBA = 0;           // Source sector
	DWORD toLBA = 0;             // Destination sector
	double seekTimeMs = 0;       // Measured seek time in milliseconds
	bool abnormal = false;       // true if latency exceeds expected range
};

// ── Audio content analysis result ───────────────────────────────────────────
// Flags sectors with anomalous audio content (silence, clipping, DC offset).
struct AudioAnalysisResult {
	int silentSectors = 0;          // Sectors containing all zeros
	int clippedSectors = 0;         // Sectors with samples at max amplitude (0x7FFF / 0x8000)
	int lowLevelSectors = 0;        // Suspiciously quiet sectors (below a threshold)
	int dcOffsetSectors = 0;        // Sectors with a measurable DC bias
	std::vector<DWORD> suspiciousLBAs; // LBAs flagged for further inspection
};