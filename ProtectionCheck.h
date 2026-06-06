// ============================================================================
// ProtectionCheck.h - Audio CD copy-protection detection
//
// Declares the result structure and entry-point function for scanning a disc
// for common copy-protection mechanisms (Cactus Data Shield, Key2Audio,
// intentional errors, illegal TOC entries, multi-session tricks, etc.).
// ============================================================================
#pragma once

#include "OpticalDrive.h"
#include <string>
#include <vector>

// ── Individual protection indicator ─────────────────────────────────────────
struct ProtectionIndicator {
	std::string name;               // Short name (e.g. "Illegal TOC")
	std::string description;        // Detailed explanation
	bool detected = false;          // Whether this indicator was found
	int severity = 0;               // 0 = info, 1 = warning, 2 = strong indicator
};

// ── Aggregate protection-check result ───────────────────────────────────────
struct ProtectionCheckResult {
	std::vector<ProtectionIndicator> indicators;   // All checks performed
	int detectedCount = 0;                         // How many indicators fired
	bool protectionLikely = false;                 // Overall verdict
	std::string verdict;                           // Human-readable summary
	std::string protectionType;                    // Best-guess protection name (if any)
};

// Entry point — runs every check and prints / saves results.
bool RunProtectionCheck(OpticalDrive& copier, DiscInfo& disc,
	const std::wstring& workDir, int scanSpeed);