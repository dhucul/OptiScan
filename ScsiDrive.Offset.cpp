// ============================================================================
// ScsiDrive.Offset.cpp - Drive offset detection
// ============================================================================
#include "ScsiDrive.h"
#include "DriveOffsetDatabase.h"
#include "OffsetCalibration.h"
#include <cctype>
#include <cmath>

bool ScsiDrive::DetectDriveOffset(OffsetDetectionResult& result) {
	DriveOffsetInfo dbInfo;
	if (LookupAccurateRipOffset(dbInfo)) {
		result.offset = dbInfo.readOffset;
		result.confidence = 95;
		result.method = OffsetDetectionMethod::Database;
		result.details = dbInfo.source;
		return true;
	}

	DriveCapabilities caps{};
	bool hasCaps = DetectCapabilities(caps);
	if (hasCaps && caps.supportsAccurateStream) {
		result.details += "Drive supports Accurate Stream. ";
	}

	if (hasCaps && caps.mediaPresent) {
		OffsetCalibration calibration(*this);
		CalibrationResult calResult = calibration.QuickCalibrate(nullptr);

		if (calResult.success && calResult.confidence >= 70) {
			result.offset = calResult.detectedOffset;
			result.confidence = calResult.confidence;
			result.method = OffsetDetectionMethod::AccurateRipCalibration;
			result.details = "Auto-calibrated using AccurateRip (" +
				std::to_string(calResult.matchingTracks) + "/" +
				std::to_string(calResult.totalTracks) + " tracks matched)";
			return true;
		}

		int pregapOffset = 0;
		if (DetectOffsetFromPregap(0, pregapOffset)) {
			result.offset = pregapOffset;
			result.confidence = 50;
			result.method = OffsetDetectionMethod::PregapAnalysis;
			result.details = "Estimated from pregap analysis (less reliable)";
			return true;
		}
	}

	result.offset = 0;
	result.confidence = 0;
	result.method = OffsetDetectionMethod::Unknown;
	result.details = "Unknown drive - insert a disc from AccurateRip database for auto-calibration";
	return false;
}

bool ScsiDrive::LookupAccurateRipOffset(DriveOffsetInfo& info) {
	std::string vendor, model;
	if (!GetDriveInfo(vendor, model)) return false;

	// Single lookup path through the runtime database.
	// Load() falls back to built-in knownOffsets[] if download/cache unavailable,
	// so Lookup() always has data to search.
	auto& db = DriveOffsetDatabase::Instance();
	db.Load();
	return db.Lookup(vendor, model, info);
}

bool ScsiDrive::DetectOffsetFromPregap(int trackStartLBA, int& estimatedOffset) {
	constexpr int SCAN_RANGE = 20;
	constexpr int SILENCE_THRESHOLD = 16;
	constexpr int MAX_REASONABLE_OFFSET = 1200;

	int transitionPoint = 0;
	bool foundSilence = false;
	bool foundAudio = false;

	for (int i = -SCAN_RANGE; i < SCAN_RANGE; i++) {
		BYTE sector[AUDIO_SECTOR_SIZE];
		if (!ReadSectorAudioOnly(trackStartLBA + i, sector)) continue;

		int16_t* samples = reinterpret_cast<int16_t*>(sector);
		int silentSamples = 0;

		for (int s = 0; s < AUDIO_SECTOR_SIZE / 2; s++) {
			if (std::abs(samples[s]) < SILENCE_THRESHOLD) silentSamples++;
		}

		bool isSilent = silentSamples > (AUDIO_SECTOR_SIZE / 4);

		if (isSilent && !foundSilence) {
			foundSilence = true;
		}
		else if (!isSilent && foundSilence && !foundAudio) {
			foundAudio = true;
			transitionPoint = i * 588;
		}
	}

	if (foundSilence && foundAudio && std::abs(transitionPoint) <= MAX_REASONABLE_OFFSET) {
		estimatedOffset = transitionPoint;
		return true;
	}

	estimatedOffset = 0;
	return false;
}