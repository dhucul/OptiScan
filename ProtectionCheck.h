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
// Informational observations do not contribute to the protection verdict.
inline void FinalizeProtectionAssessment(ProtectionCheckResult& result) {
	result.detectedCount = 0;
	int strongCount = 0;
	int weakCount = 0;
	for (const auto& ind : result.indicators) {
		if (ind.detected && ind.severity > 0) {
			result.detectedCount++;
			if (ind.severity >= 2)
				strongCount++;
			else
				weakCount++;
		}
	}

	if (result.detectedCount == 0) {
		result.protectionLikely = false;
		result.verdict = "No copy-protection indicators detected.";
		result.protectionType = "None";
	}
	else if (strongCount == 0) {
		// Only weak/informational indicators — not enough to claim protection.
		result.protectionLikely = false;
		result.verdict = std::to_string(weakCount) +
			" minor anomaly(ies) found - unlikely to be copy-protection.";
		result.protectionType = "None (minor anomalies)";
	}
	else if (strongCount == 1 && weakCount == 0) {
		result.protectionLikely = false;
		result.verdict = "One strong indicator found but no corroborating evidence - "
			"possible copy-protection, but not conclusive.";
		result.protectionType = "Inconclusive";
	}
	// ── Fix #6: require >= 2 strong, or >= 1 strong + >= 2 weak ─────────
	// Previously any strongCount >= 1 with weakCount > 0 triggered
	// "protection likely", which was too aggressive (e.g. one strong
	// indicator from a long disc + one weak from CD-Extra layout).
	else if (strongCount >= 2 || (strongCount >= 1 && weakCount >= 2)) {
		// Sufficient corroborating evidence for a positive verdict.
		result.protectionLikely = true;

		// Try to identify specific scheme from combination of indicators.
		bool hasDataTrack = false, hasErrors = false, hasMultiSession = false;
		bool hasSubchManip = false, hasTocIllegal = false;
		for (const auto& ind : result.indicators) {
			if (!ind.detected || ind.severity == 0) continue;
			if (ind.name == "Data Track Present") hasDataTrack = true;
			if (ind.name == "Intentional Errors") hasErrors = true;
			if (ind.name == "Multi-Session Abuse") hasMultiSession = true;
			if (ind.name == "Subchannel Manipulation") hasSubchManip = true;
			if (ind.name == "Illegal TOC") hasTocIllegal = true;
		}

		if (hasDataTrack && hasMultiSession)
			result.protectionType = "MediaMax / XCP-style (data session + multi-session)";
		else if (hasErrors && hasTocIllegal)
			result.protectionType = "Cactus Data Shield / Key2Audio-style (errors + illegal TOC)";
		else if (hasErrors)
			result.protectionType = "Intentional-error based (CDS / MediaClyS)";
		else if (hasSubchManip)
			result.protectionType = "Subchannel-based protection";
		else
			result.protectionType = "Unknown / custom scheme";

		result.verdict = "Multiple protection indicators detected - disc is likely copy-protected.";
	}
	else {
		// strongCount == 1 with only 1 weak indicator — not conclusive.
		result.protectionLikely = false;
		result.verdict = std::to_string(strongCount) + " strong and " +
			std::to_string(weakCount) +
			" weak indicator(s) found - insufficient evidence for copy-protection.";
		result.protectionType = "Inconclusive";
	}

}
