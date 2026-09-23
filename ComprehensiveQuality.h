#pragma once
#include "ScanResults.h"
#include "DiscRotQuality.h"
#include <algorithm>

// Pure final-assessment policy, shared by the scan workflow and regression tests.
namespace ComprehensiveQuality {
inline bool HasConfirmedPioneerLoss(const ComprehensiveScanResult& result) {
	return (result.bler.pioneerCdCheckRun && result.bler.pioneerCdCheckC2Bytes > 0)
		|| (result.rot.pioneerCdCheckRun && result.rot.pioneerCdCheckC2Bytes > 0);
}

inline bool HasConfirmedFailure(const ComprehensiveScanResult& result) {
	return result.bler.HasConfirmedFailure() || HasConfirmedPioneerLoss(result) ||
		DiscRot::HasConfirmedFailure(result.rot);
}

inline ScanQuality::C1Rating C1Assessment(const ComprehensiveScanResult& result) {
	return result.bler.c1.Rating();
}

inline bool IsIncomplete(const ComprehensiveScanResult& result) {
	return !HasConfirmedFailure(result) &&
		(C1Assessment(result) == ScanQuality::C1Rating::Unrated || !result.bler.CanAssessC2() ||
			DiscRot::HasLimitedReadConfidence(result.rot));
}

inline std::string MissingMeasurements(const ComprehensiveScanResult& result) {
	const bool c1Missing = C1Assessment(result) == ScanQuality::C1Rating::Unrated;
	std::string missing;
	if (c1Missing && !result.bler.CanAssessC2()) missing = "C1 and C2 were unavailable or unverified";
	else if (c1Missing) missing = "C1 was unavailable or unverified";
	else if (!result.bler.CanAssessC2()) missing = "C2 was unavailable or unverified";
	if (DiscRot::HasLimitedReadConfidence(result.rot)) {
		if (!missing.empty()) missing += "; ";
		missing += "disc rot scan read confidence was limited";
	}
	return missing;
}

inline int CalculateScore(const ComprehensiveScanResult& result) {
	int score = 100;

	// BLER/C2 errors (up to -40 points)
	if (result.bler.totalReadFailures > 0) {
		score -= std::min(40, result.bler.totalReadFailures * 10);
	}
	else if (result.bler.totalC2Errors > 0) {
		score -= std::min(30, result.bler.totalC2Errors / 100);
	}

	// Disc rot indicators (up to -45 points)
	if (result.rot.edgeConcentration) score -= 10;
	if (result.rot.progressivePattern) score -= 15;
	if (result.rot.readInstability) score -= 20;

	score -= static_cast<int>(result.rot.inconsistencyRate * 2);

	// Risk-level and Pioneer CD Check outcomes are final-state evidence, not
	// cosmetic report strings. Apply caps so confirmed data loss or a high rot
	// verdict cannot coexist with a high overall score.
	if (result.rot.rotRiskLevel == "CRITICAL") score = std::min(score, 20);
	else if (result.rot.rotRiskLevel == "HIGH") score = std::min(score, 40);
	else if (result.rot.rotRiskLevel == "MODERATE") score = std::min(score, 65);

	if (HasConfirmedPioneerLoss(result))
		score = std::min(score, 35);

	// Speed stability (up to -10 points)
	int speedInconsistent = 0;
	for (const auto& r : result.speedComparison) {
		if (r.inconsistent) speedInconsistent++;
	}
	if (!result.speedComparison.empty()) {
		double inconsistencyRate = (speedInconsistent * 100.0) / result.speedComparison.size();
		score -= static_cast<int>(inconsistencyRate / 10);
	}

	// Multi-pass consistency (up to -5 points)
	int multiPassFailed = 0;
	for (const auto& r : result.multiPass) {
		if (!r.allMatch) multiPassFailed++;
	}
	if (!result.multiPass.empty()) {
		double failRate = (multiPassFailed * 100.0) / result.multiPass.size();
		score -= static_cast<int>(failRate / 20);
	}

	// A composite grade cannot outrank the measured average C1 band.
	// These caps are OptiScan policy, not standards-based certification.
	switch (C1Assessment(result)) {
	case ScanQuality::C1Rating::Excellent: break;
	case ScanQuality::C1Rating::Good: score = std::min(score, 89); break;
	case ScanQuality::C1Rating::Fair: score = std::min(score, 79); break;
	case ScanQuality::C1Rating::Poor: score = std::min(score, 59); break;
	case ScanQuality::C1Rating::Unrated: score = std::min(score, 79); break;
	}
	if (!result.bler.CanAssessC2() || DiscRot::HasLimitedReadConfidence(result.rot)) score = std::min(score, 79);
	// A recovered failure remains a warning, without claiming uncorrectable loss.
	if (result.rot.recoveredReadFailures > 0) score = std::min(score, 89);
	// Missing channels cannot turn known failures into an incomplete/clean grade.
	if (HasConfirmedFailure(result)) score = std::min(score, 59);

	return std::max(0, std::min(100, score));
}

inline void Finalize(ComprehensiveScanResult& result) {
	// Recompute every derived field; a reused result cannot retain an older
	// incomplete or successful assessment after its evidence changes.
	result.overallScore = CalculateScore(result);
	if (HasConfirmedFailure(result)) result.overallRating = "F";
	else if (IsIncomplete(result)) result.overallRating = "INCOMPLETE";
	else if (result.overallScore >= 90) result.overallRating = "A";
	else if (result.overallScore >= 80) result.overallRating = "B";
	else if (result.overallScore >= 70) result.overallRating = "C";
	else if (result.overallScore >= 60) result.overallRating = "D";
	else result.overallRating = "F";
	result.summary = "Comprehensive scan complete. Overall score: " +
		std::to_string(result.overallScore) + "/100";
	if (HasConfirmedFailure(result)) result.summary += " (confirmed read failure or data loss)";
	else if (IsIncomplete(result)) result.summary += " (incomplete: " + MissingMeasurements(result) + ")";
}
} // namespace ComprehensiveQuality
