#define NOMINMAX
#include "OpticalDrive.h"
#include <iostream>
#include <fstream>
#include <algorithm>

// ============================================================================
// Comprehensive Disc Quality Scan - Orchestrates All Quality Tests
// ============================================================================

bool OpticalDrive::RunComprehensiveScan(DiscInfo& disc, ComprehensiveScanResult& result, int speed) {
	std::cout << "\n=== COMPREHENSIVE DISC QUALITY SCAN ===\n";
	std::cout << "This will run all available quality tests on the disc.\n";
	std::cout << "Estimated time: 15-30 minutes depending on disc condition.\n\n";
	
	result = ComprehensiveScanResult{};

	// Test 1: BLER Scan (C2 error distribution)
	std::cout << "\n[1/5] Running BLER quality scan...\n";
	if (!RunBlerScan(disc, result.bler, speed)) {
		std::cout << "BLER scan failed or was cancelled.\n";
		return false;
	}

	// Test 2: Disc Rot Detection
	std::cout << "\n[2/5] Running disc rot detection...\n";
	if (!RunDiscRotScan(disc, result.rot, speed)) {
		std::cout << "Disc rot scan failed or was cancelled.\n";
		return false;
	}

	// Test 3: Speed Comparison
	std::cout << "\n[3/5] Running speed comparison test...\n";
	if (!RunSpeedComparisonTest(disc, result.speedComparison)) {
		std::cout << "Speed comparison test failed or was cancelled.\n";
		return false;
	}

	// Test 4: Multi-Pass Verification
	std::cout << "\n[4/5] Running multi-pass verification (3 passes)...\n";
	if (!RunMultiPassVerification(disc, result.multiPass, 3, speed)) {
		std::cout << "Multi-pass verification failed or was cancelled.\n";
		return false;
	}

	// Test 5: Audio Content Analysis
	std::cout << "\n[5/5] Running audio content analysis...\n";
	if (!AnalyzeAudioContent(disc, result.audio, speed)) {
		std::cout << "Audio content analysis failed or was cancelled.\n";
		return false;
	}

	// Calculate overall score
	result.overallScore = CalculateOverallScore(result);
	
	if (result.overallScore >= 90) result.overallRating = "A";
	else if (result.overallScore >= 80) result.overallRating = "B";
	else if (result.overallScore >= 70) result.overallRating = "C";
	else if (result.overallScore >= 60) result.overallRating = "D";
	else result.overallRating = "F";

	result.summary = "Comprehensive scan complete. Overall score: " + std::to_string(result.overallScore) + "/100";

	PrintComprehensiveReport(result);
	return true;
}

int OpticalDrive::CalculateOverallScore(const ComprehensiveScanResult& result) {
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

	return std::max(0, std::min(100, score));
}

void OpticalDrive::PrintComprehensiveReport(const ComprehensiveScanResult& result) {
	std::cout << "\n" << std::string(70, '=') << "\n";
	std::cout << "                 COMPREHENSIVE QUALITY REPORT\n";
	std::cout << std::string(70, '=') << "\n";

	std::cout << "\n--- Overall Assessment ---\n";
	std::cout << "  Score:  " << result.overallScore << "/100\n";
	std::cout << "  Grade:  " << result.overallRating << "\n";

	// BLER Summary
	std::cout << "\n--- BLER Quality ---\n";
	std::cout << "  Rating:           " << result.bler.qualityRating << "\n";
	std::cout << "  Total C2 errors:  " << result.bler.totalC2Errors << "\n";
	std::cout << "  C2 sectors:       " << result.bler.totalC2Sectors << "\n";
	std::cout << "  Read failures:    " << result.bler.totalReadFailures << "\n";
	std::cout << "  Avg C2/sec:       " << std::fixed << std::setprecision(2) 
		<< result.bler.avgC2PerSecond << "\n";

	// Disc Rot Summary
	std::cout << "\n--- Disc Rot Analysis ---\n";
	std::cout << "  Risk Level:        " << result.rot.rotRiskLevel << "\n";
	std::cout << "  Edge conc.:        " << (result.rot.edgeConcentration ? "YES" : "NO") << "\n";
	std::cout << "  Progressive:       " << (result.rot.progressivePattern ? "YES" : "NO") << "\n";
	std::cout << "  Instability:       " << (result.rot.readInstability ? "YES" : "NO") 
		<< " (" << std::fixed << std::setprecision(1) << result.rot.inconsistencyRate << "%)\n";
	std::cout << "  Error clusters:    " << result.rot.clusters.size() << "\n";

	// Speed Comparison Summary
	std::cout << "\n--- Speed Stability ---\n";
	if (!result.speedComparison.empty()) {
		int inconsistentCount = 0;
		int lowErrors = 0, highErrors = 0;
		for (const auto& r : result.speedComparison) {
			if (r.inconsistent) inconsistentCount++;
			if (r.lowSpeedC2 > 0) lowErrors += r.lowSpeedC2;
			if (r.highSpeedC2 > 0) highErrors += r.highSpeedC2;
		}
		std::cout << "  Sectors tested:    " << result.speedComparison.size() << "\n";
		std::cout << "  Inconsistent:      " << inconsistentCount << "\n";
		std::cout << "  Low speed errors:  " << lowErrors << "\n";
		std::cout << "  High speed errors: " << highErrors << "\n";
	}
	else {
		std::cout << "  (not tested)\n";
	}

	// Multi-Pass Summary
	std::cout << "\n--- Read Consistency ---\n";
	if (!result.multiPass.empty()) {
		int perfect = 0, partial = 0, failed = 0;
		for (const auto& r : result.multiPass) {
			if (r.allMatch) perfect++;
			else if (r.passesMatched >= (r.totalPasses + 1) / 2) partial++;
			else failed++;
		}
		std::cout << "  Perfect matches:   " << perfect << "\n";
		std::cout << "  Partial matches:   " << partial << "\n";
		std::cout << "  Failed/Inconsist.: " << failed << "\n";
	}
	else {
		std::cout << "  (not tested)\n";
	}

	// Audio Content Summary
	std::cout << "\n--- Audio Content ---\n";
	std::cout << "  Silent sectors:    " << result.audio.silentSectors << "\n";
	std::cout << "  Clipped sectors:   " << result.audio.clippedSectors << "\n";
	std::cout << "  Low-level sectors: " << result.audio.lowLevelSectors << "\n";
	std::cout << "  DC offset sectors: " << result.audio.dcOffsetSectors << "\n";
	std::cout << "  Suspicious areas:  " << result.audio.suspiciousLBAs.size() << "\n";

	// Final Recommendation
	std::cout << "\n--- Recommendation ---\n";
	if (result.overallScore >= 90) {
		std::cout << "  Disc is in EXCELLENT condition.\n";
		std::cout << "  Any rip mode will produce perfect results.\n";
	}
	else if (result.overallScore >= 80) {
		std::cout << "  Disc is in GOOD condition.\n";
		std::cout << "  Standard or Secure rip mode recommended.\n";
	}
	else if (result.overallScore >= 70) {
		std::cout << "  Disc shows MODERATE wear.\n";
		std::cout << "  Use Secure rip mode for best results.\n";
	}
	else if (result.overallScore >= 60) {
		std::cout << "  Disc shows SIGNIFICANT degradation.\n";
		std::cout << "  Use Paranoid rip mode. Consider disc cleaning.\n";
	}
	else {
		std::cout << "  Disc is in POOR condition.\n";
		std::cout << "  Use Paranoid rip mode. Data loss possible.\n";
		std::cout << "  Back up immediately if this disc is irreplaceable.\n";
	}

	if (!result.rot.recommendation.empty()) {
		std::cout << "  " << result.rot.recommendation << "\n";
	}

	std::cout << "\n" << std::string(70, '=') << "\n";
}

bool OpticalDrive::SaveComprehensiveReport(const ComprehensiveScanResult& result, const std::wstring& filename) {
	std::ofstream file(filename);
	if (!file) return false;

	file << "COMPREHENSIVE DISC QUALITY REPORT\n";
	file << "==================================\n\n";

	file << "Overall Assessment\n";
	file << "------------------\n";
	file << "Score: " << result.overallScore << "/100\n";
	file << "Grade: " << result.overallRating << "\n\n";

	file << "BLER Quality\n";
	file << "------------\n";
	file << "Rating:          " << result.bler.qualityRating << "\n";
	file << "Total C2 errors: " << result.bler.totalC2Errors << "\n";
	file << "C2 sectors:      " << result.bler.totalC2Sectors << "\n";
	file << "Read failures:   " << result.bler.totalReadFailures << "\n";
	file << "Avg C2/sec:      " << result.bler.avgC2PerSecond << "\n\n";

	file << "Disc Rot Analysis\n";
	file << "-----------------\n";
	file << "Risk Level:      " << result.rot.rotRiskLevel << "\n";
	file << "Edge conc.:      " << (result.rot.edgeConcentration ? "YES" : "NO") << "\n";
	file << "Progressive:     " << (result.rot.progressivePattern ? "YES" : "NO") << "\n";
	file << "Instability:     " << (result.rot.readInstability ? "YES" : "NO") 
		<< " (" << result.rot.inconsistencyRate << "%)\n";
	file << "Error clusters:  " << result.rot.clusters.size() << "\n\n";

	file << "Audio Content\n";
	file << "-------------\n";
	file << "Silent sectors:  " << result.audio.silentSectors << "\n";
	file << "Clipped sectors: " << result.audio.clippedSectors << "\n";
	file << "Low-level:       " << result.audio.lowLevelSectors << "\n";
	file << "DC offset:       " << result.audio.dcOffsetSectors << "\n\n";

	file << "Recommendation\n";
	file << "--------------\n";
	if (!result.rot.recommendation.empty()) {
		file << result.rot.recommendation << "\n";
	}

	file.close();
	return true;
}