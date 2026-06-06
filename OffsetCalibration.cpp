// ============================================================================
// OffsetCalibration.cpp - Automatic offset detection implementation
// ============================================================================
#define NOMINMAX  // Prevent Windows.h from defining min/max macros
#include "OffsetCalibration.h"
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <Windows.h>
#include <winhttp.h>
#include <map>
#include <vector>

#pragma comment(lib, "winhttp.lib")

constexpr int OffsetCalibration::CommonOffsets[];

uint32_t OffsetCalibration::CalculateAccurateRipCRC(
	const std::vector<int16_t>& samples,
	int trackNumber,
	int totalTracks) {

	uint32_t crc = 0;
	uint32_t mult = 1;

	// Skip first/last 5 frames for first/last tracks
	size_t startSample = (trackNumber == 1) ? 5 * 588 * 2 : 0;
	size_t endSample = samples.size();
	if (trackNumber == totalTracks) {
		endSample = std::max(endSample, static_cast<size_t>(5 * 588 * 2)) - 5 * 588 * 2;
	}

	for (size_t i = startSample; i < endSample; i += 2) {
		uint32_t sample = static_cast<uint16_t>(samples[i]) |
			(static_cast<uint16_t>(samples[i + 1]) << 16);
		crc += sample * mult;
		mult++;
	}

	return crc;
}

CalibrationResult OffsetCalibration::QuickCalibrate(
	std::function<void(int progress, const std::string& status)> progressCallback) {

	CalibrationResult result;

	if (progressCallback) {
		progressCallback(0, "Reading disc TOC...");
	}

	// Get disc ID and fetch AccurateRip checksums
	std::string discId = CalculateDiscId();
	if (discId.empty()) {
		if (progressCallback) {
			progressCallback(100, "Failed to read disc TOC");
		}
		result.success = false;
		return result;
	}

	auto arChecksums = FetchAccurateRipChecksums(discId);
	if (arChecksums.empty()) {
		if (progressCallback) {
			progressCallback(100, "Disc not found in AccurateRip database");
		}
		result.success = false;
		return result;
	}

	result.totalTracks = static_cast<int>(arChecksums.size());

	// Test offset range (typical CD drive offsets: -1200 to +1200 samples)
	const int MIN_OFFSET = -1200;
	const int MAX_OFFSET = 1200;
	const int STEP = 6;  // Test every 6 samples for speed

	std::map<int, int> offsetScores;  // offset -> matching tracks
	std::map<int, std::vector<int>> offsetMatchedTracks;  // Track which tracks matched

	int testsTotal = (MAX_OFFSET - MIN_OFFSET) / STEP;
	int testsDone = 0;

	for (int testOffset = MIN_OFFSET; testOffset <= MAX_OFFSET; testOffset += STEP) {
		if (progressCallback) {
			int pct = 10 + (testsDone * 80 / testsTotal);
			progressCallback(pct, "Testing offset: " + std::to_string(testOffset));
		}

		int matches = 0;
		std::vector<int> matchedTracks;

		// Read all tracks with this offset and calculate CRCs
		for (size_t i = 0; i < arChecksums.size(); i++) {
			uint32_t calculatedCRC = ReadTrackAndCalculateCRC(static_cast<int>(i + 1), testOffset);
			if (calculatedCRC == arChecksums[i]) {
				matches++;
				matchedTracks.push_back(static_cast<int>(i + 1));
			}
		}

		offsetScores[testOffset] = matches;
		if (matches > 0) {
			offsetMatchedTracks[testOffset] = matchedTracks;
		}

		testsDone++;
	}

	// Find best offset(s)
	int bestMatches = 0;
	std::vector<int> candidateOffsets;

	for (const auto& [offset, matches] : offsetScores) {
		if (matches > bestMatches) {
			bestMatches = matches;
			candidateOffsets.clear();
			candidateOffsets.push_back(offset);
		}
		else if (matches == bestMatches && matches > 0) {
			candidateOffsets.push_back(offset);
		}
	}

	// Calculate confidence based on percentage of tracks matched
	result.matchingTracks = bestMatches;
	result.confidence = result.totalTracks > 0 ? (bestMatches * 100) / result.totalTracks : 0;

	// If multiple offsets have same score, pick the one closest to zero
	if (!candidateOffsets.empty()) {
		result.detectedOffset = *std::min_element(candidateOffsets.begin(),
			candidateOffsets.end(),
			[](int a, int b) { return std::abs(a) < std::abs(b); });

		result.success = (result.confidence >= 50);  // Need at least 50% match confidence
	}
	else {
		result.success = false;
	}

	if (progressCallback) {
		progressCallback(100, "Calibration complete");
	}

	return result;
}

std::string OffsetCalibration::CalculateDiscId() {
	// Read TOC from drive to calculate AccurateRip disc IDs

	// Read TOC using READ TOC command
	BYTE tocCdb[10] = { 0x43, 0x00, 0, 0, 0, 0, 0, 0x03, 0x24, 0 };
	std::vector<BYTE> tocBuf(804);

	if (!m_drive.SendSCSI(tocCdb, 10, tocBuf.data(), 804)) {
		return "";
	}

	int tocLen = (tocBuf[0] << 8) | tocBuf[1];
	if (tocLen < 2) return "";

	int firstTrack = tocBuf[2];
	int lastTrack = tocBuf[3];
	int trackCount = lastTrack - firstTrack + 1;
	if (trackCount <= 0 || trackCount > 99) return "";

	// Calculate AccurateRip IDs
	uint32_t discId1 = 0;
	uint32_t discId2 = 0;
	uint32_t cddbId = 0;

	int cddbSum = 0;

	for (int i = 0; i < trackCount; i++) {
		BYTE* td = tocBuf.data() + 4 + i * 8;
		DWORD startLBA = (static_cast<DWORD>(td[4]) << 24) |
			(static_cast<DWORD>(td[5]) << 16) |
			(static_cast<DWORD>(td[6]) << 8) |
			static_cast<DWORD>(td[7]);

		int offset = static_cast<int>(startLBA) + 150;
		discId1 += static_cast<uint32_t>(offset);
		discId2 += static_cast<uint32_t>(offset) * (i + 1);

		// CDDB checksum
		int seconds = offset / 75;
		while (seconds > 0) {
			cddbSum += seconds % 10;
			seconds /= 10;
		}
	}

	// Get lead-out
	BYTE* leadOut = tocBuf.data() + 4 + trackCount * 8;
	DWORD leadOutLBA = (static_cast<DWORD>(leadOut[4]) << 24) |
		(static_cast<DWORD>(leadOut[5]) << 16) |
		(static_cast<DWORD>(leadOut[6]) << 8) |
		static_cast<DWORD>(leadOut[7]);

	discId2 += static_cast<uint32_t>(leadOutLBA + 150) * (trackCount + 1);

	// Calculate CDDB ID
	BYTE* firstTd = tocBuf.data() + 4;
	DWORD firstLBA = (static_cast<DWORD>(firstTd[4]) << 24) |
		(static_cast<DWORD>(firstTd[5]) << 16) |
		(static_cast<DWORD>(firstTd[6]) << 8) |
		static_cast<DWORD>(firstTd[7]);

	int totalSeconds = static_cast<int>((leadOutLBA - firstLBA) / 75);
	cddbId = ((cddbSum % 255) << 24) | (totalSeconds << 8) | trackCount;

	// Format: trackcount-discid1-discid2-cddbid
	std::ostringstream ss;
	ss << std::setfill('0') << std::dec << std::setw(3) << trackCount << "-"

		<< std::hex << std::setw(8) << discId1 << "-"
		<< std::setw(8) << discId2 << "-"
		<< std::setw(8) << cddbId;

	return ss.str();
}

std::vector<uint32_t> OffsetCalibration::FetchAccurateRipChecksums(const std::string& discId) {
	std::vector<uint32_t> checksums;

	if (discId.length() < 30) return checksums;

	// Parse disc ID: "trackcount-discid1-discid2-cddbid"
	int trackCount = 0;
	uint32_t discId1 = 0, discId2 = 0, cddbId = 0;

	if (sscanf_s(discId.c_str(), "%d-%x-%x-%x", &trackCount, &discId1, &discId2, &cddbId) != 4) {
		return checksums;
	}

	// Build URL path
	char url[256];
	snprintf(url, sizeof(url),
		"/accuraterip/%x/%x/%x/dBAR-%03d-%08x-%08x-%08x.bin",
		discId1 & 0xF, (discId1 >> 4) & 0xF, (discId1 >> 8) & 0xF,
		trackCount, discId1, discId2, cddbId);

	// HTTP request
	HINTERNET hSession = WinHttpOpen(L"OffsetCalibration/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
	if (!hSession) return checksums;

	WinHttpSetTimeouts(hSession, 5000, 10000, 5000, 10000);

	HINTERNET hConnect = WinHttpConnect(hSession, L"www.accuraterip.com",
		INTERNET_DEFAULT_HTTP_PORT, 0);
	if (!hConnect) {
		WinHttpCloseHandle(hSession);
		return checksums;
	}

	wchar_t wUrl[256];
	MultiByteToWideChar(CP_UTF8, 0, url, -1, wUrl, 256);

	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wUrl, nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);

	if (hRequest && WinHttpSendRequest(hRequest, nullptr, 0, nullptr, 0, 0, 0) &&
		WinHttpReceiveResponse(hRequest, nullptr)) {

		DWORD statusCode = 0, statusSize = sizeof(statusCode);
		WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			nullptr, &statusCode, &statusSize, nullptr);

		if (statusCode == 200) {
			// Read response data
			std::vector<BYTE> data;
			DWORD bytesAvailable = 0;

			while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
				std::vector<BYTE> buffer(bytesAvailable);
				DWORD bytesRead = 0;
				if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
					data.insert(data.end(), buffer.begin(), buffer.begin() + bytesRead);
				}
			}

			// Parse AccurateRip response format:
			// Header: 1 byte track count, 4 bytes disc ID 1, 4 bytes disc ID 2, 4 bytes CDDB ID
			// Then per track:
			//   v1: 1 byte confidence + 4 bytes CRC                = 5 bytes/track
			//   v2: 1 byte confidence + 4 bytes CRC v1 + 4 bytes CRC v2 = 9 bytes/track
			// Multiple chunks (pressings) may be concatenated.

			const size_t headerSize = 13;  // 1 + 4 + 4 + 4
			const size_t v1PerTrack = 5;   // 1 confidence + 4 CRC
			const size_t v2PerTrack = 9;   // 1 confidence + 4 CRC v1 + 4 CRC v2
			size_t v1ChunkSize = headerSize + trackCount * v1PerTrack;
			size_t v2ChunkSize = headerSize + trackCount * v2PerTrack;

			// Determine per-track size from the total data length
			size_t perTrack = 0;
			if (data.size() >= v2ChunkSize && data.size() % v2ChunkSize == 0) {
				perTrack = v2PerTrack;
			}
			else if (data.size() >= v1ChunkSize && data.size() % v1ChunkSize == 0) {
				perTrack = v1PerTrack;
			}
			else if (data.size() >= v2ChunkSize) {
				perTrack = v2PerTrack;  // Fallback: assume v2
			}
			else if (data.size() >= v1ChunkSize) {
				perTrack = v1PerTrack;
			}

			if (perTrack > 0) {
				size_t offset = headerSize;
				checksums.resize(trackCount);

				for (int t = 0; t < trackCount && offset + perTrack <= data.size(); t++) {
					// Skip confidence byte
					offset++;
					// Read CRC v1 (little-endian)
					checksums[t] = static_cast<uint32_t>(data[offset]) |
						(static_cast<uint32_t>(data[offset + 1]) << 8) |
						(static_cast<uint32_t>(data[offset + 2]) << 16) |
						(static_cast<uint32_t>(data[offset + 3]) << 24);
					offset += 4;
					// Skip CRC v2 only if this is a v2 response
					if (perTrack == v2PerTrack) {
						offset += 4;
					}
				}
			}
		}
	}

	if (hRequest) WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	return checksums;
}

CalibrationResult OffsetCalibration::CalibrateWithDisc(
	std::function<void(int progress, const std::string& status)> progressCallback) {

	// Full calibration tests all offsets from -2000 to +2000
	// This is slower but more thorough for unknown drives

	CalibrationResult result;

	// First try quick calibration with common offsets
	result = QuickCalibrate(progressCallback);
	if (result.success && result.confidence >= 80) {
		return result;
	}

	// If quick calibration failed, do full sweep
	if (progressCallback) {
		progressCallback(0, "Quick calibration inconclusive, performing full sweep...");
	}

	// Get disc ID and checksums for full sweep
	std::string discId = CalculateDiscId();
	if (discId.empty()) {
		result.success = false;
		return result;
	}

	auto arChecksums = FetchAccurateRipChecksums(discId);
	if (arChecksums.empty()) {
		result.success = false;
		return result;
	}

	result.totalTracks = static_cast<int>(arChecksums.size());
	int bestMatches = 0;
	int bestOffset = 0;

	// Test offsets -2000 to +2000 in steps of 1
	// This is slow but thorough
	for (int offset = -2000; offset <= 2000; offset++) {
		if (progressCallback && (offset % 100 == 0)) {
			int pct = ((offset + 2000) * 100) / 4000;
			progressCallback(pct, "Testing offset " + std::to_string(offset) + "...");
		}

		int matches = 0;

		// Test this offset against AccurateRip checksums
		for (size_t i = 0; i < arChecksums.size(); i++) {
			uint32_t calculatedCRC = ReadTrackAndCalculateCRC(static_cast<int>(i + 1), offset);
			if (calculatedCRC != 0 && calculatedCRC == arChecksums[i]) {
				matches++;
			}
		}

		if (matches > bestMatches) {
			bestMatches = matches;
			bestOffset = offset;
		}
	}

	result.detectedOffset = bestOffset;
	result.matchingTracks = bestMatches;
	result.confidence = result.totalTracks > 0 ? (bestMatches * 100) / result.totalTracks : 0;
	result.success = bestMatches > 0;

	if (progressCallback) {
		progressCallback(100, "Full calibration complete: offset " + std::to_string(bestOffset));
	}

	return result;
}

uint32_t OffsetCalibration::ReadTrackAndCalculateCRC(int trackNumber, int offsetSamples) {
	// Read TOC to get track boundaries
	BYTE tocCdb[10] = { 0x43, 0x00, 0, 0, 0, 0, 0, 0x03, 0x24, 0 };
	std::vector<BYTE> tocBuf(804);
	if (!m_drive.SendSCSI(tocCdb, 10, tocBuf.data(), 804)) return 0;

	int firstTrack = tocBuf[2];
	int lastTrack = tocBuf[3];
	int totalTracks = lastTrack - firstTrack + 1;
	if (trackNumber < 1 || trackNumber > totalTracks) return 0;

	// Parse track start/end LBAs
	BYTE* trackDesc = tocBuf.data() + 4 + ((trackNumber - 1) * 8);
	DWORD startLBA = (static_cast<DWORD>(trackDesc[4]) << 24) |
		(static_cast<DWORD>(trackDesc[5]) << 16) |
		(static_cast<DWORD>(trackDesc[6]) << 8) |
		static_cast<DWORD>(trackDesc[7]);

	// Get next track or lead-out for end boundary
	BYTE* nextDesc = tocBuf.data() + 4 + (trackNumber * 8);
	DWORD endLBA = (static_cast<DWORD>(nextDesc[4]) << 24) |
		(static_cast<DWORD>(nextDesc[5]) << 16) |
		(static_cast<DWORD>(nextDesc[6]) << 8) |
		static_cast<DWORD>(nextDesc[7]);

	if (endLBA <= startLBA) return 0;

	// Determine extra sectors needed to accommodate the offset.
	// Each sector holds 588 stereo sample-pairs = 1176 int16_t values.
	// offsetSamples is in stereo pairs, so the byte shift is offsetSamples * 4.
	int extraSectorsBefore = 0;
	int extraSectorsAfter = 0;
	if (offsetSamples > 0) {
		extraSectorsAfter = (offsetSamples / 588) + 1;
	}
	else if (offsetSamples < 0) {
		extraSectorsBefore = ((-offsetSamples) / 588) + 1;
	}

	DWORD readStart = startLBA;
	if (static_cast<int>(readStart) > extraSectorsBefore)
		readStart -= extraSectorsBefore;
	else
		readStart = 0;

	DWORD readEnd = endLBA + extraSectorsAfter;

	// Read all sectors into a contiguous sample buffer
	std::vector<int16_t> allSamples;
	allSamples.reserve(static_cast<size_t>(readEnd - readStart) * 588 * 2);

	BYTE sectorBuf[AUDIO_SECTOR_SIZE];
	for (DWORD lba = readStart; lba < readEnd; lba++) {
		if (!m_drive.ReadSectorAudioOnly(lba, sectorBuf)) {
			// Pad with silence on read failure (e.g. overread boundaries)
			for (int s = 0; s < AUDIO_SECTOR_SIZE / 2; s++)
				allSamples.push_back(0);
			continue;
		}
		// Convert little-endian bytes to int16_t samples
		for (int i = 0; i < AUDIO_SECTOR_SIZE; i += 2) {
			int16_t sample = static_cast<int16_t>(
				sectorBuf[i] | (sectorBuf[i + 1] << 8));
			allSamples.push_back(sample);
		}
	}

	// The nominal track data starts at this index within allSamples
	// (each sector = 588 * 2 int16_t values)
	size_t nominalStart = static_cast<size_t>(startLBA - readStart) * 588 * 2;
	size_t nominalLength = static_cast<size_t>(endLBA - startLBA) * 588 * 2;

	// Apply the offset: shift the extraction window by offsetSamples stereo pairs
	int sampleShift = offsetSamples * 2;  // stereo pair = 2 int16_t values
	ptrdiff_t windowStart = static_cast<ptrdiff_t>(nominalStart) + sampleShift;

	if (windowStart < 0 ||
		static_cast<size_t>(windowStart) + nominalLength > allSamples.size()) {
		return 0;  // Offset too large for the available data
	}

	std::vector<int16_t> trackSamples(
		allSamples.begin() + windowStart,
		allSamples.begin() + windowStart + static_cast<ptrdiff_t>(nominalLength));

	return CalculateAccurateRipCRC(trackSamples, trackNumber, totalTracks);
}