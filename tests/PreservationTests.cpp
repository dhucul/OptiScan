#define NOMINMAX
#include "../AccurateRip.h"
#include "../DriveCapabilityParsing.h"
#include "../Preservation.h"
#include "../RecoveryCheckpoint.h"
#include "../ScanResults.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
	if (condition) {
		std::cout << "[PASS] " << message << "\n";
	}
	else {
		std::cerr << "[FAIL] " << message << "\n";
		failures++;
	}
}

std::array<uint32_t, 256> BuildEdcTable() {
	std::array<uint32_t, 256> table{};
	for (uint32_t i = 0; i < table.size(); ++i) {
		uint32_t value = i;
		for (int bit = 0; bit < 8; ++bit)
			value = (value >> 1) ^ ((value & 1) ? 0xD8018001u : 0u);
		table[i] = value;
	}
	return table;
}

uint32_t Edc(const BYTE* data, size_t size) {
	static auto table = BuildEdcTable();
	uint32_t result = 0;
	for (size_t i = 0; i < size; ++i)
		result = (result >> 8) ^ table[(result ^ data[i]) & 0xFF];
	return result;
}

void PutLe32(BYTE* data, uint32_t value) {
	data[0] = static_cast<BYTE>(value);
	data[1] = static_cast<BYTE>(value >> 8);
	data[2] = static_cast<BYTE>(value >> 16);
	data[3] = static_cast<BYTE>(value >> 24);
}

void PutBe16(BYTE* data, uint16_t value) {
	data[0] = static_cast<BYTE>(value >> 8);
	data[1] = static_cast<BYTE>(value);
}

void PutBe32(BYTE* data, uint32_t value) {
	data[0] = static_cast<BYTE>(value >> 24);
	data[1] = static_cast<BYTE>(value >> 16);
	data[2] = static_cast<BYTE>(value >> 8);
	data[3] = static_cast<BYTE>(value);
}

std::vector<std::vector<BYTE>> MakeAudioSectors(size_t sectorCount) {
	return std::vector<std::vector<BYTE>>(
		sectorCount, std::vector<BYTE>(AUDIO_SECTOR_SIZE, 0));
}

void PutAudioFrame(std::vector<std::vector<BYTE>>& sectors,
	size_t frameIndex, uint32_t value) {
	constexpr size_t framesPerSector = AUDIO_SECTOR_SIZE / 4;
	const size_t sector = frameIndex / framesPerSector;
	const size_t byteOffset = (frameIndex % framesPerSector) * 4;
	PutLe32(sectors[sector].data() + byteOffset, value);
}

uint32_t GetAudioFrame(const std::vector<std::vector<BYTE>>& sectors,
	size_t frameIndex) {
	constexpr size_t framesPerSector = AUDIO_SECTOR_SIZE / 4;
	const size_t sector = frameIndex / framesPerSector;
	const size_t byteOffset = (frameIndex % framesPerSector) * 4;
	const BYTE* data = sectors[sector].data() + byteOffset;
	return static_cast<uint32_t>(data[0]) |
		(static_cast<uint32_t>(data[1]) << 8) |
		(static_cast<uint32_t>(data[2]) << 16) |
		(static_cast<uint32_t>(data[3]) << 24);
}

void FillDeterministicAudio(std::vector<std::vector<BYTE>>& sectors,
	uint32_t seed) {
	constexpr size_t framesPerSector = AUDIO_SECTOR_SIZE / 4;
	uint32_t state = seed;
	for (size_t frame = 0; frame < sectors.size() * framesPerSector; ++frame) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		PutAudioFrame(sectors, frame, state);
	}
}

uint32_t CalculateFrame450Reference(
	const std::vector<std::vector<BYTE>>& sectors) {
	constexpr size_t framesPerSector = AUDIO_SECTOR_SIZE / 4;
	constexpr size_t frame450Start = 450 * framesPerSector;
	uint32_t checksum = 0;
	for (size_t i = 0; i < framesPerSector; ++i) {
		checksum += GetAudioFrame(sectors, frame450Start + i) *
			static_cast<uint32_t>(i + 1);
	}
	return checksum;
}

void BuildEccTables(std::array<BYTE, 256>& forward,
	std::array<BYTE, 256>& backward) {
	for (int i = 0; i < 256; ++i) {
		int value = i << 1;
		if (value & 0x100) value ^= 0x11D;
		forward[i] = static_cast<BYTE>(value);
		backward[i ^ value] = static_cast<BYTE>(i);
	}
}

void Ecc(const BYTE* source, int majorCount, int minorCount,
	int majorMult, int minorInc, BYTE* destination) {
	static std::array<BYTE, 256> forward{};
	static std::array<BYTE, 256> backward{};
	static bool initialized = false;
	if (!initialized) {
		BuildEccTables(forward, backward);
		initialized = true;
	}
	int size = majorCount * minorCount;
	for (int major = 0; major < majorCount; ++major) {
		int index = (major >> 1) * majorMult + (major & 1);
		BYTE a = 0, b = 0;
		for (int minor = 0; minor < minorCount; ++minor) {
			BYTE value = source[index];
			index += minorInc;
			if (index >= size) index -= size;
			a ^= value;
			b ^= value;
			a = forward[a];
		}
		a = backward[forward[a] ^ b];
		destination[major] = a;
		destination[major + majorCount] = a ^ b;
	}
}

std::array<BYTE, AUDIO_SECTOR_SIZE> MakeMode1Sector(DWORD lba) {
	std::array<BYTE, AUDIO_SECTOR_SIZE> sector{};
	sector[0] = 0;
	std::fill(sector.begin() + 1, sector.begin() + 11, static_cast<BYTE>(0xFF));
	sector[11] = 0;
	DWORD absolute = lba + 150;
	sector[12] = BinToBcd(static_cast<BYTE>(absolute / (60 * 75)));
	sector[13] = BinToBcd(static_cast<BYTE>((absolute / 75) % 60));
	sector[14] = BinToBcd(static_cast<BYTE>(absolute % 75));
	sector[15] = 1;
	for (int i = 16; i < 2064; ++i)
		sector[i] = static_cast<BYTE>((i * 17 + 31) & 0xFF);
	PutLe32(sector.data() + 2064, Edc(sector.data(), 2064));
	Ecc(sector.data() + 12, 86, 24, 2, 86, sector.data() + 2076);
	Ecc(sector.data() + 12, 52, 43, 86, 88, sector.data() + 2248);
	return sector;
}

std::array<BYTE, AUDIO_SECTOR_SIZE> MakeMode2Sector(DWORD lba, bool form2) {
	std::array<BYTE, AUDIO_SECTOR_SIZE> sector{};
	sector[0] = 0;
	std::fill(sector.begin() + 1, sector.begin() + 11, static_cast<BYTE>(0xFF));
	sector[11] = 0;
	DWORD absolute = lba + 150;
	sector[12] = BinToBcd(static_cast<BYTE>(absolute / (60 * 75)));
	sector[13] = BinToBcd(static_cast<BYTE>((absolute / 75) % 60));
	sector[14] = BinToBcd(static_cast<BYTE>(absolute % 75));
	sector[15] = 2;
	sector[16] = 1;       // File number.
	sector[17] = 2;       // Channel number.
	sector[18] = form2 ? 0x28 : 0x08; // Data, plus Form 2 when requested.
	sector[19] = 0;
	std::copy_n(sector.begin() + 16, 4, sector.begin() + 20);

	const int payloadEnd = form2 ? 2348 : 2072;
	for (int i = 24; i < payloadEnd; ++i)
		sector[i] = static_cast<BYTE>((i * 29 + 7) & 0xFF);

	if (form2) {
		PutLe32(sector.data() + 2348, Edc(sector.data() + 16, 2332));
	}
	else {
		PutLe32(sector.data() + 2072, Edc(sector.data() + 16, 2056));
		auto eccSource = sector;
		std::fill(eccSource.begin() + 12, eccSource.begin() + 16,
			static_cast<BYTE>(0));
		Ecc(eccSource.data() + 12, 86, 24, 2, 86,
			eccSource.data() + 2076);
		Ecc(eccSource.data() + 12, 52, 43, 86, 88,
			eccSource.data() + 2248);
		std::copy(eccSource.begin() + 2076, eccSource.end(),
			sector.begin() + 2076);
	}
	return sector;
}

std::array<BYTE, AUDIO_SECTOR_SIZE> MakeMode2FormlessSector(DWORD lba) {
	std::array<BYTE, AUDIO_SECTOR_SIZE> sector{};
	sector[0] = 0;
	std::fill(sector.begin() + 1, sector.begin() + 11, static_cast<BYTE>(0xFF));
	sector[11] = 0;
	DWORD absolute = lba + 150;
	sector[12] = BinToBcd(static_cast<BYTE>(absolute / (60 * 75)));
	sector[13] = BinToBcd(static_cast<BYTE>((absolute / 75) % 60));
	sector[14] = BinToBcd(static_cast<BYTE>(absolute % 75));
	sector[15] = 2;
	for (int i = 16; i < AUDIO_SECTOR_SIZE; ++i)
		sector[i] = static_cast<BYTE>((i * 37 + 11) & 0xFF);
	return sector;
}

DiscInfo MakeDisc() {
	DiscInfo disc;
	TrackInfo track;
	track.trackNumber = 1;
	track.startLBA = 0;
	track.pregapLBA = 0;
	track.endLBA = 1;
	track.isAudio = true;
	disc.tracks.push_back(track);
	disc.leadOutLBA = 2;
	disc.includeSubchannel = true;
	return disc;
}

} // namespace

int main() {
	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;

	QCheckResult qcheckStability;
	Check(ClassifyQCheckC2Stability(qcheckStability) ==
		QCheckC2Stability::NoActivity,
		"Q-Check classifies a C2-clean primary pass as no observed activity");
	qcheckStability.totalC2 = 12;
	Check(ClassifyQCheckC2Stability(qcheckStability) ==
		QCheckC2Stability::RecheckIncomplete,
		"Q-Check retains primary C2 when verification is unavailable");
	qcheckStability.c2RecheckCompleted = true;
	Check(ClassifyQCheckC2Stability(qcheckStability) ==
		QCheckC2Stability::RecheckIncomplete,
		"Q-Check does not accept a sample-less verification pass as clean");
	qcheckStability.c2RecheckSamples.push_back(QCheckSample{});
	Check(ClassifyQCheckC2Stability(qcheckStability) ==
		QCheckC2Stability::Intermittent,
		"Q-Check treats a clean verification pass as intermittent");
	qcheckStability.c2RecheckTotal = 3;
	Check(ClassifyQCheckC2Stability(qcheckStability) ==
		QCheckC2Stability::Reproducible,
		"Q-Check treats repeated C2 as reproducible");
	qcheckStability.c2RecheckTotalCU = 1;
	Check(ClassifyQCheckC2Stability(qcheckStability) ==
		QCheckC2Stability::Unrecoverable,
		"Q-Check gives verification-pass CU the highest severity");
	QCheckResult partialRecheckC2;
	partialRecheckC2.totalC2 = 4;
	partialRecheckC2.c2RecheckAttempted = true;
	partialRecheckC2.c2RecheckTotal = 2;
	Check(ClassifyQCheckC2Stability(partialRecheckC2) ==
		QCheckC2Stability::Reproducible,
		"Q-Check retains reproducible C2 from an incomplete verification pass");
	QCheckResult partialRecheckCU;
	partialRecheckCU.totalC2 = 4;
	partialRecheckCU.c2RecheckAttempted = true;
	partialRecheckCU.c2RecheckTotalCU = 1;
	Check(ClassifyQCheckC2Stability(partialRecheckCU) ==
		QCheckC2Stability::Unrecoverable,
		"Q-Check retains CU from an incomplete verification pass");

	// MMC Mode Page 2Ah: exercise the corrected DVD masks, C2/R-W/Q
	// distinction, changer mechanism values, and speed descriptor offsets.
	std::vector<BYTE> mode2A(40, 0);
	mode2A[0] = 0x2A;
	mode2A[1] = 38;
	mode2A[2] = 0x20; // DVD-RAM read alone still means the drive reads DVD.
	mode2A[3] = 0x24; // DVD-RAM write + test write.
	mode2A[4] = 0xC1; // BUF + multisession + hardware audio play.
	mode2A[5] = 0x1F; // C2 + corrected R-W + raw R-W + accurate + CD-DA.
	mode2A[6] = static_cast<BYTE>((4 << 5) | 0x09); // individual changer, eject, lock.
	mode2A[7] = 0x03;
	PutBe16(mode2A.data() + 8, 8467);
	PutBe16(mode2A.data() + 12, 4096);
	PutBe16(mode2A.data() + 14, 2822);
	PutBe16(mode2A.data() + 18, 8467);
	PutBe16(mode2A.data() + 20, 1764);
	PutBe16(mode2A.data() + 28, 706);
	PutBe16(mode2A.data() + 30, 2);
	PutBe16(mode2A.data() + 34, 706);
	PutBe16(mode2A.data() + 38, 8467);
	DriveCapabilities modeCaps;
	Check(DriveCapabilityParsing::ParseModePage2A(
		mode2A.data(), mode2A.size(), modeCaps),
		"Mode Page 2Ah parses a normalized complete response");
	Check(modeCaps.readsDVD && modeCaps.writesDVDRAM,
		"DVD capability includes the distinct DVD-RAM bits");
	Check(modeCaps.supportsC2ErrorReporting
		&& modeCaps.supportsSubchannelRaw
		&& modeCaps.supportsSubchannelDeinterleaved
		&& !modeCaps.supportsSubchannelQ,
		"Mode Page 2Ah does not mislabel corrected R-W as formatted Q");
	Check(modeCaps.loadingMechanism == 4 && modeCaps.isChanger,
		"MMC loading mechanism 4 is an individual-disc changer");
	Check(modeCaps.supportedWriteSpeeds == std::vector<int>({ 706, 8467 }),
		"Mode Page 2Ah uses its 4-byte write-speed descriptors");
	Check(modeCaps.maxWriteSpeedKB == 8467 && modeCaps.currentWriteSpeedKB == 706,
		"Mode Page 2Ah keeps maximum and selected write-speed fields distinct");

	// GET CONFIGURATION Feature 0000h: profile list is the authoritative
	// whole-drive media capability source, independent of current media.
	const std::vector<WORD> profileCodes = {
		0x0008, 0x0009, 0x000A, 0x0010, 0x0011, 0x0012,
		0x001A, 0x0040, 0x0041, 0x0043
	};
	std::vector<BYTE> profileResponse(12 + profileCodes.size() * 4, 0);
	PutBe32(profileResponse.data(), static_cast<uint32_t>(profileResponse.size() - 4));
	PutBe16(profileResponse.data() + 6, 0x0008);
	profileResponse[11] = static_cast<BYTE>(profileCodes.size() * 4);
	for (size_t i = 0; i < profileCodes.size(); ++i)
		PutBe16(profileResponse.data() + 12 + i * 4, profileCodes[i]);
	DriveCapabilities profileCaps;
	Check(DriveCapabilityParsing::ParseProfileListResponse(
		profileResponse.data(), profileResponse.size(), profileCaps),
		"GET CONFIGURATION Profile List parses successfully");
	Check(profileCaps.currentMediaProfile == 0x0008
		&& profileCaps.currentMediaType == "CD-ROM",
		"Current MMC profile identifies loaded media");
	Check(profileCaps.writesCDR && profileCaps.writesCDRW
		&& profileCaps.writesDVD && profileCaps.writesDVDRAM
		&& profileCaps.writesBD && profileCaps.readsBD,
		"Profile List supplies complete CD, DVD, DVD-RAM, and BD capabilities");

	DriveCapabilities retainedProfileCaps;
	retainedProfileCaps.currentMediaProfile = 0x0040;
	retainedProfileCaps.currentMediaType = "BD-ROM";
	std::vector<BYTE> truncatedProfileResponse = profileResponse;
	truncatedProfileResponse.resize(13);
	Check(!DriveCapabilityParsing::ParseProfileListResponse(
		truncatedProfileResponse.data(), truncatedProfileResponse.size(),
		retainedProfileCaps),
		"Truncated Profile List response is rejected");
	Check(retainedProfileCaps.currentMediaProfile == 0x0040
		&& retainedProfileCaps.currentMediaType == "BD-ROM",
		"Rejected Profile List response does not partially replace current media");
	Check(DriveCapabilityParsing::MediaProfileName(0x001A) == "DVD+RW"
		&& DriveCapabilityParsing::MediaProfileName(0x0043) == "BD-RE",
		"MMC profile names preserve plus-format and Blu-ray distinctions");
	Check(DriveCapabilityParsing::WriteSpeedBaseKB(0x0008) == 176
		&& std::string(DriveCapabilityParsing::WriteSpeedFamilyName(0x0008)) == "CD",
		"A mounted CD-ROM supplies the CD x-rate base for reported write speeds");

	// Feature-specific bits: presence of CD Mastering alone does not imply SAO,
	// and Real-Time Streaming is not a buffer-underrun flag.
	BYTE cdReadFeature[8] = { 0x00, 0x1E, 0, 4, 0x03, 0, 0, 0 };
	BYTE taoFeature[8] = { 0x00, 0x2D, 0, 4, 0x44, 0, 0, 0 };
	BYTE masteringFeature[8] = { 0x00, 0x2E, 0, 4, 0x7D, 0, 0, 0 };
	DriveCapabilities featureCaps;
	Check(DriveCapabilityParsing::ApplyFeatureDescriptor(
		cdReadFeature, sizeof(cdReadFeature), 0x001E, featureCaps)
		&& featureCaps.supportsRawRead && featureCaps.supportsC2ErrorReporting
		&& featureCaps.supportsCDText,
		"CD Read feature uses CD-Text bit 0 and C2 bit 1");
	Check(DriveCapabilityParsing::ApplyFeatureDescriptor(
		taoFeature, sizeof(taoFeature), 0x002D, featureCaps)
		&& featureCaps.supportsWriteTAO && featureCaps.supportsTestWrite
		&& featureCaps.supportsBufferUnderrunProtection,
		"TAO feature parses test-write and BUF flags");
	Check(DriveCapabilityParsing::ApplyFeatureDescriptor(
		masteringFeature, sizeof(masteringFeature), 0x002E, featureCaps)
		&& featureCaps.supportsWriteSAO && featureCaps.supportsWriteRAW
		&& featureCaps.supportsWriteCDText,
		"CD Mastering feature parses SAO, RAW, and R-W flags");

	// GET PERFORMANCE Type 03h: each descriptor is 16 bytes and the write
	// speed is the final 32-bit field. 0x7300 in End LBA would display as 167x
	// if the old 4-byte parser walked into the descriptor interior.
	const std::vector<int> expectedSpeeds = { 706, 1764, 2822, 4234, 5645, 7056, 8467 };
	std::vector<BYTE> performance(
		DriveCapabilityParsing::GET_PERFORMANCE_HEADER_SIZE
		+ expectedSpeeds.size() * DriveCapabilityParsing::WRITE_SPEED_DESCRIPTOR_SIZE, 0);
	PutBe32(performance.data(), static_cast<uint32_t>(performance.size() - 4));
	for (size_t i = 0; i < expectedSpeeds.size(); ++i) {
		const size_t offset = DriveCapabilityParsing::GET_PERFORMANCE_HEADER_SIZE
			+ i * DriveCapabilityParsing::WRITE_SPEED_DESCRIPTOR_SIZE;
		PutBe32(performance.data() + offset + 4, 0x00007300); // not a speed
		PutBe32(performance.data() + offset + 8, 8467);      // read speed
		PutBe32(performance.data() + offset + 12,
			static_cast<uint32_t>(expectedSpeeds[i]));
	}
	std::vector<int> parsedSpeeds;
	Check(DriveCapabilityParsing::ParseWriteSpeedDescriptors(
		performance.data(), performance.size(), parsedSpeeds)
		&& parsedSpeeds == expectedSpeeds,
		"GET PERFORMANCE returns only its 32-bit write-speed fields");
	Check(std::find(parsedSpeeds.begin(), parsedSpeeds.end(), 0x7300)
		== parsedSpeeds.end(),
		"GET PERFORMANCE does not turn End LBA into bogus 167x speed");

	const BYTE crcInput[] = "123456789";
	Check(PreservationCRC32(crcInput, 9) == 0xCBF43926u,
		"CRC32 matches the canonical vector");

	auto arV2Vector = MakeAudioSectors(1);
	PutAudioFrame(arV2Vector, 1, 0xFFFFFFFFu);
	uint32_t arV1 = 0, arV2 = 0;
	Check(AccurateRip::CalculateCRCs(arV2Vector, 2, 3, arV1, arV2) &&
		arV1 == 0xFFFFFFFEu && arV2 == 0xFFFFFFFFu,
		"AccurateRip V2 folds the high product word into the V1 checksum");

	auto arFirstBoundary = MakeAudioSectors(6);
	PutAudioFrame(arFirstBoundary, 2938, 1); // Position 2,939: excluded.
	PutAudioFrame(arFirstBoundary, 2939, 1); // Position 2,940: included.
	Check(AccurateRip::CalculateCRCs(
		arFirstBoundary, 1, 2, arV1, arV2) &&
		arV1 == 2940u && arV2 == 2940u,
		"AccurateRip excludes exactly the first 2,939 frames of track 1");

	auto arLastBoundary = MakeAudioSectors(6);
	PutAudioFrame(arLastBoundary, 587, 1); // Position 588: included.
	PutAudioFrame(arLastBoundary, 588, 1); // Position 589: excluded.
	Check(AccurateRip::CalculateCRCs(
		arLastBoundary, 2, 2, arV1, arV2) &&
		arV1 == 588u && arV2 == 588u,
		"AccurateRip excludes exactly the final 2,940 frames of the last track");

	// Build two contiguous canonical tracks, then model an uncorrected drive
	// capture by moving the complete audio stream 667 stereo frames later. The
	// first track is deliberately only 451 sectors long, so its shifted
	// Frame450 probe has to cross into the next track's contiguous audio.
	constexpr size_t arTrackSectors = 451;
	constexpr size_t arFramesPerSector = AUDIO_SECTOR_SIZE / 4;
	constexpr size_t arVerificationOffset = 667;
	auto arReferenceTrack1 = MakeAudioSectors(arTrackSectors);
	auto arReferenceTrack2 = MakeAudioSectors(arTrackSectors);
	FillDeterministicAudio(arReferenceTrack1, 0x12345678u);
	FillDeterministicAudio(arReferenceTrack2, 0x9ABCDEF0u);

	uint32_t arTrack1V1 = 0, arTrack1V2 = 0;
	uint32_t arTrack2V1 = 0, arTrack2V2 = 0;
	const bool arReferencesCalculated =
		AccurateRip::CalculateCRCs(
			arReferenceTrack1, 1, 2, arTrack1V1, arTrack1V2) &&
		AccurateRip::CalculateCRCs(
			arReferenceTrack2, 2, 2, arTrack2V1, arTrack2V2);
	Check(arReferencesCalculated,
		"AccurateRip offset fixture full-track references are calculable");

	std::vector<std::vector<BYTE>> arCanonicalDisc;
	arCanonicalDisc.reserve(arTrackSectors * 2);
	arCanonicalDisc.insert(arCanonicalDisc.end(),
		arReferenceTrack1.begin(), arReferenceTrack1.end());
	arCanonicalDisc.insert(arCanonicalDisc.end(),
		arReferenceTrack2.begin(), arReferenceTrack2.end());
	auto arCapturedDisc = MakeAudioSectors(arCanonicalDisc.size());
	const size_t arDiscFrames = arCanonicalDisc.size() * arFramesPerSector;
	for (size_t source = 0; source + arVerificationOffset < arDiscFrames;
		++source) {
		PutAudioFrame(arCapturedDisc, source + arVerificationOffset,
			GetAudioFrame(arCanonicalDisc, source));
	}

	DiscInfo arOffsetDisc;
	TrackInfo arTrack1;
	arTrack1.trackNumber = 1;
	arTrack1.startLBA = 0;
	arTrack1.pregapLBA = 0;
	arTrack1.endLBA = static_cast<DWORD>(arTrackSectors - 1);
	arTrack1.isAudio = true;
	TrackInfo arTrack2 = arTrack1;
	arTrack2.trackNumber = 2;
	arTrack2.startLBA = static_cast<DWORD>(arTrackSectors);
	arTrack2.pregapLBA = arTrack2.startLBA;
	arTrack2.endLBA = static_cast<DWORD>(arTrackSectors * 2 - 1);
	arOffsetDisc.tracks = { arTrack1, arTrack2 };
	arOffsetDisc.leadOutLBA = static_cast<DWORD>(arTrackSectors * 2);
	arOffsetDisc.audioLeadOutLBA = arOffsetDisc.leadOutLBA;
	arOffsetDisc.pregapMode = PregapMode::Include;
	arOffsetDisc.rawSectors = std::move(arCapturedDisc);

	AccurateRipPressing arPressing(2);
	arPressing[0].confidence = 11;
	arPressing[0].checksum = arTrack1V1;
	arPressing[0].frame450Checksum =
		CalculateFrame450Reference(arReferenceTrack1);
	arPressing[0].hasFrame450Checksum = true;
	arPressing[1].confidence = 13;
	arPressing[1].checksum = arTrack2V1;
	arPressing[1].frame450Checksum =
		CalculateFrame450Reference(arReferenceTrack2);
	arPressing[1].hasFrame450Checksum = true;
	std::vector<AccurateRipPressing> arPressings{ arPressing };

	DiscInfo arExactDisc = arOffsetDisc;
	arExactDisc.rawSectors = arCanonicalDisc;
	Check(AccurateRip::VerifyCRCs(arExactDisc, arPressings) ==
		AccurateRipVerificationResult::Verified,
		"AccurateRip matches structured full-track references at offset zero");

	Check(AccurateRip::VerifyCRCs(arOffsetDisc, arPressings) ==
		AccurateRipVerificationResult::Verified,
		"AccurateRip uses Frame450 evidence across a track boundary to verify at +667 samples");

	constexpr size_t arExtendedVerificationOffset = 2000;
	auto arExtendedCapturedDisc = MakeAudioSectors(arCanonicalDisc.size());
	for (size_t source = 0;
		source + arExtendedVerificationOffset < arDiscFrames; ++source) {
		PutAudioFrame(arExtendedCapturedDisc,
			source + arExtendedVerificationOffset,
			GetAudioFrame(arCanonicalDisc, source));
	}
	DiscInfo arExtendedOffsetDisc = arOffsetDisc;
	arExtendedOffsetDisc.rawSectors = std::move(arExtendedCapturedDisc);
	Check(AccurateRip::VerifyCRCs(arExtendedOffsetDisc, arPressings) ==
		AccurateRipVerificationResult::Verified,
		"AccurateRip searches the full five-sector offset window");

	auto arLegacyPressings = arPressings;
	for (auto& pressing : arLegacyPressings) {
		for (auto& reference : pressing) {
			reference.frame450Checksum = 0;
			reference.hasFrame450Checksum = false;
		}
	}
	Check(AccurateRip::VerifyCRCs(arExactDisc, arLegacyPressings) ==
		AccurateRipVerificationResult::Verified,
		"AccurateRip verifies legacy records directly at offset zero");
	Check(AccurateRip::VerifyCRCs(arOffsetDisc, arLegacyPressings) ==
		AccurateRipVerificationResult::Inconclusive,
		"AccurateRip reports shifted legacy records without probes as inconclusive");

	auto arZeroDisc = arExactDisc;
	arZeroDisc.rawSectors = MakeAudioSectors(arCanonicalDisc.size());
	auto arZeroChecksumPressings = arPressings;
	for (auto& pressing : arZeroChecksumPressings) {
		for (auto& reference : pressing) reference.checksum = 0;
	}
	Check(AccurateRip::VerifyCRCs(arZeroDisc, arZeroChecksumPressings) ==
		AccurateRipVerificationResult::Inconclusive,
		"AccurateRip rejects zero-checksum sentinel records instead of verifying silent audio");

	auto arZeroConfidencePressings = arPressings;
	for (auto& pressing : arZeroConfidencePressings) {
		for (auto& reference : pressing) reference.confidence = 0;
	}
	Check(AccurateRip::VerifyCRCs(arExactDisc, arZeroConfidencePressings) ==
		AccurateRipVerificationResult::Inconclusive,
		"AccurateRip rejects checksum records with zero confidence");

	auto arInvalidThenValid = arPressings;
	auto arInvalidPressing = arPressing;
	for (auto& reference : arInvalidPressing) reference.checksum = 0;
	arInvalidThenValid.insert(arInvalidThenValid.begin(), arInvalidPressing);
	Check(AccurateRip::VerifyCRCs(arOffsetDisc, arInvalidThenValid) ==
		AccurateRipVerificationResult::Verified,
		"AccurateRip ignores sentinel records when a usable pressing is also present");

	auto arFrame450Only = arPressings;
	uint32_t wrongFullTrackChecksum = arTrack2V1 ^ 0xA5A5A5A5u;
	while (wrongFullTrackChecksum == arTrack2V1 ||
		wrongFullTrackChecksum == arTrack2V2) {
		++wrongFullTrackChecksum;
	}
	arFrame450Only[0][1].checksum = wrongFullTrackChecksum;
	std::ostringstream arPartialLog;
	std::streambuf* partialCoutBuffer = std::cout.rdbuf(arPartialLog.rdbuf());
	const auto arPartialResult =
		AccurateRip::VerifyCRCs(arOffsetDisc, arFrame450Only);
	std::cout.rdbuf(partialCoutBuffer);
	Check(arPartialResult ==
		AccurateRipVerificationResult::Mismatch,
		"AccurateRip does not verify from Frame450 evidence without every full-track checksum");
	Check(arPartialLog.str().find(
		"Full-track CRCs at offset +667 matched 1/2") != std::string::npos &&
		arPartialLog.str().find(
			"[OK - AR v1, record #1, confidence 11, offset +667]") !=
			std::string::npos &&
		arPartialLog.str().find("[MISMATCH at offset +667]") !=
			std::string::npos,
		"AccurateRip reports genuine per-track matches at the strongest candidate offset");

	// Make every Frame450 candidate equally strong while preserving exactly one
	// full-track match at +667. The work cap must report Inconclusive rather than
	// declaring a mismatch before that offset is reached.
	constexpr size_t arAmbiguousTrackSectors = 500;
	constexpr size_t arSearchRadius = 3 * arFramesPerSector - 1;
	constexpr size_t arFrame450Start = 450 * arFramesPerSector;
	constexpr size_t arAmbiguousBandStart =
		arFrame450Start - arSearchRadius - arVerificationOffset;
	constexpr size_t arAmbiguousBandEnd =
		arFrame450Start + arSearchRadius - arVerificationOffset +
		arFramesPerSector;
	auto arAmbiguousCanonical = MakeAudioSectors(arAmbiguousTrackSectors);
	for (size_t frame = arAmbiguousBandStart;
		frame < arAmbiguousBandEnd; ++frame) {
		PutAudioFrame(arAmbiguousCanonical, frame, 1u);
	}
	uint32_t arAmbiguousV1 = 0, arAmbiguousV2 = 0;
	Check(AccurateRip::CalculateCRCs(
		arAmbiguousCanonical, 1, 1, arAmbiguousV1, arAmbiguousV2),
		"AccurateRip tied-candidate fixture checksum is calculable");
	Check(CalculateFrame450Reference(arAmbiguousCanonical) == 173166u,
		"AccurateRip tied-candidate fixture has a non-sentinel Frame450 checksum");

	auto arAmbiguousCaptured = MakeAudioSectors(arAmbiguousTrackSectors);
	const size_t arAmbiguousFrames =
		arAmbiguousTrackSectors * arFramesPerSector;
	for (size_t source = 0;
		source + arVerificationOffset < arAmbiguousFrames; ++source) {
		PutAudioFrame(arAmbiguousCaptured, source + arVerificationOffset,
			GetAudioFrame(arAmbiguousCanonical, source));
	}
	DiscInfo arAmbiguousDisc;
	TrackInfo arAmbiguousTrack;
	arAmbiguousTrack.trackNumber = 1;
	arAmbiguousTrack.startLBA = 0;
	arAmbiguousTrack.pregapLBA = 0;
	arAmbiguousTrack.endLBA =
		static_cast<DWORD>(arAmbiguousTrackSectors - 1);
	arAmbiguousTrack.isAudio = true;
	arAmbiguousDisc.tracks = { arAmbiguousTrack };
	arAmbiguousDisc.leadOutLBA =
		static_cast<DWORD>(arAmbiguousTrackSectors);
	arAmbiguousDisc.audioLeadOutLBA = arAmbiguousDisc.leadOutLBA;
	arAmbiguousDisc.pregapMode = PregapMode::Include;
	arAmbiguousDisc.rawSectors = std::move(arAmbiguousCaptured);
	AccurateRipPressing arAmbiguousPressing(1);
	arAmbiguousPressing[0].confidence = 9;
	arAmbiguousPressing[0].checksum = arAmbiguousV1;
	arAmbiguousPressing[0].frame450Checksum =
		CalculateFrame450Reference(arAmbiguousCanonical);
	arAmbiguousPressing[0].hasFrame450Checksum = true;

	std::ostringstream arAmbiguousLog;
	std::streambuf* previousCoutBuffer = std::cout.rdbuf(arAmbiguousLog.rdbuf());
	const auto arAmbiguousResult = AccurateRip::VerifyCRCs(
		arAmbiguousDisc, { arAmbiguousPressing });
	std::cout.rdbuf(previousCoutBuffer);
	Check(arAmbiguousResult == AccurateRipVerificationResult::Inconclusive,
		"AccurateRip reports a capped tied-candidate search as inconclusive");
	Check(arAmbiguousLog.str().find(
		"best shared offset 0 samples matched 1/1") != std::string::npos,
		"AccurateRip diagnostic tie-breaking prefers the closest shared offset");

	DiscInfo mixedModeIds;
	TrackInfo mixedAudio;
	mixedAudio.trackNumber = 1;
	mixedAudio.startLBA = 0;
	mixedAudio.pregapLBA = 0;
	mixedAudio.endLBA = 999;
	mixedAudio.isAudio = true;
	TrackInfo mixedData;
	mixedData.trackNumber = 2;
	mixedData.startLBA = 1000;
	mixedData.pregapLBA = 1000;
	mixedData.endLBA = 1999;
	mixedData.isAudio = false;
	mixedData.session = 2;
	mixedModeIds.tracks = { mixedAudio, mixedData };
	mixedModeIds.audioLeadOutLBA = 1000;
	mixedModeIds.leadOutLBA = 2000;
	Check(AccurateRip::CalculateDiscID1(mixedModeIds) == 2000u &&
		AccurateRip::CalculateDiscID2(mixedModeIds) == 4001u &&
		AccurateRip::CalculateCDDBID(mixedModeIds) == 0x08001A02u,
		"AccurateRip mixed-mode IDs use overall lead-out and physical CDDB track count");

	auto mode1 = MakeMode1Sector(1234);
	auto valid = ValidateRawDataSector(mode1.data(), 1234);
	Check(valid.mode == RawDataMode::Mode1, "Mode 1 is detected");
	Check(valid.syncValid && valid.addressValid && valid.edcValid &&
		valid.eccValid && valid.valid, "Mode 1 sync/MSF/EDC/ECC validate");
	mode1[100] ^= 0x40;
	auto corrupt = ValidateRawDataSector(mode1.data(), 1234);
	Check(!corrupt.edcValid && !corrupt.eccValid && !corrupt.valid,
		"Corrupt raw sector fails EDC and ECC");

	auto mode2Form1 = MakeMode2Sector(2345, false);
	auto validForm1 = ValidateRawDataSector(mode2Form1.data(), 2345);
	Check(validForm1.mode == RawDataMode::Mode2Form1 &&
		validForm1.subheaderValid && validForm1.edcValid &&
		validForm1.eccValid && validForm1.valid,
		"Mode 2 Form 1 subheader/EDC/ECC validate");

	auto mode2Form2 = MakeMode2Sector(3456, true);
	auto validForm2 = ValidateRawDataSector(mode2Form2.data(), 3456);
	Check(validForm2.mode == RawDataMode::Mode2Form2 &&
		validForm2.subheaderValid && validForm2.edcValid &&
		!validForm2.eccPresent && validForm2.valid,
		"Mode 2 Form 2 subheader/EDC validate without ECC");
	mode2Form2[20] ^= 0x01;
	auto corruptForm2 = ValidateRawDataSector(mode2Form2.data(), 3456);
	Check(!corruptForm2.subheaderValid && !corruptForm2.edcValid &&
		!corruptForm2.valid,
		"Corrupt Mode 2 Form 2 subheader and payload fail validation");

	auto mode2Formless = MakeMode2FormlessSector(4567);
	auto validFormless = ValidateRawDataSector(mode2Formless.data(), 4567);
	Check(validFormless.mode == RawDataMode::Mode2Formless &&
		validFormless.subheaderValid && !validFormless.edcPresent &&
		!validFormless.eccPresent && validFormless.valid,
		"Original formless Mode 2 validates without XA EDC/ECC");
	DiscInfo formlessDisc = MakeDisc();
	formlessDisc.tracks[0].isAudio = false;
	formlessDisc.tracks[0].endLBA = 0;
	formlessDisc.leadOutLBA = 1;
	formlessDisc.includeSubchannel = false;
	auto mode2FormlessAtZero = MakeMode2FormlessSector(0);
	formlessDisc.rawSectors.emplace_back(
		mode2FormlessAtZero.begin(), mode2FormlessAtZero.end());
	DataValidationSummary formlessSummary;
	Check(ValidateDataTracks(formlessDisc, formlessSummary) &&
		formlessSummary.validSectors == 1 &&
		formlessSummary.mode2FormlessSectors == 1 &&
		formlessSummary.invalidSubheader == 0 &&
		formlessDisc.tracks[0].mode == 2,
		"Formless Mode 2 is summarized as valid Mode 2 without XA errors");

	std::filesystem::path temp = std::filesystem::temp_directory_path() /
		(L"OptiScanPreservationTests-" + std::to_wstring(GetCurrentProcessId()));
	std::filesystem::create_directories(temp);
	std::filesystem::path abc = temp / L"abc.bin";
	{
		std::ofstream output(abc, std::ios::binary);
		output << "abc";
	}
	FileHashSet hashes;
	Check(HashFileForPreservation(abc.wstring(), hashes), "File hashing succeeds");
	Check(hashes.size == 3 && hashes.crc32 == 0x352441C2u,
		"File size and CRC32 are correct");
	Check(hashes.md5 == "900150983cd24fb0d6963f7d28e17f72" &&
		hashes.sha1 == "a9993e364706816aba3e25717850c26c9cd0d89d" &&
		hashes.sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		"MD5/SHA-1/SHA-256 match known vectors");

	DiscInfo disc = MakeDisc();
	uint64_t signature = CalculateDiscSignature(disc);
	DiscInfo changed = disc;
	changed.tracks[0].endLBA++;
	Check(signature == CalculateDiscSignature(disc) &&
		signature != CalculateDiscSignature(changed),
		"Disc signature is stable and TOC-sensitive");
	Check(CalculateDiscSignature(disc, 0x1234) ==
		CalculateDiscSignature(disc, 0x1234) &&
		CalculateDiscSignature(disc, 0x1234) !=
		CalculateDiscSignature(disc, 0x5678),
		"Disc signature is content-fingerprint-sensitive");

	std::filesystem::path imageBase = temp / L"image";
	for (const auto* suffix : { L".bin", L".cue", L".sub",
		L"_track9_pregap.bin" }) {
		std::ofstream output(imageBase.wstring() + suffix, std::ios::binary);
		output.put('x');
	}
	auto currentArtifacts =
		CollectPreservationArtifacts(imageBase.wstring(), disc);
	Check(currentArtifacts.size() == 3 &&
		std::none_of(currentArtifacts.begin(), currentArtifacts.end(),
			[](const std::wstring& path) {
				return path.ends_with(L"_track9_pregap.bin");
			}),
		"Artifact collection excludes stale outputs from another rip layout");
	disc.includeSubchannel = false;
	currentArtifacts = CollectPreservationArtifacts(imageBase.wstring(), disc);
	Check(currentArtifacts.size() == 2,
		"Artifact collection excludes stale subchannel output when disabled");
	disc.includeSubchannel = true;

	std::wstring checkpointBase = (temp / L"resume").wstring();
	std::vector<BYTE> sector(RAW_SECTOR_SIZE, 0x5A);
	RecoveryCheckpointEntry entry;
	entry.lba = 0;
	entry.track = 1;
	entry.isAudio = true;
	entry.status = CheckpointSectorStatus::Recovered;
	entry.passesUsed = 4;
	entry.confirmedBytes = AUDIO_SECTOR_SIZE;
	entry.subchannelValid = true;
	constexpr uint64_t contentFingerprint = 0x0123456789ABCDEFull;
	{
		RecoveryCheckpoint checkpoint;
		Check(checkpoint.Open(checkpointBase, disc, 2, "Drive A", 30,
			contentFingerprint),
			"Recovery checkpoint is created");
		Check(checkpoint.WriteSector(0, sector, entry) && checkpoint.Flush(),
			"Recovery checkpoint sector is durable");
	}
	{
		RecoveryCheckpoint checkpoint;
		Check(checkpoint.Open(checkpointBase, disc, 2, "Drive B", 667,
			contentFingerprint) &&
			checkpoint.WasResumed(), "Checkpoint resumes on another drive");
		Check(checkpoint.ReferenceDriveOffset() == 30 &&
			checkpoint.ReferenceDrive() == "Drive A",
			"Original drive coordinate is preserved");
		std::vector<BYTE> loaded;
		RecoveryCheckpointEntry loadedEntry;
		Check(checkpoint.ReadSector(0, loaded, loadedEntry) &&
			loaded == sector &&
			loadedEntry.status == CheckpointSectorStatus::Recovered,
			"Checkpoint round-trip preserves bytes and status");
	}
	{
		std::fstream partial(checkpointBase + L".recovery.partial.bin",
			std::ios::binary | std::ios::in | std::ios::out);
		char byte = 0;
		partial.seekg(10);
		partial.read(&byte, 1);
		byte ^= 0x01;
		partial.seekp(10);
		partial.write(&byte, 1);
	}
	{
		RecoveryCheckpoint checkpoint;
		Check(checkpoint.Open(checkpointBase, disc, 2, "Drive B", 667,
			contentFingerprint) && checkpoint.WasResumed(),
			"Checkpoint metadata remains resumable after payload damage");
		std::vector<BYTE> loaded;
		RecoveryCheckpointEntry loadedEntry;
		Check(checkpoint.LoadedSectorCount() == 0 &&
			!checkpoint.ReadSector(0, loaded, loadedEntry),
			"Corrupt checkpoint payload is rejected instead of resumed");
	}
	Check(RemoveRecoveryCheckpointFiles(checkpointBase),
		"Recovery checkpoint sidecars are removed after finalization");

	std::wstring invalidCheckpointBase = (temp / L"invalid_resume").wstring();
	{
		std::ofstream state(invalidCheckpointBase + L".recovery.state",
			std::ios::binary);
		std::ofstream partial(invalidCheckpointBase + L".recovery.partial.bin",
			std::ios::binary);
		state << "invalid";
		partial << "partial";
	}
	{
		RecoveryCheckpoint checkpoint;
		Check(checkpoint.Open(invalidCheckpointBase, disc, 2, "Drive A", 30,
			contentFingerprint) && checkpoint.PreservedInvalidFiles(),
			"Invalid checkpoint is preserved before a new checkpoint is created");
		Check(std::filesystem::exists(
			invalidCheckpointBase + L".recovery.state.invalid") &&
			std::filesystem::exists(
				invalidCheckpointBase + L".recovery.partial.bin.invalid"),
			"Preserved invalid checkpoint sidecars remain available for diagnosis");
	}
	Check(RemoveRecoveryCheckpointFiles(invalidCheckpointBase),
		"Replacement checkpoint sidecars are removable");

	PreservationManifestContext manifest;
	manifest.workflow = "Automated test";
	manifest.artifacts.push_back(abc.wstring());
	manifest.verificationStatus = "VERIFIED WITH CAUTION";
	manifest.verificationMethod = "Physical byte comparison";
	manifest.verificationNote = "A retry was required.";
	manifest.verificationAffectedTracks = { 1, 2 };
	std::filesystem::path manifestPath = temp / L"manifest.json";
	Check(WritePreservationManifest(disc, manifest, manifestPath.wstring()),
		"Preservation manifest is generated");
	std::ifstream manifestInput(manifestPath);
	std::string manifestText((std::istreambuf_iterator<char>(manifestInput)),
		std::istreambuf_iterator<char>());
	Check(manifestText.find(hashes.sha256) != std::string::npos &&
		manifestText.find("\"workflow\": \"Automated test\"") != std::string::npos &&
		manifestText.find("\"status\": \"VERIFIED WITH CAUTION\"") != std::string::npos &&
		manifestText.find("\"affected_tracks\": [1, 2]") != std::string::npos,
		"Manifest records workflow, artifact hashes, and verification evidence");
	PreservationManifestContext missingManifest;
	missingManifest.workflow = "Missing artifact test";
	missingManifest.artifacts.push_back((temp / L"missing.bin").wstring());
	Check(!WritePreservationManifest(disc, missingManifest,
		(temp / L"missing.manifest.json").wstring()),
		"Manifest creation fails when an expected artifact is missing");

	manifestInput.close();
	std::error_code cleanupError;
	std::filesystem::remove_all(temp, cleanupError);
	Check(!cleanupError, "Test artifacts are cleaned up");
	std::cout << "\n" << (failures == 0 ? "All tests passed.\n" : "Tests failed.\n");
	return failures == 0 ? 0 : 1;
}
