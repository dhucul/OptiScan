// ============================================================================
// AccurateRip.cpp - AccurateRip implementation
// ============================================================================
#include "AccurateRip.h"
#include <winhttp.h>
#include <iostream>
#include <iomanip>

#pragma comment(lib, "winhttp.lib")

// Helper: get the lead-out LBA for AccurateRip purposes.
// For enhanced/multisession CDs, use the audio session's lead-out.
static DWORD GetAudioLeadOut(const DiscInfo& disc) {
	return (disc.audioLeadOutLBA != 0) ? disc.audioLeadOutLBA : disc.leadOutLBA;
}

// Helper: count audio tracks only.
static int CountAudioTracks(const DiscInfo& disc) {
	int count = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) count++;
	}
	return count;
}

// Helper: the track's ORIGINAL TOC start LBA, before OptiScan's internal
// clamps. AccurateRip disc IDs must reflect the disc's physical TOC geometry,
// but TrackInfo::startLBA is mutated for the ripper/verifier — the Red-Book
// INDEX-01 floor (ReadTOC forces track 1 to LBA 150 when its detected INDEX 01
// is < 150) and copy-protection TOC repair both rewrite it. Using the clamped
// value shifts the disc ID and breaks the database lookup: e.g. a track-1 start
// of 32 clamped to 150 moves both Disc ID 1 and Disc ID 2 by +118, turning a
// FOUND disc into NOT FOUND. ReadFullTOC snapshots the pre-clamp LBAs into
// rawTocEntries; use those, falling back to the live value when no snapshot
// exists (e.g. a TOC-less scan).
static DWORD TocStartLBA(const DiscInfo& disc, const TrackInfo& t) {
	for (const auto& raw : disc.rawTocEntries) {
		if (raw.trackNumber == t.trackNumber) return raw.originalStartLBA;
	}
	return t.startLBA;
}

uint32_t AccurateRip::CalculateCRC(const std::vector<std::vector<BYTE>>& sectors,
	int trackNum, int totalTracks, DWORD trackStartLBA) {
	uint32_t crc = 0;
	uint32_t mult = 1;  // Always start at 1 for each track

	// AccurateRip V1: Skip first/last 5 sectors (2940 samples) of DISC
	// For first track: skip first 5 sectors
	// For last track: skip last 5 sectors
	size_t skipStartSectors = (trackNum == 1) ? 5 : 0;
	size_t skipEndSectors = (trackNum == totalTracks) ? 5 : 0;

	for (size_t i = 0; i < sectors.size(); i++) {
		const BYTE* data = sectors[i].data();

		for (int j = 0; j < AUDIO_SECTOR_SIZE; j += 4) {
			uint32_t sample = static_cast<uint32_t>(data[j]) |
				(static_cast<uint32_t>(data[j + 1]) << 8) |
				(static_cast<uint32_t>(data[j + 2]) << 16) |
				(static_cast<uint32_t>(data[j + 3]) << 24);

			// Skip first 5 sectors of first track, last 5 sectors of last track
			bool skip = (trackNum == 1 && i < skipStartSectors) ||
				(trackNum == totalTracks && i >= sectors.size() - skipEndSectors);

			if (!skip) {
				crc += sample * mult;
			}
			mult++;  // Always increment — mult is the 1-based sample position in the track
		}
	}
	return crc;
}

uint32_t AccurateRip::CalculateDiscID1(const DiscInfo& disc) {
	uint32_t id = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) id += TocStartLBA(disc, t);
	}
	id += GetAudioLeadOut(disc);
	return id;
}

uint32_t AccurateRip::CalculateDiscID2(const DiscInfo& disc) {
	uint32_t id = 0;
	int audioTrackNum = 0;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		audioTrackNum++;
		DWORD startLBA = TocStartLBA(disc, t);
		uint32_t frameNum = (startLBA == 0) ? 1 : startLBA;
		id += frameNum * static_cast<uint32_t>(audioTrackNum);
	}
	id += GetAudioLeadOut(disc) * static_cast<uint32_t>(audioTrackNum + 1);
	return id;
}

uint32_t AccurateRip::CalculateCDDBID(const DiscInfo& disc) {
	auto sumDigits = [](int n) {
		int sum = 0;
		while (n > 0) { sum += n % 10; n /= 10; }
		return sum;
		};

	uint32_t n = 0;
	DWORD firstAudioLBA = 0;
	bool firstFound = false;
	int audioTrackCount = 0;

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD startLBA = TocStartLBA(disc, t);
		int seconds = (startLBA + 150) / 75;
		n += sumDigits(seconds);
		audioTrackCount++;
		if (!firstFound) { firstAudioLBA = startLBA; firstFound = true; }
	}

	DWORD leadOut = GetAudioLeadOut(disc);
	int leadOutSec = (leadOut + 150) / 75;
	int firstTrackSec = (firstAudioLBA + 150) / 75;
	int totalSec = leadOutSec - firstTrackSec;

	return ((n % 0xFF) << 24) | (totalSec << 8) | static_cast<uint32_t>(audioTrackCount);
}

// Add this helper function before Lookup()
static bool IsInternetAvailable() {
	HINTERNET hSession = WinHttpOpen(L"ConnectionTest",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
	if (!hSession) return false;

	HINTERNET hConnect = WinHttpConnect(hSession, L"www.accuraterip.com",
		INTERNET_DEFAULT_HTTP_PORT, 0);

	bool available = (hConnect != nullptr);

	if (hConnect) WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	return available;
}

bool AccurateRip::Lookup(DiscInfo& disc, std::vector<std::vector<uint32_t>>& pressingCRCs) {
	pressingCRCs.clear();

	if (!IsInternetAvailable()) {
		std::cout << "  SKIPPED: No internet connection available\n";
		return false;
	}

	uint32_t discId1 = CalculateDiscID1(disc);
	uint32_t discId2 = CalculateDiscID2(disc);
	uint32_t cddbId = CalculateCDDBID(disc);
	int trackCount = CountAudioTracks(disc);

	char url[256];
	snprintf(url, sizeof(url),
		"/accuraterip/%x/%x/%x/dBAR-%03d-%08x-%08x-%08x.bin",
		discId1 & 0xF, (discId1 >> 4) & 0xF, (discId1 >> 8) & 0xF,
		trackCount, discId1, discId2, cddbId);

	std::cout << "AccurateRip lookup...\n";
	std::cout << "  Disc ID 1: " << std::hex << std::setfill('0')
		<< std::setw(8) << discId1 << std::dec << std::setfill(' ') << "\n";
	std::cout << "  Disc ID 2: " << std::hex << std::setfill('0')
		<< std::setw(8) << discId2 << std::dec << std::setfill(' ') << "\n";
	std::cout << "  CDDB ID:   " << std::hex << std::setfill('0')
		<< std::setw(8) << cddbId << std::dec << std::setfill(' ') << "\n";

	HINTERNET hSession = WinHttpOpen(L"OptiScan/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
	if (!hSession) {
		std::cout << "  SKIPPED: Failed to initialize HTTP\n";
		return false;
	}

	WinHttpSetTimeouts(hSession, 5000, 10000, 5000, 10000);

	HINTERNET hConnect = WinHttpConnect(hSession, L"www.accuraterip.com",
		INTERNET_DEFAULT_HTTP_PORT, 0);
	if (!hConnect) {
		WinHttpCloseHandle(hSession);
		std::cout << "  SKIPPED: Cannot connect to AccurateRip\n";
		return false;
	}

	wchar_t wUrl[256];
	MultiByteToWideChar(CP_UTF8, 0, url, -1, wUrl, 256);

	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wUrl, nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);

	bool found = false;
	if (hRequest && WinHttpSendRequest(hRequest, nullptr, 0, nullptr, 0, 0, 0) &&
		WinHttpReceiveResponse(hRequest, nullptr)) {

		DWORD statusCode = 0, statusSize = sizeof(statusCode);
		WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			nullptr, &statusCode, &statusSize, nullptr);

		if (statusCode == 200) {
			found = true;
			std::cout << "  FOUND in AccurateRip database!\n";

			std::vector<BYTE> data;
			DWORD bytesAvailable = 0;
			while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
				std::vector<BYTE> buffer(bytesAvailable);
				DWORD bytesRead = 0;
				if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
					data.insert(data.end(), buffer.begin(), buffer.begin() + bytesRead);
				}
			}

			const size_t headerSize = 13;
			const size_t v1PerTrack = 5;
			const size_t v2PerTrack = 9;
			size_t v1ChunkSize = headerSize + trackCount * v1PerTrack;
			size_t v2ChunkSize = headerSize + trackCount * v2PerTrack;

			size_t perTrack = 0;
			size_t chunkSize = 0;
			if (data.size() >= v2ChunkSize && data.size() % v2ChunkSize == 0) {
				perTrack = v2PerTrack;
				chunkSize = v2ChunkSize;
			}
			else if (data.size() >= v1ChunkSize && data.size() % v1ChunkSize == 0) {
				perTrack = v1PerTrack;
				chunkSize = v1ChunkSize;
			}
			else if (data.size() >= v2ChunkSize) {
				perTrack = v2PerTrack;
				chunkSize = v2ChunkSize;
			}
			else if (data.size() >= v1ChunkSize) {
				perTrack = v1PerTrack;
				chunkSize = v1ChunkSize;
			}

			if (perTrack > 0 && chunkSize > 0) {
				// Parse ALL pressings (chunks) in the response
				size_t chunkOffset = 0;
				while (chunkOffset + chunkSize <= data.size()) {
					std::vector<uint32_t> pressing(trackCount, 0);
					size_t pos = chunkOffset + headerSize;

					for (int t = 0; t < trackCount && pos + perTrack <= chunkOffset + chunkSize; t++) {
						pos++;  // skip confidence byte
						pressing[t] = static_cast<uint32_t>(data[pos]) |
							(static_cast<uint32_t>(data[pos + 1]) << 8) |
							(static_cast<uint32_t>(data[pos + 2]) << 16) |
							(static_cast<uint32_t>(data[pos + 3]) << 24);
						pos += 4;
						if (perTrack == v2PerTrack) {
							pos += 4;  // skip CRC v2
						}
					}

					pressingCRCs.push_back(std::move(pressing));
					chunkOffset += chunkSize;
				}
				std::cout << "  Found " << pressingCRCs.size() << " pressing(s)\n";
			}
		}
		else if (statusCode == 404) {
			std::cout << "  NOT FOUND in AccurateRip database\n";
		}
	}

	if (hRequest) WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	return found;
}

bool AccurateRip::VerifyCRCs(const DiscInfo& disc, const std::vector<std::vector<uint32_t>>& pressingCRCs) {
	std::cout << "\n=== AccurateRip CRC Verification ===\n";
	bool allMatch = true;
	int audioTrackIdx = 0;
	int totalAudioTracks = CountAudioTracks(disc);

	// Build a flat-offset table: for each track, compute where its startLBA
	// data begins inside the rawSectors array.  This avoids fragile sequential
	// index tracking that breaks when AccurateRip boundaries don't match the
	// stored (possibly pregap-trimmed) boundaries.
	std::vector<size_t> trackDataOffset(disc.tracks.size());
	size_t cumulative = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		DWORD readStart = (disc.pregapMode == PregapMode::Skip)
			? disc.tracks[i].startLBA
			: disc.tracks[i].pregapLBA;
		trackDataOffset[i] = cumulative + (disc.tracks[i].startLBA - readStart);
		cumulative += disc.tracks[i].endLBA - readStart + 1;
	}

	DWORD audioLeadOut = GetAudioLeadOut(disc);

	for (size_t i = 0; i < disc.tracks.size(); i++) {
		const auto& t = disc.tracks[i];
		if (!t.isAudio) continue;

		// AccurateRip defines track boundaries by the original TOC:
		//   startLBA  →  next audio track's startLBA - 1   (or audio lead-out - 1 for last audio track)
		// The stored endLBA may have been trimmed by pregap scanning, so we
		// reconstruct the original boundary here.
		// For enhanced CDs, use the audio session lead-out instead of the
		// data track's startLBA.
		DWORD originalEndLBA = audioLeadOut - 1;  // default: last audio track
		for (size_t j = i + 1; j < disc.tracks.size(); j++) {
			if (disc.tracks[j].isAudio) {
				originalEndLBA = disc.tracks[j].startLBA - 1;
				break;
			}
		}
		DWORD arSectorCount = originalEndLBA - t.startLBA + 1;

		// Use absolute offset — sectors are contiguous in rawSectors because
		// each track's readStart == previous track's endLBA + 1.
		size_t baseIdx = trackDataOffset[i];
		std::vector<std::vector<BYTE>> trackSectors;
		for (DWORD j = 0; j < arSectorCount && baseIdx + j < disc.rawSectors.size(); j++) {
			std::vector<BYTE> audioOnly(AUDIO_SECTOR_SIZE);
			memcpy(audioOnly.data(), disc.rawSectors[baseIdx + j].data(), AUDIO_SECTOR_SIZE);
			trackSectors.push_back(std::move(audioOnly));
		}

		uint32_t crc = CalculateCRC(trackSectors, audioTrackIdx + 1,
			totalAudioTracks, t.startLBA);

		// Check calculated CRC against ALL pressings
		bool match = false;
		int matchedPressing = -1;
		for (size_t p = 0; p < pressingCRCs.size(); p++) {
			if (audioTrackIdx < static_cast<int>(pressingCRCs[p].size()) &&
				crc == pressingCRCs[p][audioTrackIdx]) {
				match = true;
				matchedPressing = static_cast<int>(p) + 1;
				break;
			}
		}

		std::cout << "Track " << std::setw(2) << t.trackNumber << ": CRC = "
			<< std::hex << std::setw(8) << std::setfill('0') << crc
			<< std::dec << std::setfill(' ');

		if (pressingCRCs.empty()) {
			std::cout << "  [NO REFERENCE]\n";
		}
		else if (match) {
			std::cout << "  [OK - pressing #" << matchedPressing << "]\n";
		}
		else {
			std::cout << "  [MISMATCH]\n";
			allMatch = false;
		}

		audioTrackIdx++;
	}

	return allMatch;
}