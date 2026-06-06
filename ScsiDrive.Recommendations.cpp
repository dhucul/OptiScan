// ============================================================================
// ScsiDrive.Recommendations.cpp - User-friendly drive recommendations
// ============================================================================
#include "ScsiDrive.h"
#include "DriveTypes.h"
#include "ConsoleColor.h"
#include "ConsoleFormat.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

// ── ASCII-safe symbols for console output ───────────────────────────────────
static constexpr const char* SYM_STAR = "*";   // was \u2605 (★)
static constexpr const char* SYM_CHECK = "+";   // was \u2713 (✓)
static constexpr const char* SYM_WARN = "!";   // was \u26A0 (⚠)
static constexpr const char* SYM_INFO = "i";   // was \u2139 (ℹ)
static constexpr const char* SYM_BULL = "-";   // was \u2022 (•)

// ── Helper: Normalize vendor/model strings ──────────────────────────────────
static void NormalizeString(std::string& s) {
	// Trim leading/trailing whitespace
	size_t start = s.find_first_not_of(" \t\r\n");
	size_t end = s.find_last_not_of(" \t\r\n");
	if (start == std::string::npos) {
		s.clear();
		return;
	}
	s = s.substr(start, end - start + 1);

	// Convert to uppercase
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return std::toupper(c); });
}

DriveTier ScsiDrive::GetDriveTier(std::string* outFamilyName) const {
	// Curated wins: any exact knownDriveConfigs hit beats a family match.
	DriveSpecificConfig cfg;
	if (GetDriveSpecificConfig(cfg)) {
		return DriveTier::Curated;
	}

	std::string vendor, model;
	const_cast<ScsiDrive*>(this)->GetDriveInfo(vendor, model);
	NormalizeString(vendor);
	NormalizeString(model);

	for (const auto& fam : knownDriveFamilies) {
		std::string fv = fam.vendorPattern;
		std::string fm = fam.modelPattern;
		NormalizeString(fv);
		NormalizeString(fm);
		if (vendor == fv && model.find(fm) != std::string::npos) {
			if (outFamilyName) *outFamilyName = fam.familyName;
			return DriveTier::KnownGoodFamily;
		}
	}
	return DriveTier::Unknown;
}

bool ScsiDrive::GetDriveSpecificConfig(DriveSpecificConfig& config) const {
	std::string vendor, model;
	const_cast<ScsiDrive*>(this)->GetDriveInfo(const_cast<std::string&>(vendor),
		const_cast<std::string&>(model));

	// Normalize strings (trim, uppercase)
	NormalizeString(vendor);
	NormalizeString(model);

	// Search known configurations
	for (const auto& knownConfig : knownDriveConfigs) {
		std::string knownVendor = knownConfig.vendor;
		std::string knownModel = knownConfig.model;
		NormalizeString(knownVendor);
		NormalizeString(knownModel);

		// Match vendor exactly and model with substring match
		if (vendor == knownVendor && model.find(knownModel) != std::string::npos) {
			config = knownConfig;
			return true;
		}
	}

	return false;
}

void ScsiDrive::DisplayDriveRecommendations() const {
	DriveSpecificConfig config;
	if (!GetDriveSpecificConfig(config)) {
		return; // No specific recommendations for this drive
	}

	std::string vendor, model;
	const_cast<ScsiDrive*>(this)->GetDriveInfo(const_cast<std::string&>(vendor),
		const_cast<std::string&>(model));

	std::cout << "\n" << std::string(70, '=') << "\n";
	Console::SetColor(Console::Color::Green);
	std::cout << "  OPTIMAL SETTINGS FOR YOUR DRIVE\n";
	Console::Reset();
	std::cout << std::string(70, '=') << "\n";

	std::cout << "\n  Drive: " << vendor << " " << model << "\n\n";

	// ── Burst Mode Recommendation ──
	if (config.optimizedForBurst && config.forceAccurateStream) {
		Console::SetColor(Console::Color::Yellow);
		std::cout << "  " << SYM_STAR << " BURST MODE HIGHLY RECOMMENDED " << SYM_STAR << "\n\n";
		Console::Reset();

		std::cout << "  Why Burst Mode is optimal for this drive:\n\n";

		std::cout << "  " << SYM_CHECK << " Accurate Stream Support\n";
		std::cout << "    Your drive guarantees bit-perfect audio extraction with zero\n";
		std::cout << "    jitter. This means burst mode is completely safe and accurate.\n\n";

		if (config.cacheBehaviorPredictable) {
			std::cout << "  " << SYM_CHECK << " Intelligent Cache Management\n";
			std::cout << "    The drive's internal buffer is designed for sequential reads.\n";
			std::cout << "    Multi-sector reads are MORE reliable than single-sector reads.\n\n";
		}

		if (config.burstReadSize > 0) {
			std::cout << "  " << SYM_CHECK << " Optimized Batch Size: " << config.burstReadSize << " sectors\n";
			std::cout << "    Reading " << config.burstReadSize << " sectors at once minimizes command overhead\n";
			std::cout << "    and allows the drive to work at peak efficiency.\n\n";
		}

		std::cout << "  Performance Comparison:\n";
		std::cout << "    Single-sector mode: ~8-12 MB/s   (lots of USB/SCSI overhead)\n";
		std::cout << "    Burst mode:         ~18-24 MB/s  (near maximum 24x speed)\n\n";

		Console::SetColor(Console::Color::Cyan);
		std::cout << "  RECOMMENDED: Use option '1. Copy disc' with Burst mode\n";
		Console::Reset();
	}
	else if (config.forceAccurateStream && !config.optimizedForBurst) {
		std::cout << "  " << SYM_CHECK << " Accurate Stream Support\n";
		std::cout << "    Your drive supports accurate streaming, meaning standard mode\n";
		std::cout << "    provides reliable extraction without excessive re-reads.\n\n";

		if (config.hasJitterIssues) {
			Console::SetColor(Console::Color::Yellow);
			std::cout << "  " << SYM_WARN << " Known Jitter Issues\n";
			Console::Reset();
			std::cout << "    This drive may produce slight jitter at high speeds.\n";
			std::cout << "    Consider using Secure mode for archival-quality rips.\n\n";
		}
	}

	// ── Read Speed Recommendation ──
	if (config.recommendedReadSpeed > 0) {
		int speedMultiplier = config.recommendedReadSpeed / 176;
		std::cout << "  Recommended Read Speed: " << speedMultiplier << "x ("
			<< config.recommendedReadSpeed << " KB/s)\n";

		if (config.limitSpeedForAudio) {
			std::cout << "    This drive produces better quality at reduced speeds.\n";
			std::cout << "    Maximum speed may introduce errors on some discs.\n\n";
		}
		else {
			std::cout << "    This drive maintains excellent quality at full speed.\n\n";
		}
	}

	// ── C2 Error Reporting ──
	if (config.forceC2ErrorReporting) {
		std::cout << "  " << SYM_CHECK << " C2 Error Reporting: Recommended\n";
		std::cout << "    Your drive has reliable C2 error detection. Use Comprehensive\n";
		std::cout << "    or Paranoid mode for damaged/scratched discs.\n\n";
	}
	else if (config.disableC2ErrorReporting) {
		Console::SetColor(Console::Color::Yellow);
		std::cout << "  " << SYM_WARN << " C2 Error Reporting: NOT Recommended\n";
		Console::Reset();
		std::cout << "    This drive's C2 implementation is unreliable. Standard or\n";
		std::cout << "    Secure mode will produce better results.\n\n";
	}

	// ── Offset Correction ──
	if (config.readOffset != 0) {
		std::cout << "  Read Offset: " << (config.readOffset > 0 ? "+" : "")
			<< config.readOffset << " samples\n";
		std::cout << "    This offset is verified and will be automatically applied\n";
		std::cout << "    for AccurateRip matching.\n\n";
	}

	// ── Special Features ──
	if (config.supportsOverreadLeadIn && config.supportsOverreadLeadOut) {
		std::cout << "  " << SYM_CHECK << " Lead-In/Out Overread\n";
		std::cout << "    Your drive can extract hidden tracks in pregap areas.\n\n";
	}

	// ── Quirks & Warnings ──
	if (config.requiresExtraSpinup) {
		Console::SetColor(Console::Color::Yellow);
		std::cout << "  " << SYM_WARN << " Slow Spin-Up\n";
		Console::Reset();
		std::cout << "    This drive needs extra time to reach operating speed.\n";
		std::cout << "    Allow 10-15 seconds after disc insertion before ripping.\n\n";
	}

	if (config.requiresCacheFlush) {
		std::cout << "  " << SYM_INFO << " Cache Defeat Enabled\n";
		std::cout << "    This drive benefits from periodic cache-flush seeks to\n";
		std::cout << "    ensure fresh data is read from the disc.\n\n";
	}

	// ── Bottom Line ──
	std::cout << std::string(70, '-') << "\n";
	Console::SetColor(Console::Color::Green);

	if (config.optimizedForBurst && config.forceAccurateStream) {
		std::cout << "  VERDICT: Excellent drive for audio extraction!\n";
		std::cout << "           Use Burst mode for maximum speed and accuracy.\n";
	}
	else if (config.forceAccurateStream) {
		std::cout << "  VERDICT: Good drive for audio extraction.\n";
		std::cout << "           Standard or Secure mode recommended.\n";
	}
	else {
		std::cout << "  VERDICT: Capable drive for audio extraction.\n";
		std::cout << "           Secure or Paranoid mode recommended for best quality.\n";
	}

	Console::Reset();
	std::cout << std::string(70, '=') << "\n";
}

std::string ScsiDrive::GetDriveRecommendationText() const {
	DriveSpecificConfig config;
	if (!GetDriveSpecificConfig(config)) {
		return "";
	}

	std::ostringstream oss;

	if (config.optimizedForBurst && config.forceAccurateStream) {
		oss << SYM_STAR << " BURST MODE recommended (Accurate Stream + optimized buffering)";
	}
	else if (config.forceAccurateStream) {
		oss << "Standard mode recommended (Accurate Stream)";
	}
	else if (config.hasJitterIssues) {
		oss << "Secure mode recommended (jitter issues detected)";
	}
	else {
		oss << "Secure/Paranoid mode recommended for best quality";
	}

	if (config.readOffset != 0) {
		oss << " " << SYM_BULL << " Offset: " << (config.readOffset > 0 ? "+" : "") << config.readOffset;
	}

	return oss.str();
}