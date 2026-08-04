// ============================================================================
// AccurateRip.cpp - AccurateRip implementation
// ============================================================================
#include "AccurateRip.h"
#include <winhttp.h>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "winhttp.lib")

namespace {

constexpr size_t kFramesPerSector = AUDIO_SECTOR_SIZE / 4;
constexpr size_t kMaxAccurateRipResponseBytes = 16 * 1024 * 1024;
// AccurateRip's offset window spans five CD-DA sectors in either direction.
constexpr int kFrame450OffsetSearchRadius =
	5 * static_cast<int>(kFramesPerSector) - 1;
constexpr size_t kFrame450Start = 450 * kFramesPerSector;
constexpr size_t kMaxFullOffsetCandidates = 64;

struct TrackCRCs {
	uint32_t v1 = 0;
	uint32_t v2 = 0;
};

bool TryCalculateTrackCRCsAtOffset(
	const std::vector<std::vector<BYTE>>& sectors,
	size_t firstSector,
	size_t sectorCount,
	int trackNum,
	int totalTracks,
	int frameOffset,
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
	if (sectors.size() > static_cast<size_t>((std::numeric_limits<int64_t>::max)()) /
		kFramesPerSector || firstSector > sectors.size()) {
		return false;
	}

	const int64_t capturedFrames =
		static_cast<int64_t>(sectors.size() * kFramesPerSector);
	const int64_t firstSourceFrame =
		static_cast<int64_t>(firstSector * kFramesPerSector) +
		static_cast<int64_t>(skipStartFrames) + frameOffset;
	const int64_t sourceEndFrame =
		static_cast<int64_t>(firstSector * kFramesPerSector) +
		static_cast<int64_t>(includedEnd) + frameOffset;
	if (firstSourceFrame < 0 || sourceEndFrame > capturedFrames ||
		firstSourceFrame >= sourceEndFrame) {
		return false;
	}

	uint32_t highProductSum = 0;
	size_t sourceSector =
		static_cast<size_t>(firstSourceFrame) / kFramesPerSector;
	size_t sourceFrameInSector =
		static_cast<size_t>(firstSourceFrame) % kFramesPerSector;
	for (size_t frameIndex = skipStartFrames;
		frameIndex < includedEnd; ++frameIndex) {
		if (sourceSector >= sectors.size() ||
			sectors[sourceSector].size() < AUDIO_SECTOR_SIZE) {
			return false;
		}
		const BYTE* data = sectors[sourceSector].data() + sourceFrameInSector * 4;
		const uint32_t sample = static_cast<uint32_t>(data[0]) |
			(static_cast<uint32_t>(data[1]) << 8) |
			(static_cast<uint32_t>(data[2]) << 16) |
			(static_cast<uint32_t>(data[3]) << 24);
		const uint64_t product =
			static_cast<uint64_t>(sample) * (frameIndex + 1);
		result.v1 += static_cast<uint32_t>(product);
		highProductSum += static_cast<uint32_t>(product >> 32);

		if (++sourceFrameInSector == kFramesPerSector) {
			sourceFrameInSector = 0;
			++sourceSector;
		}
	}

	// ARv2 folds the high 32 bits of every weighted product into the ARv1 sum.
	result.v2 = result.v1 + highProductSum;
	return true;
}

bool TryCalculateTrackCRCs(
	const std::vector<std::vector<BYTE>>& sectors,
	size_t firstSector,
	size_t sectorCount,
	int trackNum,
	int totalTracks,
	TrackCRCs& result) {
	return TryCalculateTrackCRCsAtOffset(
		sectors, firstSector, sectorCount, trackNum, totalTracks, 0, result);
}

bool TryCalculateFrame450(
	const std::vector<std::vector<BYTE>>& sectors,
	size_t firstSector,
	size_t sectorCount,
	int frameOffset,
	uint32_t& checksum) {
	checksum = 0;
	if (sectorCount == 0 || firstSector > sectors.size() ||
		sectorCount > sectors.size() - firstSector ||
		sectorCount > (std::numeric_limits<size_t>::max)() / kFramesPerSector ||
		sectors.size() > static_cast<size_t>((std::numeric_limits<int64_t>::max)()) /
			kFramesPerSector) {
		return false;
	}

	const int64_t totalFrames =
		static_cast<int64_t>(sectorCount * kFramesPerSector);
	// The unshifted probe must exist in the reference track. A shifted probe
	// may cross into an adjacent audio track; the verifier separately checks
	// that the captured range is contiguous audio.
	if (totalFrames < static_cast<int64_t>(
		kFrame450Start + kFramesPerSector)) {
		return false;
	}

	const int64_t firstSourceFrameSigned =
		static_cast<int64_t>(firstSector * kFramesPerSector) +
		static_cast<int64_t>(kFrame450Start) + frameOffset;
	const int64_t capturedFrames =
		static_cast<int64_t>(sectors.size() * kFramesPerSector);
	if (firstSourceFrameSigned < 0 ||
		firstSourceFrameSigned + static_cast<int64_t>(kFramesPerSector) >
		capturedFrames) {
		return false;
	}
	const size_t firstSourceFrame =
		static_cast<size_t>(firstSourceFrameSigned);
	size_t sourceSector = firstSourceFrame / kFramesPerSector;
	size_t sourceFrameInSector = firstSourceFrame % kFramesPerSector;
	for (size_t i = 0; i < kFramesPerSector; ++i) {
		if (sourceSector >= sectors.size() ||
			sectors[sourceSector].size() < AUDIO_SECTOR_SIZE) {
			return false;
		}
		const BYTE* data = sectors[sourceSector].data() + sourceFrameInSector * 4;
		const uint32_t sample = static_cast<uint32_t>(data[0]) |
			(static_cast<uint32_t>(data[1]) << 8) |
			(static_cast<uint32_t>(data[2]) << 16) |
			(static_cast<uint32_t>(data[3]) << 24);
		checksum += sample * static_cast<uint32_t>(i + 1);

		if (++sourceFrameInSector == kFramesPerSector) {
			sourceFrameInSector = 0;
			++sourceSector;
		}
	}
	return true;
}

uint32_t ReadLittleEndian32(const std::vector<BYTE>& data, size_t offset) {
	return static_cast<uint32_t>(data[offset]) |
		(static_cast<uint32_t>(data[offset + 1]) << 8) |
		(static_cast<uint32_t>(data[offset + 2]) << 16) |
		(static_cast<uint32_t>(data[offset + 3]) << 24);
}

} // namespace

// Helper: get the lead-out LBA for AccurateRip checksum slicing.
// For enhanced/multisession CDs, audio ends at the first session's lead-out.
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
	// The AccurateRip DLL uses the overall TOC lead-out for both disc IDs,
	// including on enhanced CDs. Only checksum slicing stops at the audio
	// session lead-out.
	const uint64_t leadOut = disc.leadOutLBA;
	if (leadOut == 0) return false;

	uint64_t id1 = leadOut;
	uint64_t id2 = 0;
	uint64_t cddbDigitSum = 0;
	uint64_t firstTrackLBA = 0;
	uint64_t previousTrackLBA = 0;
	int physicalTrackCount = 0;
	bool haveAudio = false;
	bool haveTrack = false;

	for (const auto& track : disc.tracks) {
		const uint64_t startLBA = TocStartLBA(disc, track);
		if (startLBA >= leadOut ||
			(haveTrack && startLBA <= previousTrackLBA) ||
			physicalTrackCount == 255) {
			return false;
		}
		if (!haveTrack) firstTrackLBA = startLBA;
		previousTrackLBA = startLBA;
		haveTrack = true;
		physicalTrackCount++;
		cddbDigitSum += SumDecimalDigits((startLBA + 150) / 75);

		if (!track.isAudio) continue;
		if (ids.trackCount == 255) return false;

		ids.trackCount++;
		const uint64_t frameNumber = (startLBA == 0) ? 1 : startLBA;
		id1 += startLBA;
		id2 += frameNumber * static_cast<uint64_t>(ids.trackCount);
		haveAudio = true;
	}

	if (!haveAudio || !haveTrack) return false;

	id2 += leadOut * static_cast<uint64_t>(ids.trackCount + 1);
	const uint64_t leadOutSeconds = (leadOut + 150) / 75;
	const uint64_t firstTrackSeconds = (firstTrackLBA + 150) / 75;
	if (leadOutSeconds < firstTrackSeconds) return false;

	const uint64_t durationSeconds = leadOutSeconds - firstTrackSeconds;
	if (durationSeconds > 0x00FFFFFFu) return false;

	ids.discId1 = static_cast<uint32_t>(id1);
	ids.discId2 = static_cast<uint32_t>(id2);
	ids.cddbId =
		(static_cast<uint32_t>(cddbDigitSum % 0xFF) << 24) |
		(static_cast<uint32_t>(durationSeconds) << 8) |
		static_cast<uint32_t>(physicalTrackCount);
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
	std::vector<AccurateRipPressing>& references) {
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
		AccurateRipPressing record;
		record.reserve(static_cast<size_t>(ids.trackCount));
		size_t position = chunkOffset + 13;

		for (int track = 0; track < ids.trackCount; ++track) {
			AccurateRipTrackReference reference;
			reference.confidence = data[position++];
			reference.checksum = ReadLittleEndian32(data, position);
			position += 4;
			if (isExtended) {
				reference.frame450Checksum =
					ReadLittleEndian32(data, position);
				reference.hasFrame450Checksum =
					reference.frame450Checksum != 0;
				position += 4;
			}
			record.push_back(reference);
		}
		references.push_back(std::move(record));
	}

	return !references.empty();
}

} // namespace

bool AccurateRip::Lookup(DiscInfo& disc, std::vector<AccurateRipPressing>& pressings) {
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
		pressings.clear();
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

	std::vector<AccurateRipPressing> parsedReferences;
	if (!TryParseReferences(data, ids, parsedReferences)) {
		std::cout << "  SKIPPED: AccurateRip response was malformed\n";
		return false;
	}

	pressings = std::move(parsedReferences);
	disc.accurateRipLookupAttempted = true;
	std::cout << "  FOUND in AccurateRip database!\n";
	std::cout << "  Found " << pressings.size() << " pressing(s)\n";
	return true;
}

AccurateRipVerificationResult AccurateRip::VerifyCRCs(
	const DiscInfo& disc,
	const std::vector<AccurateRipPressing>& pressings) {
	std::cout << "\n=== AccurateRip CRC Verification ===\n";
	auto unavailable = [](const char* reason) {
		std::cout << "AccurateRip verification unavailable: "
			<< reason << "\n";
		return AccurateRipVerificationResult::Inconclusive;
	};

	if (pressings.empty()) {
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
	for (const auto& record : pressings) {
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

	constexpr size_t invalidOffset = (std::numeric_limits<size_t>::max)();
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

	struct VerificationTrack {
		const TrackInfo* track = nullptr;
		size_t baseSector = 0;
		size_t sectorCount = 0;
		int referenceTrackIndex = 0;
	};
	std::vector<VerificationTrack> verificationTracks;
	verificationTracks.reserve(static_cast<size_t>(selectedAudioTracks));

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

		verificationTracks.push_back(
			{ &t, baseIdx, arSectorCount, referenceTrackIdx });
	}

	if (verificationTracks.empty()) {
		return unavailable("the selected session has no verifiable audio tracks.");
	}

	auto floorDivideFrames = [](int64_t value) {
		int64_t quotient = value / static_cast<int64_t>(kFramesPerSector);
		if (value < 0 && value % static_cast<int64_t>(kFramesPerSector) != 0)
			--quotient;
		return quotient;
	};

	// A shifted checksum may consume a few samples from the adjacent track.
	// Validate those samples against the captured LBA map so an offset search
	// can never bridge a data track, omitted range, or non-contiguous session.
	auto shiftedRangeIsContiguousAudio = [&](const VerificationTrack& view,
		size_t logicalBegin, size_t logicalEnd, int frameOffset) {
		if (logicalBegin >= logicalEnd ||
			view.baseSector > static_cast<size_t>((std::numeric_limits<int64_t>::max)()) /
				kFramesPerSector ||
			disc.rawSectors.size() >
				static_cast<size_t>((std::numeric_limits<int64_t>::max)()) /
				kFramesPerSector) {
			return false;
		}

		const int64_t relativeFirst =
			static_cast<int64_t>(logicalBegin) + frameOffset;
		const int64_t relativeLast =
			static_cast<int64_t>(logicalEnd - 1) + frameOffset;
		const int64_t baseFrame =
			static_cast<int64_t>(view.baseSector * kFramesPerSector);
		const int64_t firstFrame = baseFrame + relativeFirst;
		const int64_t lastFrame = baseFrame + relativeLast;
		const int64_t capturedFrames =
			static_cast<int64_t>(disc.rawSectors.size() * kFramesPerSector);
		if (firstFrame < 0 || lastFrame < firstFrame ||
			lastFrame >= capturedFrames) {
			return false;
		}

		const size_t firstSector =
			static_cast<size_t>(firstFrame) / kFramesPerSector;
		const size_t lastSector =
			static_cast<size_t>(lastFrame) / kFramesPerSector;
		const int64_t relativeFirstSector = floorDivideFrames(relativeFirst);
		const int64_t expectedFirstLBA =
			static_cast<int64_t>(view.track->startLBA) + relativeFirstSector;
		if (expectedFirstLBA < 0) return false;

		for (size_t sector = firstSector; sector <= lastSector; ++sector) {
			const uint64_t expectedLBA =
				static_cast<uint64_t>(expectedFirstLBA) +
				static_cast<uint64_t>(sector - firstSector);
			if (sector >= rawSectorLBA.size() ||
				rawSectorLBA[sector] != expectedLBA ||
				!rawSectorIsAudio[sector]) {
				return false;
			}
		}
		return true;
	};

	struct TrackMatch {
		TrackCRCs crcs;
		bool matched = false;
		int pressing = -1;
		int version = 0;
		int confidence = 0;
	};

	struct IndexedReference {
		int pressing = -1;
		int confidence = 0;
	};
	std::vector<std::unordered_map<uint32_t, IndexedReference>>
		checksumReferences(static_cast<size_t>(totalAudioTracks));
	std::vector<std::unordered_set<uint32_t>>
		frame450References(static_cast<size_t>(totalAudioTracks));
	for (size_t p = 0; p < pressings.size(); ++p) {
		for (int track = 0; track < totalAudioTracks; ++track) {
			const auto& reference = pressings[p][track];
			// The AccurateRip DLL treats a zero primary checksum as an absent
			// reference and returns the confidence byte as the verification
			// result. Neither a zero checksum nor zero confidence can verify a
			// track, even when the calculated checksum is also zero.
			if (reference.checksum == 0 || reference.confidence == 0) continue;
			checksumReferences[track].try_emplace(
				reference.checksum,
				IndexedReference{ static_cast<int>(p) + 1,
					reference.confidence });
			if (reference.hasFrame450Checksum) {
				frame450References[track].insert(
					reference.frame450Checksum);
			}
		}
	}
	for (const auto& view : verificationTracks) {
		if (checksumReferences[view.referenceTrackIndex].empty()) {
			return unavailable(
				"at least one selected track has no usable checksum reference.");
		}
	}

	auto findReferenceMatch = [&](int referenceTrackIndex,
		const TrackCRCs& crcs, TrackMatch& result) {
		const auto& references = checksumReferences[referenceTrackIndex];
		const auto v1Match = references.find(crcs.v1);
		const auto v2Match = references.find(crcs.v2);
		const IndexedReference* match = nullptr;
		int version = 0;
		if (v1Match != references.end() &&
			(v2Match == references.end() ||
				v1Match->second.pressing <= v2Match->second.pressing)) {
			match = &v1Match->second;
			version = 1;
		}
		else if (v2Match != references.end()) {
			match = &v2Match->second;
			version = 2;
		}
		if (match) {
			result.matched = true;
			result.pressing = match->pressing;
			result.version = version;
			result.confidence = match->confidence;
		}
	};

	std::vector<size_t> candidateTrackOrder;
	candidateTrackOrder.reserve(verificationTracks.size());
	for (size_t i = 0; i < verificationTracks.size(); ++i)
		candidateTrackOrder.push_back(i);
	auto includedFrameCount = [&](size_t index) {
		const auto& view = verificationTracks[index];
		const uint64_t totalFrames =
			static_cast<uint64_t>(view.sectorCount) * kFramesPerSector;
		const uint64_t skipStart =
			(view.referenceTrackIndex == 0) ? 5 * kFramesPerSector - 1 : 0;
		const uint64_t skipEnd =
			(view.referenceTrackIndex + 1 == totalAudioTracks)
			? 5 * kFramesPerSector : 0;
		const uint64_t excludedFrames = skipStart + skipEnd;
		return totalFrames > excludedFrames
			? totalFrames - excludedFrames : uint64_t{ 0 };
	};
	std::sort(candidateTrackOrder.begin(), candidateTrackOrder.end(),
		[&](size_t left, size_t right) {
			const uint64_t leftFrames = includedFrameCount(left);
			const uint64_t rightFrames = includedFrameCount(right);
			if (leftFrames != rightFrames) {
				return leftFrames < rightFrames;
			}
			return left < right;
		});

	auto calculateAtOffset = [&](int frameOffset,
		std::vector<TrackMatch>& results, bool& allMatch,
		bool stopOnMismatch) {
		results.clear();
		results.resize(verificationTracks.size());
		allMatch = true;
		for (size_t position = 0;
			position < verificationTracks.size(); ++position) {
			const size_t i = stopOnMismatch
				? candidateTrackOrder[position] : position;
			const auto& view = verificationTracks[i];
			const size_t totalFrames = view.sectorCount * kFramesPerSector;
			const size_t skipStart =
				(view.referenceTrackIndex == 0) ? 5 * kFramesPerSector - 1 : 0;
			const size_t skipEnd =
				(view.referenceTrackIndex + 1 == totalAudioTracks)
				? 5 * kFramesPerSector : 0;
			if (skipStart + skipEnd >= totalFrames ||
				!shiftedRangeIsContiguousAudio(
					view, skipStart, totalFrames - skipEnd, frameOffset) ||
				!TryCalculateTrackCRCsAtOffset(
					disc.rawSectors, view.baseSector, view.sectorCount,
					view.referenceTrackIndex + 1, totalAudioTracks,
					frameOffset, results[i].crcs)) {
				return false;
			}
			findReferenceMatch(
				view.referenceTrackIndex, results[i].crcs, results[i]);
			if (!results[i].matched) {
				allMatch = false;
				if (stopOnMismatch) return true;
			}
		}
		return true;
	};

	std::vector<TrackMatch> directResults;
	bool directMatch = false;
	if (!calculateAtOffset(0, directResults, directMatch, false)) {
		return unavailable(
			"an audio track contains missing, non-contiguous, or undersized sectors.");
	}

	int verificationOffset = 0;
	int reportedOffset = 0;
	bool allMatch = directMatch;
	std::vector<TrackMatch> finalResults = directResults;
	int reportedMatchCount = static_cast<int>(std::count_if(
		finalResults.begin(), finalResults.end(),
		[](const TrackMatch& result) { return result.matched; }));
	bool reportedCandidateEvaluated = false;
	std::map<int, int> candidateSupport;
	int probeTrackCount = 0;
	bool candidateLimitReached = false;
	int bestProbeOffset = 0;
	int bestProbeSupport = 0;
	bool haveProbeCandidate = false;

	if (!directMatch) {
		for (const auto& view : verificationTracks) {
			const auto& probeReferences =
				frame450References[view.referenceTrackIndex];
			if (probeReferences.empty()) continue;

			bool trackWasProbeEligible = false;
			for (int offset = -kFrame450OffsetSearchRadius;
				offset <= kFrame450OffsetSearchRadius; ++offset) {
				if (!shiftedRangeIsContiguousAudio(
					view, kFrame450Start,
					kFrame450Start + kFramesPerSector, offset)) {
					continue;
				}
				uint32_t probe = 0;
				if (!TryCalculateFrame450(
					disc.rawSectors, view.baseSector,
					view.sectorCount, offset, probe)) {
					continue;
				}
				trackWasProbeEligible = true;
				if (probeReferences.contains(probe)) {
					candidateSupport[offset]++;
				}
			}
			if (trackWasProbeEligible) probeTrackCount++;
		}
		if (probeTrackCount == 0) {
			return unavailable(
				"no selected track has a usable Frame450 probe for offset search.");
		}

		struct OffsetCandidate {
			int offset = 0;
			int support = 0;
		};
		std::vector<OffsetCandidate> candidates;
		for (const auto& [offset, support] : candidateSupport) {
			candidates.push_back({ offset, support });
		}
		std::sort(candidates.begin(), candidates.end(),
			[](const OffsetCandidate& left, const OffsetCandidate& right) {
				if (left.support != right.support)
					return left.support > right.support;
				const int leftMagnitude =
					left.offset < 0 ? -left.offset : left.offset;
				const int rightMagnitude =
					right.offset < 0 ? -right.offset : right.offset;
				if (leftMagnitude != rightMagnitude)
					return leftMagnitude < rightMagnitude;
				return left.offset > right.offset;
			});
		if (!candidates.empty()) {
			haveProbeCandidate = true;
			bestProbeOffset = candidates.front().offset;
			bestProbeSupport = candidates.front().support;
		}

		const int minimumSupport = probeTrackCount >= 3 ? 2 : 1;
		size_t candidatesAttempted = 0;
		for (const auto& candidate : candidates) {
			if (candidate.support < minimumSupport) break;
			if (candidate.offset == 0) continue; // Already fully tested above.
			if (candidatesAttempted == kMaxFullOffsetCandidates) {
				candidateLimitReached = true;
				break;
			}
			++candidatesAttempted;
			std::vector<TrackMatch> shiftedResults;
			bool shiftedMatch = false;
			if (calculateAtOffset(
				candidate.offset, shiftedResults, shiftedMatch, true) &&
				shiftedMatch) {
				verificationOffset = candidate.offset;
				reportedOffset = candidate.offset;
				allMatch = true;
				finalResults = std::move(shiftedResults);
				reportedMatchCount =
					static_cast<int>(verificationTracks.size());
				break;
			}
		}
	}

	// If no common offset verified the complete disc, calculate every selected
	// track at the strongest Frame450 candidate once more without short-circuiting.
	// This preserves genuine per-track matches instead of printing the unrelated
	// offset-zero results as though every track had failed.
	if (!allMatch && haveProbeCandidate) {
		std::vector<TrackMatch> diagnosticResults;
		bool diagnosticAllMatch = false;
		if (bestProbeOffset == 0) {
			diagnosticResults = directResults;
			diagnosticAllMatch = directMatch;
			reportedCandidateEvaluated = true;
		}
		else if (calculateAtOffset(
			bestProbeOffset, diagnosticResults, diagnosticAllMatch, false)) {
			reportedCandidateEvaluated = true;
		}
		if (reportedCandidateEvaluated) {
			reportedOffset = bestProbeOffset;
			finalResults = std::move(diagnosticResults);
			reportedMatchCount = static_cast<int>(std::count_if(
				finalResults.begin(), finalResults.end(),
				[](const TrackMatch& result) { return result.matched; }));
			if (diagnosticAllMatch) {
				verificationOffset = bestProbeOffset;
				allMatch = true;
			}
		}
	}

	if (verificationOffset != 0) {
		std::cout << "Full-track CRCs match at pressing/verification offset "
			<< (verificationOffset > 0 ? "+" : "")
			<< verificationOffset << " samples.\n";
	}
	else if (!allMatch && candidateLimitReached) {
		std::cout << "Frame450 verification inconclusive: best shared offset "
			<< (bestProbeOffset > 0 ? "+" : "") << bestProbeOffset
			<< " samples matched " << bestProbeSupport << "/"
			<< probeTrackCount << " usable track probe(s); only the "
			<< kMaxFullOffsetCandidates
			<< " strongest nonzero candidates were fully tested.\n";
	}
	else if (!allMatch && haveProbeCandidate) {
		std::cout << "Frame450 diagnostic: strongest shared offset "
			<< (bestProbeOffset > 0 ? "+" : "") << bestProbeOffset
			<< " samples matched " << bestProbeSupport << "/"
			<< probeTrackCount << " usable track probe(s).\n";
	}
	if (!allMatch && reportedCandidateEvaluated) {
		std::cout << "Full-track CRCs at offset "
			<< (reportedOffset > 0 ? "+" : "") << reportedOffset
			<< " matched " << reportedMatchCount << "/"
			<< verificationTracks.size() << " selected track(s).\n";
	}

	for (size_t i = 0; i < verificationTracks.size(); ++i) {
		const auto& view = verificationTracks[i];
		const auto& result = finalResults[i];

		std::cout << "Track " << std::setw(2) << view.track->trackNumber
			<< ": CRC V1 = " << std::hex << std::setw(8)
			<< std::setfill('0') << result.crcs.v1
			<< ", V2 = " << std::setw(8) << result.crcs.v2
			<< std::dec << std::setfill(' ');

		if (candidateLimitReached) {
			if (result.matched) {
				std::cout << "  [AR v" << result.version
					<< " MATCH - record #" << result.pressing
					<< ", confidence " << result.confidence
					<< ", offset " << (reportedOffset > 0 ? "+" : "")
					<< reportedOffset << " - overall search incomplete]";
			}
			else {
				std::cout << "  [NO MATCH AT OFFSET "
					<< (reportedOffset > 0 ? "+" : "") << reportedOffset
					<< " - overall search incomplete]";
			}
			std::cout << "\n";
		}
		else if (result.matched) {
			std::cout << "  [OK - AR v" << result.version
				<< ", record #" << result.pressing
				<< ", confidence " << result.confidence;
			if (reportedOffset != 0) {
				std::cout << ", offset "
					<< (reportedOffset > 0 ? "+" : "")
					<< reportedOffset;
			}
			std::cout << "]\n";
		}
		else {
			std::cout << "  [MISMATCH";
			if (reportedCandidateEvaluated) {
				std::cout << " at offset "
					<< (reportedOffset > 0 ? "+" : "")
					<< reportedOffset;
			}
			std::cout << "]\n";
		}
	}

	if (allMatch) return AccurateRipVerificationResult::Verified;
	if (candidateLimitReached)
		return AccurateRipVerificationResult::Inconclusive;
	return AccurateRipVerificationResult::Mismatch;
}
