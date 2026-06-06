// ============================================================================
// ScsiTypes.h - SCSI drive-related type definitions
// ============================================================================
#pragma once

#include <string>

// Drive offset detection structures
struct DriveOffsetInfo {
	int readOffset = 0;
	int writeOffset = 0;
	bool fromDatabase = false;
	bool supportsAccurateStream = false;
	std::string source;
};

enum class OffsetDetectionMethod {
	Unknown,
	Database,              // From AccurateRip drive database
	AccurateRipCalibration, // Auto-detected via disc verification
	PregapAnalysis,        // Estimated from pregap silence
	Manual                 // User-provided
};

struct OffsetDetectionResult {
	int offset = 0;
	int confidence = 0;  // 0-100
	OffsetDetectionMethod method = OffsetDetectionMethod::Unknown;
	std::string details;
};

enum class C2Mode {
	NotSupported,
	ErrorBlock,      // Standard C2 error block
	ErrorPointers,   // C2 error pointers (more accurate)
	PlextorD8       // Plextor vendor command
};

// Enhanced C2 reading options
struct C2ReadOptions {
	bool multiPass = false;
	int passCount = 3;
	bool defeatCache = false;
	bool countBytes = false;  // true = PlexTools-style byte counting
};