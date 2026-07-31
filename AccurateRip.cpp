// ============================================================================
// AccurateRip.cpp - AccurateRip implementation
// ============================================================================
#include "AccurateRip.h"
#include <winhttp.h>
#include <iostream>
#include <iomanip>
#include <limits>

#pragma comment(lib, "winhttp.lib")

namespace {

constexpr size_t kFramesPerSector = AUDIO_SECTOR_SIZE / 4;
constexpr size_t kMaxAccurateRipResponseBytes = 16 * 1024 * 1024;

struct TrackCRCs {
	uint32_t v1 = 0;
	uint32_t v2 = 0;
};

bool TryCalculateTrackCRCs(
	const std::vector<std::vector<BYTE>>& sectors,
	size_t firstSector,
	size_t sectorCount,
	int trackNum,
	int totalTracks,
	TrackCRCs& result) {
	result = TrackCRCs{};
	if (trackNum < 1 || totalTracks < 1 || trackNum > totalTracks ||
		sectorCount == 0 || firstSector > sectors.size() ||
		sectorCount > sectors.size() - firstSector ||
		sectorCount > (std::numeric_limits<size_t>::max)() / kFramesPerSector) {
		return false;
	}

	for (size_t i = 0; i < sectorCount; ++i) {
		if (sectors[firstSector + i].size() < AUDIO_SECTOR_SIZE) {
			return false;
		}
	}

	uint32_t highProductSum = 0;
	uint64_t position = 1;
	size_t frameIndex = 0;
	const size_t totalFrames = sectorCount * kFramesPerSector;

	// AccurateRip excludes the first 2,939 stereo frames of track 1 and the
	// final 2,940 frames of the last track. The first included frame of track 1
	// therefore retains multiplier 2,940.
	const size_t skipStartFrames =
		(trackNum == 1) ? 5 * kFramesPerSector - 1 : 0;
	const size_t skipEndFrames =
		(trackNum == totalTracks) ? 5 * kFramesPerSector : 0;
	if (skipStartFrames > totalFrames ||
		skipEndFrames > totalFrames - skipStartFrames ||
		skipStartFrames + skipEndFrames >= totalFrames) {
		return false;
	}
	const size_t includedEnd = totalFrames - skipEndFrames;

	for (size_t sectorIndex = 0; sectorIndex < sectorCount; ++sectorIndex) {
		const BYTE* data = sectors[firstSector + sectorIndex].data();
		for (int j = 0; j < AUDIO_SECTOR_SIZE; j += 4) {
			uint32_t sample = static_cast<uint32_t>(data[j]) |
				(static_cast<uint32_t>(data[j + 1]) << 8) |
				(static_cast<uint32_t>(data[j + 2]) << 16) |
				(static_cast<uint32_t>(data[j + 3]) << 24);

			if (frameIndex >= skipStartFrames && frameIndex < includedEnd) {
				uint64_t product = static_cast<uint64_t>(sample) * position;
				result.v1 += static_cast<uint32_t>(product);
				highProductSum += static_cast<uint32_t>(product >> 32);
			}
			frameIndex++;
			position++;
		}
	}

	// ARv2 folds the high 32 bits of every weighted product into the ARv1 sum.
	result.v2 = result.v1 + highProductSum;
	return true;
}

uint32_t ReadLittleEndian32(const std::vector<BYTE>& data, size_t offset) {
	return static_cast<uint32_t>(data[offset]) |
		(static_cast<uint32_t>(data[offset + 1]) << 8) |
		(static_cast<uint32_t>(data[offset + 2]) << 16) |
		(static_cast<uint32_t>(data[offset + 3]) << 24);
}

} // namespace

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

namespace {

struct AccurateRipIds {
	uint32_t discId1 = 0;
	uint32_t discId2 = 0;
	uint32_t cddbId = 0;
	int trackCount = 0;
};

uint32_t SumDecimalDigits(uint64_t value) {
	uint32_t sum = 0;
	while (value > 0) {
		sum += static_cast<uint32_t>(value % 10);
		value /= 10;
	}
	return sum;
}

bool TryCalculateDiscIds(const DiscInfo& disc, AccurateRipIds& ids) {
	ids = AccurateRipIds{};
	const uint64_t leadOut = GetAudioLeadOut(disc);
	if (leadOut == 0) return false;

	uint64_t id1 = leadOut;
	uint64_t id2 = 0;
	uint64_t cddbDigitSum = 0;
	uint64_t firstAudioLBA = 0;
	uint64_t previousAudioLBA = 0;
	bool haveAudio = false;

	for (const auto& track : disc.tracks) {
		if (!track.isAudio) continue;

		const uint64_t startLBA = TocStartLBA(disc, track);
		if (startLBA >= leadOut ||
			(haveAudio && startLBA <= previousAudioLBA) ||
			ids.trackCount == 255) {
			return false;
		}

		ids.trackCount++;
		const uint64_t frameNumber = (startLBA == 0) ? 1 : startLBA;
		id1 += startLBA;
		id2 += frameNumber * static_cast<uint64_t>(ids.trackCount);
		cddbDigitSum += SumDecimalDigits((startLBA + 150) / 75);

		if (!haveAudio) firstAudioLBA = startLBA;
		previousAudioLBA = startLBA;
		haveAudio = true;
	}

	if (!haveAudio) return false;

	id2 += leadOut * static_cast<uint64_t>(ids.trackCount + 1);
	const uint64_t leadOutSeconds = (leadOut + 150) / 75;
	const uint64_t firstTrackSeconds = (firstAudioLBA + 150) / 75;
	if (leadOutSeconds < firstTrackSeconds) return false;

	const uint64_t durationSeconds = leadOutSeconds - firstTrackSeconds;
	if (durationSeconds > 0x00FFFFFFu) return false;

	ids.discId1 = static_cast<uint32_t>(id1);
	ids.discId2 = static_cast<uint32_t>(id2);
	ids.cddbId =
		(static_cast<uint32_t>(cddbDigitSum % 0xFF) << 24) |
		(static_cast<uint32_t>(durationSeconds) << 8) |
		static_cast<uint32_t>(ids.trackCount);
	return true;
}

} // namespace

bool AccurateRip::CalculateCRCs(
	const std::vector<std::vector<BYTE>>& sectors,
	int trackNum, int totalTracks, uint32_t& crcV1, uint32_t& crcV2) {
	TrackCRCs crcs;
	if (!TryCalculateTrackCRCs(
		sectors, 0, sectors.size(), trackNum, totalTracks, crcs)) {
		crcV1 = 0;
		crcV2 = 0;
		return false;
	}
	crcV1 = crcs.v1;
	crcV2 = crcs.v2;
	return true;
}

uint32_t AccurateRip::CalculateDiscID1(const DiscInfo& disc) {
	AccurateRipIds ids;
	return TryCalculateDiscIds(disc, ids) ? ids.discId1 : 0;
}

uint32_t AccurateRip::CalculateDiscID2(const DiscInfo& disc) {
	AccurateRipIds ids;
	return TryCalculateDiscIds(disc, ids) ? ids.discId2 : 0;
}

uint32_t AccurateRip::CalculateCDDBID(const DiscInfo& disc) {
	AccurateRipIds ids;
	return TryCalculateDiscIds(disc, ids) ? ids.cddbId : 0;
}

namespace {

class ScopedInternetHandle {
public:
	explicit ScopedInternetHandle(HINTERNET handle = nullptr) : handle_(handle) {}
	~ScopedInternetHandle() {
		if (handle_) WinHttpCloseHandle(handle_);
	}

	ScopedInternetHandle(const ScopedInternetHandle&) = delete;
	ScopedInternetHandle& operator=(const ScopedInternetHandle&) = delete;

	HINTERNET get() const { return handle_; }
	explicit operator bool() const { return handle_ != nullptr; }

private:
	HINTERNET handle_;
};

bool ResponseLayoutMatches(
	const std::vector<BYTE>& data,
	size_t perTrack,
	const AccurateRipIds& ids) {
	if (ids.trackCount <= 0 ||
		static_cast<size_t>(ids.trackCount) >
			((std::numeric_limits<size_t>::max)() - 13) / perTrack) {
		return false;
	}

	const size_t chunkSize =
		13 + static_cast<size_t>(ids.trackCount) * perTrack;
	if (data.empty() || data.size() % chunkSize != 0) return false;

	for (size_t offset = 0; offset < data.size(); offset += chunkSize) {
		if (data[offset] != static_cast<BYTE>(ids.trackCount) ||
			ReadLittleEndian32(data, offset + 1) != ids.discId1 ||
			ReadLittleEndian32(data, offset + 5) != ids.discId2 ||
			ReadLittleEndian32(data, offset + 9) != ids.cddbId) {
			return false;
		}
	}
	return true;
}

bool TryParseReferences(
	const std::vector<BYTE>& data,
	const AccurateRipIds& ids,
	std::vector<std::vector<uint32_t>>& references) {
	references.clear();

	constexpr size_t v1PerTrack = 5;
	constexpr size_t extendedPerTrack = 9;
	const bool isV1 = ResponseLayoutMatches(data, v1PerTrack, ids);
	const bool isExtended =
		ResponseLayoutMatches(data, extendedPerTrack, ids);
	if (isV1 == isExtended) return false;

	const size_t perTrack = isExtended ? extendedPerTrack : v1PerTrack;
	const size_t chunkSize =
		13 + static_cast<size_t>(ids.trackCount) * perTrack;

	for (size_t chunkOffset = 0;
		chunkOffset < data.size();
		chunkOffset += chunkSize) {
		std::vector<uint32_t> record;
		record.reserve(static_cast<size_t>(ids.trackCount));
		size_t position = chunkOffset + 13;

		for (int track = 0; track < ids.trackCount; ++track) {
			position++; // confidence byte
			record.push_back(ReadLittleEndian32(data, position));
			position += 4;
			if (isExtended) {
				// Extended records carry a secondary CRC field that is not the
				// standard per-track ARv2 checksum used for matching here.
				position += 4;
			}
		}
		references.push_back(std::move(record));
	}

	return !references.empty();
}

} // namespace

bool AccurateRip::Lookup(DiscInfo& disc, std::vector<std::vector<uint32_t>>& pressingCRCs) {
	AccurateRipIds ids;
	if (!TryCalculateDiscIds(disc, ids)) {
		std::cout << "  SKIPPED: Invalid audio TOC for AccurateRip lookup\n";
		return false;
	}

	char url[256];
	snprintf(url, sizeof(url),
		"/accuraterip/%x/%x/%x/dBAR-%03d-%08x-%08x-%08x.bin",
		ids.discId1 & 0xF, (ids.discId1 >> 4) & 0xF,
		(ids.discId1 >> 8) & 0xF, ids.trackCount,
		ids.discId1, ids.discId2, ids.cddbId);

	std::cout << "AccurateRip lookup...\n";
	std::cout << "  Disc ID 1: " << std::hex << std::setfill('0')
		<< std::setw(8) << ids.discId1 << std::dec << std::setfill(' ') << "\n";
	std::cout << "  Disc ID 2: " << std::hex << std::setfill('0')
		<< std::setw(8) << ids.discId2 << std::dec << std::setfill(' ') << "\n";
	std::cout << "  CDDB ID:   " << std::hex << std::setfill('0')
		<< std::setw(8) << ids.cddbId << std::dec << std::setfill(' ') << "\n";

	ScopedInternetHandle hSession(WinHttpOpen(
		L"OptiScan/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		nullptr, nullptr, 0));
	if (!hSession) {
		std::cout << "  SKIPPED: Failed to initialize HTTP\n";
		return false;
	}

	WinHttpSetTimeouts(hSession.get(), 5000, 10000, 5000, 10000);

	ScopedInternetHandle hConnect(WinHttpConnect(
		hSession.get(), L"www.accuraterip.com",
		INTERNET_DEFAULT_HTTP_PORT, 0));
	if (!hConnect) {
		std::cout << "  SKIPPED: Cannot connect to AccurateRip\n";
		return false;
	}

	wchar_t wUrl[256];
	if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wUrl, 256) == 0) {
		std::cout << "  SKIPPED: Failed to encode AccurateRip request path\n";
		return false;
	}

	ScopedInternetHandle hRequest(WinHttpOpenRequest(
		hConnect.get(), L"GET", wUrl, nullptr, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
	if (!hRequest ||
		!WinHttpSendRequest(
			hRequest.get(), nullptr, 0, nullptr, 0, 0, 0) ||
		!WinHttpReceiveResponse(hRequest.get(), nullptr)) {
		std::cout << "  SKIPPED: AccurateRip request failed\n";
		return false;
	}

	DWORD statusCode = 0;
	DWORD statusSize = sizeof(statusCode);
	if (!WinHttpQueryHeaders(
		hRequest.get(),
		WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		nullptr, &statusCode, &statusSize, nullptr)) {
		std::cout << "  SKIPPED: AccurateRip response had no status code\n";
		return false;
	}

	if (statusCode == 404) {
		pressingCRCs.clear();
		disc.accurateRipLookupAttempted = true;
		std::cout << "  NOT FOUND in AccurateRip database\n";
		return false;
	}
	if (statusCode != 200) {
		std::cout << "  SKIPPED: AccurateRip returned HTTP "
			<< statusCode << "\n";
		return false;
	}

	DWORD contentLength = 0;
	DWORD contentLengthSize = sizeof(contentLength);
	const bool haveContentLength = WinHttpQueryHeaders(
		hRequest.get(),
		WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
		nullptr, &contentLength, &contentLengthSize, nullptr) != FALSE;
	if (haveContentLength &&
		contentLength > kMaxAccurateRipResponseBytes) {
		std::cout << "  SKIPPED: AccurateRip response was too large\n";
		return false;
	}

	std::vector<BYTE> data;
	if (haveContentLength) data.reserve(contentLength);
	for (;;) {
		DWORD bytesAvailable = 0;
		if (!WinHttpQueryDataAvailable(
			hRequest.get(), &bytesAvailable)) {
			std::cout << "  SKIPPED: AccurateRip response was incomplete\n";
			return false;
		}
		if (bytesAvailable == 0) break;
		if (bytesAvailable >
			kMaxAccurateRipResponseBytes - data.size()) {
			std::cout << "  SKIPPED: AccurateRip response was too large\n";
			return false;
		}

		std::vector<BYTE> buffer(bytesAvailable);
		DWORD bytesRead = 0;
		if (!WinHttpReadData(
			hRequest.get(), buffer.data(), bytesAvailable, &bytesRead) ||
			bytesRead == 0 || bytesRead > bytesAvailable) {
			std::cout << "  SKIPPED: AccurateRip response was incomplete\n";
			return false;
		}
		data.insert(
			data.end(), buffer.begin(), buffer.begin() + bytesRead);
	}

	if (haveContentLength && data.size() != contentLength) {
		std::cout << "  SKIPPED: AccurateRip response length did not match\n";
		return false;
	}

	std::vector<std::vector<uint32_t>> parsedReferences;
	if (!TryParseReferences(data, ids, parsedReferences)) {
		std::cout << "  SKIPPED: AccurateRip response was malformed\n";
		return false;
	}

	pressingCRCs = std::move(parsedReferences);
	disc.accurateRipLookupAttempted = true;
	std::cout << "  FOUND in AccurateRip database!\n";
	std::cout << "  Found " << pressingCRCs.size() << " pressing(s)\n";
	return true;
}

bool AccurateRip::VerifyCRCs(const DiscInfo& disc, const std::vector<std::vector<uint32_t>>& pressingCRCs) {
	std::cout << "\n=== AccurateRip CRC Verification ===\n";
	auto unavailable = [](const char* reason) {
		std::cout << "AccurateRip verification unavailable: "
			<< reason << "\n";
		return false;
	};

	if (pressingCRCs.empty()) {
		return unavailable("no reference CRCs.");
	}
	if (disc.pregapMode == PregapMode::Skip) {
		return unavailable(
			"the read omitted pregap sectors required by "
			"AccurateRip track boundaries.");
	}

	const int totalAudioTracks = CountAudioTracks(disc);
	if (totalAudioTracks <= 0) {
		return unavailable("the disc has no audio tracks.");
	}
	for (const auto& record : pressingCRCs) {
		if (record.size() != static_cast<size_t>(totalAudioTracks)) {
			return unavailable(
				"the reference records do not cover every audio track.");
		}
	}
	if (disc.rawSectors.empty()) {
		return unavailable("no captured sectors are available.");
	}

	auto trackSelected = [&disc](const TrackInfo& track) {
		return disc.selectedSession <= 0 ||
			track.session == disc.selectedSession;
	};

	const size_t invalidOffset = (std::numeric_limits<size_t>::max)();
	std::vector<size_t> trackDataOffset(
		disc.tracks.size(), invalidOffset);
	std::vector<DWORD> rawSectorLBA;
	std::vector<bool> rawSectorIsAudio;
	rawSectorLBA.reserve(disc.rawSectors.size());
	rawSectorIsAudio.reserve(disc.rawSectors.size());

	size_t cumulative = 0;
	int selectedAudioTracks = 0;
	DWORD previousReadStart = 0;
	DWORD previousEndLBA = 0;
	bool haveSelectedTrack = false;

	for (size_t i = 0; i < disc.tracks.size(); ++i) {
		const auto& track = disc.tracks[i];
		if (!trackSelected(track)) continue;

		const uint64_t readStart = track.pregapLBA;
		const uint64_t startLBA = track.startLBA;
		const uint64_t endLBA = track.endLBA;
		if (readStart > startLBA || startLBA > endLBA ||
			(haveSelectedTrack &&
				(readStart <= previousReadStart ||
					readStart <= previousEndLBA))) {
			return unavailable("the captured track geometry is invalid.");
		}

		const uint64_t sectorCount64 = endLBA - readStart + 1;
		const uint64_t startOffset64 = startLBA - readStart;
		if (sectorCount64 > disc.rawSectors.size() - cumulative ||
			startOffset64 >= sectorCount64) {
			return unavailable(
				"the captured sectors do not match the track geometry.");
		}

		const size_t sectorCount = static_cast<size_t>(sectorCount64);
		const size_t startOffset = static_cast<size_t>(startOffset64);
		trackDataOffset[i] = cumulative + startOffset;

		for (size_t sector = 0; sector < sectorCount; ++sector) {
			rawSectorLBA.push_back(
				static_cast<DWORD>(readStart + sector));
			rawSectorIsAudio.push_back(track.isAudio);
		}

		cumulative += sectorCount;
		if (track.isAudio) selectedAudioTracks++;
		previousReadStart = static_cast<DWORD>(readStart);
		previousEndLBA = track.endLBA;
		haveSelectedTrack = true;
	}

	if (selectedAudioTracks <= 0) {
		return unavailable(
			"the selected session has no audio tracks.");
	}
	if (cumulative != disc.rawSectors.size() ||
		rawSectorLBA.size() != disc.rawSectors.size()) {
		return unavailable(
			"the captured sector count does not match the selected tracks.");
	}

	const uint64_t audioLeadOut = GetAudioLeadOut(disc);
	if (audioLeadOut == 0) {
		return unavailable("the audio lead-out is missing.");
	}

	bool allMatch = true;
	int audioTrackIdx = 0;
	for (size_t i = 0; i < disc.tracks.size(); ++i) {
		const auto& t = disc.tracks[i];
		if (!t.isAudio) continue;
		const int referenceTrackIdx = audioTrackIdx++;
		if (!trackSelected(t)) continue;

		// AccurateRip defines track boundaries by the original TOC:
		//   startLBA  →  next audio track's startLBA - 1   (or audio lead-out - 1 for last audio track)
		// The stored endLBA may have been trimmed by pregap scanning, so we
		// reconstruct the original boundary here.
		// For enhanced CDs, use the audio session lead-out instead of the
		// data track's startLBA.
		if (static_cast<uint64_t>(t.startLBA) >= audioLeadOut) {
			return unavailable(
				"an audio track starts at or beyond the audio lead-out.");
		}

		uint64_t originalEndLBA = audioLeadOut - 1;
		for (size_t j = i + 1; j < disc.tracks.size(); ++j) {
			const auto& next = disc.tracks[j];
			if (trackSelected(next) && next.isAudio &&
				next.session == t.session) {
				if (next.startLBA <= t.startLBA) {
					return unavailable(
						"the audio track order is invalid.");
				}
				originalEndLBA =
					static_cast<uint64_t>(next.startLBA) - 1;
				break;
			}
		}
		if (originalEndLBA < t.startLBA) {
			return unavailable("an audio track has an invalid boundary.");
		}

		const uint64_t arSectorCount64 =
			originalEndLBA - t.startLBA + 1;
		if (arSectorCount64 >
			(std::numeric_limits<size_t>::max)()) {
			return unavailable("an audio track is too large.");
		}
		const size_t arSectorCount =
			static_cast<size_t>(arSectorCount64);
		const size_t baseIdx = trackDataOffset[i];
		if (baseIdx == invalidOffset ||
			baseIdx > disc.rawSectors.size() ||
			arSectorCount > disc.rawSectors.size() - baseIdx) {
			return unavailable(
				"the captured audio is incomplete.");
		}

		for (size_t sector = 0; sector < arSectorCount; ++sector) {
			const uint64_t expectedLBA =
				static_cast<uint64_t>(t.startLBA) + sector;
			if (rawSectorLBA[baseIdx + sector] != expectedLBA ||
				!rawSectorIsAudio[baseIdx + sector]) {
				return unavailable(
					"the captured audio is not contiguous at "
					"AccurateRip track boundaries.");
			}
		}

		TrackCRCs crcs;
		if (!TryCalculateTrackCRCs(
			disc.rawSectors, baseIdx, arSectorCount,
			referenceTrackIdx + 1, totalAudioTracks, crcs)) {
			return unavailable(
				"an audio track contains missing or undersized sectors.");
		}
		uint32_t crcV1 = crcs.v1;
		uint32_t crcV2 = crcs.v2;

		// AccurateRip database records may have been submitted by either an
		// ARv1 or ARv2 client, so accept a match from either algorithm.
		bool match = false;
		int matchedPressing = -1;
		int matchedVersion = 0;
		for (size_t p = 0; p < pressingCRCs.size(); p++) {
			if (crcV1 == pressingCRCs[p][referenceTrackIdx]) {
				match = true;
				matchedPressing = static_cast<int>(p) + 1;
				matchedVersion = 1;
				break;
			}
			if (crcV2 == pressingCRCs[p][referenceTrackIdx]) {
				match = true;
				matchedPressing = static_cast<int>(p) + 1;
				matchedVersion = 2;
				break;
			}
		}

		std::cout << "Track " << std::setw(2) << t.trackNumber
			<< ": CRC V1 = " << std::hex << std::setw(8)
			<< std::setfill('0') << crcV1
			<< ", V2 = " << std::setw(8) << crcV2
			<< std::dec << std::setfill(' ');

		if (match) {
			std::cout << "  [OK - AR v" << matchedVersion
				<< ", record #" << matchedPressing << "]\n";
		}
		else {
			std::cout << "  [MISMATCH]\n";
			allMatch = false;
		}
	}

	return allMatch;
}
