#define NOMINMAX
#include "../Preservation.h"
#include "../RecoveryCheckpoint.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
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
	const BYTE crcInput[] = "123456789";
	Check(PreservationCRC32(crcInput, 9) == 0xCBF43926u,
		"CRC32 matches the canonical vector");

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
	std::filesystem::path manifestPath = temp / L"manifest.json";
	Check(WritePreservationManifest(disc, manifest, manifestPath.wstring()),
		"Preservation manifest is generated");
	std::ifstream manifestInput(manifestPath);
	std::string manifestText((std::istreambuf_iterator<char>(manifestInput)),
		std::istreambuf_iterator<char>());
	Check(manifestText.find(hashes.sha256) != std::string::npos &&
		manifestText.find("\"workflow\": \"Automated test\"") != std::string::npos,
		"Manifest records workflow and artifact hashes");
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
