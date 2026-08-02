// ============================================================================
// Preservation.cpp - Archival hashes, raw-sector checks, and manifest output
// ============================================================================
#define NOMINMAX
#include "Preservation.h"
#include "AccurateRip.h"
#include "Constants.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")

namespace {

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

uint32_t ComputeEdc(const BYTE* data, size_t size) {
	static const auto table = BuildEdcTable();
	uint32_t edc = 0;
	for (size_t i = 0; i < size; ++i)
		edc = (edc >> 8) ^ table[(edc ^ data[i]) & 0xFF];
	return edc;
}

uint32_t ReadLe32(const BYTE* p) {
	return static_cast<uint32_t>(p[0]) |
		(static_cast<uint32_t>(p[1]) << 8) |
		(static_cast<uint32_t>(p[2]) << 16) |
		(static_cast<uint32_t>(p[3]) << 24);
}

bool IsValidBcd(BYTE value) {
	return (value & 0x0F) <= 9 && ((value >> 4) & 0x0F) <= 9;
}

bool IsSync(const BYTE* sector) {
	if (sector[0] != 0x00 || sector[11] != 0x00) return false;
	for (int i = 1; i < 11; ++i)
		if (sector[i] != 0xFF) return false;
	return true;
}

void BuildEccTables(std::array<BYTE, 256>& forward, std::array<BYTE, 256>& backward) {
	for (int i = 0; i < 256; ++i) {
		int j = i << 1;
		if (j & 0x100) j ^= 0x11D;
		forward[i] = static_cast<BYTE>(j);
		backward[i ^ j] = static_cast<BYTE>(i);
	}
}

void ComputeEcc(const BYTE* source, int majorCount, int minorCount,
	int majorMult, int minorInc, BYTE* destination) {
	static std::array<BYTE, 256> forward{};
	static std::array<BYTE, 256> backward{};
	static bool initialized = false;
	if (!initialized) {
		BuildEccTables(forward, backward);
		initialized = true;
	}

	const int size = majorCount * minorCount;
	for (int major = 0; major < majorCount; ++major) {
		int index = (major >> 1) * majorMult + (major & 1);
		BYTE a = 0;
		BYTE b = 0;
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

bool ValidateEcc(const BYTE* sector, bool mode2) {
	std::array<BYTE, AUDIO_SECTOR_SIZE> work{};
	std::copy_n(sector, work.size(), work.data());
	if (mode2)
		std::fill(work.begin() + 12, work.begin() + 16, static_cast<BYTE>(0));

	std::array<BYTE, 172> p{};
	std::array<BYTE, 104> q{};
	ComputeEcc(work.data() + 12, 86, 24, 2, 86, p.data());
	std::copy(p.begin(), p.end(), work.begin() + 2076);
	ComputeEcc(work.data() + 12, 52, 43, 86, 88, q.data());
	return std::equal(p.begin(), p.end(), sector + 2076) &&
		std::equal(q.begin(), q.end(), sector + 2248);
}

std::string Hex(const BYTE* bytes, size_t size) {
	std::ostringstream out;
	out << std::hex << std::setfill('0');
	for (size_t i = 0; i < size; ++i)
		out << std::setw(2) << static_cast<unsigned>(bytes[i]);
	return out.str();
}

bool HashWithAlgorithm(const BYTE* data, ULONG size, BCRYPT_HASH_HANDLE hash) {
	return BCryptHashData(hash, const_cast<PUCHAR>(data), size, 0) >= 0;
}

std::string JsonEscape(const std::string& value) {
	std::ostringstream out;
	for (unsigned char c : value) {
		switch (c) {
		case '"': out << "\\\""; break;
		case '\\': out << "\\\\"; break;
		case '\b': out << "\\b"; break;
		case '\f': out << "\\f"; break;
		case '\n': out << "\\n"; break;
		case '\r': out << "\\r"; break;
		case '\t': out << "\\t"; break;
		default:
			if (c < 0x20)
				out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
					<< static_cast<int>(c) << std::dec;
			else
				out << static_cast<char>(c);
		}
	}
	return out.str();
}

std::string WideToUtf8(const std::wstring& value) {
	if (value.empty()) return {};
	int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(),
		static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) return {};
	std::string result(static_cast<size_t>(bytes), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
		result.data(), bytes, nullptr, nullptr);
	return result;
}

struct SectorLocation {
	const TrackInfo* track = nullptr;
	DWORD lba = 0;
	const std::vector<BYTE>* bytes = nullptr;
};

std::vector<SectorLocation> BuildSectorMap(const DiscInfo& disc) {
	std::vector<SectorLocation> result;
	result.reserve(disc.rawSectors.size());
	size_t index = 0;
	for (const auto& track : disc.tracks) {
		if (disc.selectedSession > 0 && track.session != disc.selectedSession) continue;
		DWORD start = disc.pregapMode == PregapMode::Skip
			? track.startLBA : track.pregapLBA;
		if (track.endLBA < start) continue;
		for (DWORD lba = start;; ++lba) {
			if (index >= disc.rawSectors.size()) return result;
			result.push_back({ &track, lba, &disc.rawSectors[index++] });
			if (lba == track.endLBA) break;
		}
	}
	return result;
}

bool IsSilentFrame(const BYTE* frame) {
	const int16_t left = static_cast<int16_t>(
		static_cast<uint16_t>(frame[0]) | (static_cast<uint16_t>(frame[1]) << 8));
	const int16_t right = static_cast<int16_t>(
		static_cast<uint16_t>(frame[2]) | (static_cast<uint16_t>(frame[3]) << 8));
	return std::abs(static_cast<int>(left)) <= 2 &&
		std::abs(static_cast<int>(right)) <= 2;
}

void AddExpectedArtifact(std::vector<std::wstring>& artifacts,
	const std::wstring& path) {
	artifacts.push_back(path);
}

} // namespace

uint32_t PreservationCRC32(const BYTE* data, size_t size, uint32_t seed) {
	static std::array<uint32_t, 256> table = [] {
		std::array<uint32_t, 256> value{};
		for (uint32_t i = 0; i < value.size(); ++i) {
			uint32_t c = i;
			for (int bit = 0; bit < 8; ++bit)
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			value[i] = c;
		}
		return value;
	}();

	uint32_t crc = seed ^ 0xFFFFFFFFu;
	for (size_t i = 0; i < size; ++i)
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

bool HashFileForPreservation(const std::wstring& path, FileHashSet& hashes) {
	hashes = {};
	std::ifstream input(std::filesystem::path(path), std::ios::binary);
	if (!input) return false;

	struct Algorithm {
		LPCWSTR name;
		BCRYPT_ALG_HANDLE provider = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		std::vector<BYTE> object;
		std::vector<BYTE> digest;
	};
	std::array<Algorithm, 3> algorithms{ {
		{ BCRYPT_MD5_ALGORITHM }, { BCRYPT_SHA1_ALGORITHM },
		{ BCRYPT_SHA256_ALGORITHM }
	} };

	auto cleanup = [&] {
		for (auto& item : algorithms) {
			if (item.hash) BCryptDestroyHash(item.hash);
			if (item.provider) BCryptCloseAlgorithmProvider(item.provider, 0);
		}
	};

	for (auto& item : algorithms) {
		if (BCryptOpenAlgorithmProvider(&item.provider, item.name, nullptr, 0) < 0) {
			cleanup();
			return false;
		}
		ULONG objectBytes = 0, digestBytes = 0, returned = 0;
		if (BCryptGetProperty(item.provider, BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returned, 0) < 0 ||
			BCryptGetProperty(item.provider, BCRYPT_HASH_LENGTH,
				reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes), &returned, 0) < 0) {
			cleanup();
			return false;
		}
		item.object.resize(objectBytes);
		item.digest.resize(digestBytes);
		if (BCryptCreateHash(item.provider, &item.hash, item.object.data(),
			objectBytes, nullptr, 0, 0) < 0) {
			cleanup();
			return false;
		}
	}

	// Keep the 1 MiB streaming buffer off the Windows thread stack (which is
	// commonly only 1 MiB for GUI and test processes).
	std::vector<BYTE> buffer(1024 * 1024);
	uint32_t crcState = 0;
	while (input) {
		input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
		std::streamsize got = input.gcount();
		if (got <= 0) break;
		hashes.size += static_cast<uint64_t>(got);
		crcState = PreservationCRC32(buffer.data(), static_cast<size_t>(got), crcState);
		for (auto& item : algorithms) {
			if (!HashWithAlgorithm(buffer.data(),
				static_cast<ULONG>(got), item.hash)) {
				cleanup();
				return false;
			}
		}
	}
	if (!input.eof()) {
		cleanup();
		return false;
	}

	for (auto& item : algorithms) {
		if (BCryptFinishHash(item.hash, item.digest.data(),
			static_cast<ULONG>(item.digest.size()), 0) < 0) {
			cleanup();
			return false;
		}
	}
	hashes.crc32 = crcState;
	hashes.md5 = Hex(algorithms[0].digest.data(), algorithms[0].digest.size());
	hashes.sha1 = Hex(algorithms[1].digest.data(), algorithms[1].digest.size());
	hashes.sha256 = Hex(algorithms[2].digest.data(), algorithms[2].digest.size());
	cleanup();
	return true;
}

RawSectorValidation ValidateRawDataSector(const BYTE* sector, DWORD expectedLBA) {
	RawSectorValidation result;
	if (!sector) return result;
	result.syncValid = IsSync(sector);
	if (!result.syncValid) return result;

	if (IsValidBcd(sector[12]) && IsValidBcd(sector[13]) && IsValidBcd(sector[14])) {
		int actual = (BcdToBin(sector[12]) * 60 + BcdToBin(sector[13])) * 75 +
			BcdToBin(sector[14]) - 150;
		result.addressDeltaSectors = actual - static_cast<int>(expectedLBA);
		result.addressValid = result.addressDeltaSectors == 0;
	}

	if (sector[15] == 1) {
		result.mode = RawDataMode::Mode1;
		result.edcPresent = true;
		result.edcValid = ComputeEdc(sector, 2064) == ReadLe32(sector + 2064);
		result.eccPresent = true;
		result.eccValid = ValidateEcc(sector, false);
		result.valid = result.syncValid && result.addressValid &&
			result.edcValid && result.eccValid;
		return result;
	}

	if (sector[15] == 2) {
		result.subheaderValid = std::equal(sector + 16, sector + 20, sector + 20);
		if (!result.subheaderValid) {
			int differingSubheaderBytes = 0;
			for (int i = 0; i < 4; ++i) {
				if (sector[16 + i] != sector[20 + i]) differingSubheaderBytes++;
			}
			if (differingSubheaderBytes <= 1) {
				// A near-match is overwhelmingly more likely to be a damaged
				// duplicated XA subheader than a formless payload. Preserve
				// that structural failure instead of accepting the sector.
				result.mode = (sector[18] & 0x20) != 0
					? RawDataMode::Mode2Form2
					: RawDataMode::Mode2Form1;
				return result;
			}
			// Original Yellow Book Mode 2 ("formless") carries 2336 user bytes
			// without the duplicated XA subheader or an EDC/ECC field.
			result.mode = RawDataMode::Mode2Formless;
			result.subheaderValid = true; // Not present, therefore not applicable.
			result.valid = result.syncValid && result.addressValid;
			return result;
		}
		result.edcPresent = true;
		const bool form2 = (sector[18] & 0x20) != 0;
		if (form2) {
			result.mode = RawDataMode::Mode2Form2;
			result.edcValid = ComputeEdc(sector + 16, 2332) == ReadLe32(sector + 2348);
			result.eccPresent = false;
			result.eccValid = true;
		}
		else {
			result.mode = RawDataMode::Mode2Form1;
			result.edcValid = ComputeEdc(sector + 16, 2056) == ReadLe32(sector + 2072);
			result.eccPresent = true;
			result.eccValid = ValidateEcc(sector, true);
		}
		result.valid = result.syncValid && result.addressValid &&
			result.subheaderValid && result.edcValid && result.eccValid;
	}
	return result;
}

bool ValidateDataTracks(DiscInfo& disc, DataValidationSummary& summary) {
	summary = {};
	auto sectors = BuildSectorMap(disc);
	std::map<int, std::map<RawDataMode, int>> modesByTrack;
	std::map<int, TrackInfo*> tracksByNumber;
	for (auto& track : disc.tracks)
		tracksByNumber[track.trackNumber] = &track;

	for (const auto& location : sectors) {
		if (!location.track || location.track->isAudio) continue;
		if (!location.bytes || location.bytes->size() < AUDIO_SECTOR_SIZE) continue;
		RawSectorValidation validation =
			ValidateRawDataSector(location.bytes->data(), location.lba);
		summary.sectors++;
		if (validation.valid) summary.validSectors++;
		if (!validation.syncValid) summary.invalidSync++;
		if (!validation.addressValid) summary.invalidAddress++;
		if (!validation.subheaderValid) summary.invalidSubheader++;
		if (validation.edcPresent && !validation.edcValid) summary.invalidEdc++;
		if (validation.eccPresent && !validation.eccValid) summary.invalidEcc++;
		if (!validation.valid && summary.invalidLBAs.size() < 4096)
			summary.invalidLBAs.push_back(location.lba);

		switch (validation.mode) {
		case RawDataMode::Mode1: summary.mode1Sectors++; break;
		case RawDataMode::Mode2Formless: summary.mode2FormlessSectors++; break;
		case RawDataMode::Mode2Form1: summary.mode2Form1Sectors++; break;
		case RawDataMode::Mode2Form2: summary.mode2Form2Sectors++; break;
		default: break;
		}
		modesByTrack[location.track->trackNumber][validation.mode]++;
	}

	summary.tracks = static_cast<int>(modesByTrack.size());
	for (const auto& [trackNumber, counts] : modesByTrack) {
		auto trackIt = tracksByNumber.find(trackNumber);
		if (trackIt == tracksByNumber.end()) continue;
		RawDataMode bestMode = RawDataMode::Unknown;
		int bestCount = 0;
		for (const auto& [mode, count] : counts) {
			if (mode != RawDataMode::Unknown && count > bestCount) {
				bestMode = mode;
				bestCount = count;
			}
		}
		if (bestMode == RawDataMode::Mode1) trackIt->second->mode = 1;
		else if (bestMode == RawDataMode::Mode2Formless ||
			bestMode == RawDataMode::Mode2Form1 ||
			bestMode == RawDataMode::Mode2Form2) trackIt->second->mode = 2;
	}
	return summary.sectors > 0;
}

PreservationOffsetResult AnalyzePreservationWriteOffset(const DiscInfo& disc) {
	PreservationOffsetResult result;
	auto sectors = BuildSectorMap(disc);

	std::map<int, int> addressDeltas;
	for (const auto& location : sectors) {
		if (!location.track || location.track->isAudio || !location.bytes ||
			location.bytes->size() < AUDIO_SECTOR_SIZE) continue;
		RawSectorValidation validation =
			ValidateRawDataSector(location.bytes->data(), location.lba);
		if (validation.syncValid && validation.addressDeltaSectors != 0)
			addressDeltas[validation.addressDeltaSectors]++;
	}
	for (const auto& [delta, count] : addressDeltas) {
		if (count >= 3) {
			result.detected = true;
			result.sampleOffset = delta * 588;
			result.confidencePercent = 98;
			result.evidenceCount = count;
			result.method = "raw data-header MSF alignment";
			result.note = "Preservation-only estimate; normal audio/AccurateRip data was not shifted.";
			return result;
		}
	}

	std::map<DWORD, const std::vector<BYTE>*> audioByLba;
	for (const auto& location : sectors)
		if (location.track && location.track->isAudio && location.bytes &&
			location.bytes->size() >= AUDIO_SECTOR_SIZE)
			audioByLba[location.lba] = location.bytes;

	std::vector<int> candidates;
	for (const auto& track : disc.tracks) {
		if (!track.isAudio || track.pregapLBA >= track.startLBA) continue;
		const int window = 588;
		bool found = false;
		int transition = 0;
		int silentRun = 0;
		for (int relative = -window; relative <= window && !found; ++relative) {
			long long absoluteSample =
				static_cast<long long>(track.startLBA) * 588 + relative;
			if (absoluteSample < 0) continue;
			DWORD lba = static_cast<DWORD>(absoluteSample / 588);
			int sample = static_cast<int>(absoluteSample % 588);
			auto it = audioByLba.find(lba);
			if (it == audioByLba.end()) {
				silentRun = 0;
				continue;
			}
			const BYTE* frame = it->second->data() + sample * 4;
			if (IsSilentFrame(frame)) {
				silentRun++;
			}
			else if (silentRun >= 32) {
				transition = relative;
				found = true;
			}
			else {
				silentRun = 0;
			}
		}
		if (found && std::abs(transition) <= window)
			candidates.push_back(transition);
	}

	if (candidates.size() >= 2) {
		std::sort(candidates.begin(), candidates.end());
		int median = candidates[candidates.size() / 2];
		int agreeing = static_cast<int>(std::count_if(candidates.begin(), candidates.end(),
			[&](int value) { return std::abs(value - median) <= 4; }));
		if (agreeing >= 2 && agreeing * 3 >= static_cast<int>(candidates.size()) * 2) {
			result.detected = true;
			result.sampleOffset = median;
			result.confidencePercent = std::min(80, 55 + agreeing * 5);
			result.evidenceCount = agreeing;
			result.method = "silent INDEX 00 boundary consensus";
			result.note = "Heuristic preservation estimate; it is recorded but never auto-applied.";
			return result;
		}
	}

	result.note = "No repeatable preservation write-offset signature was found.";
	return result;
}

uint64_t CalculateDiscSignature(const DiscInfo& disc,
	uint64_t contentFingerprint) {
	uint64_t hash = 1469598103934665603ull;
	auto mix = [&](uint64_t value) {
		for (int i = 0; i < 8; ++i) {
			hash ^= static_cast<BYTE>((value >> (i * 8)) & 0xFF);
			hash *= 1099511628211ull;
		}
	};
	mix(disc.selectedSession);
	mix(static_cast<int>(disc.pregapMode));
	mix(disc.leadOutLBA);
	mix(contentFingerprint);
	for (const auto& track : disc.tracks) {
		mix(track.trackNumber);
		mix(track.startLBA);
		mix(track.endLBA);
		mix(track.pregapLBA);
		mix(track.isAudio ? 1 : 0);
		mix(track.session);
	}
	return hash;
}

bool WriteDataValidationReport(const DataValidationSummary& summary,
	const std::wstring& filename) {
	std::ofstream out(std::filesystem::path(filename), std::ios::trunc);
	if (!out) return false;
	out << "# OptiScan raw data-track validation\n"
		<< "tracks=" << summary.tracks << "\n"
		<< "sectors=" << summary.sectors << "\n"
		<< "valid_sectors=" << summary.validSectors << "\n"
		<< "invalid_sync=" << summary.invalidSync << "\n"
		<< "invalid_address=" << summary.invalidAddress << "\n"
		<< "invalid_subheader=" << summary.invalidSubheader << "\n"
		<< "invalid_edc=" << summary.invalidEdc << "\n"
		<< "invalid_ecc=" << summary.invalidEcc << "\n"
		<< "mode1=" << summary.mode1Sectors << "\n"
		<< "mode2_formless=" << summary.mode2FormlessSectors << "\n"
		<< "mode2_form1=" << summary.mode2Form1Sectors << "\n"
		<< "mode2_form2=" << summary.mode2Form2Sectors << "\n";
	if (!summary.invalidLBAs.empty()) {
		out << "invalid_lbas=";
		for (size_t i = 0; i < summary.invalidLBAs.size(); ++i) {
			if (i) out << ",";
			out << summary.invalidLBAs[i];
		}
		out << "\n";
	}
	return out.good();
}

std::vector<std::wstring> CollectPreservationArtifacts(
	const std::wstring& basePath, const DiscInfo& disc) {
	std::vector<std::wstring> result;

	AddExpectedArtifact(result, basePath + L".bin");
	AddExpectedArtifact(result, basePath + L".cue");
	if (disc.includeSubchannel)
		AddExpectedArtifact(result, basePath + L".sub");

	if (disc.pregapMode == PregapMode::Separate) {
		for (const auto& track : disc.tracks) {
			if (disc.selectedSession > 0 &&
				track.session != disc.selectedSession) continue;
			if (track.pregapLBA >= track.startLBA) continue;
			const std::wstring pregapBase = basePath + L"_track" +
				std::to_wstring(track.trackNumber) + L"_pregap";
			AddExpectedArtifact(result, pregapBase + L".bin");
			if (disc.includeSubchannel)
				AddExpectedArtifact(result, pregapBase + L".sub");
		}
	}
	std::sort(result.begin(), result.end());
	return result;
}

bool WritePreservationManifest(const DiscInfo& disc,
	const PreservationManifestContext& context, const std::wstring& filename) {
	struct HashedArtifact {
		std::wstring path;
		FileHashSet hashes;
	};
	std::vector<HashedArtifact> artifacts;
	for (const auto& path : context.artifacts) {
		std::error_code error;
		if (!std::filesystem::is_regular_file(
			std::filesystem::path(path), error) || error) {
			return false;
		}
		HashedArtifact item{ path, {} };
		if (!HashFileForPreservation(path, item.hashes)) return false;
		artifacts.push_back(std::move(item));
	}

	std::ofstream out(std::filesystem::path(filename), std::ios::trunc);
	if (!out) return false;
	out << "{\n"
		<< "  \"format\": \"OptiScan preservation manifest\",\n"
		<< "  \"version\": 1,\n"
		<< "  \"workflow\": \"" << JsonEscape(context.workflow) << "\",\n"
		<< "  \"disc_signature\": \"" << std::hex << std::setw(16)
		<< std::setfill('0') << CalculateDiscSignature(disc) << std::dec << "\",\n"
		<< "  \"accuraterip\": {\"id1\": \"" << std::hex << std::setw(8)
		<< AccurateRip::CalculateDiscID1(disc) << "\", \"id2\": \"" << std::setw(8)
		<< AccurateRip::CalculateDiscID2(disc) << "\", \"cddb\": \"" << std::setw(8)
		<< AccurateRip::CalculateCDDBID(disc) << std::dec << "\"},\n"
		<< "  \"read_settings\": {\"drive_offset_samples\": " << disc.driveOffset
		<< ", \"subchannel\": " << (disc.includeSubchannel ? "true" : "false")
		<< ", \"selected_session\": " << disc.selectedSession
		<< ", \"pregap_mode\": " << static_cast<int>(disc.pregapMode) << "},\n";

	if (context.drive) {
		out << "  \"drive\": {\"vendor\": \"" << JsonEscape(context.drive->vendor)
			<< "\", \"model\": \"" << JsonEscape(context.drive->model)
			<< "\", \"firmware\": \"" << JsonEscape(context.drive->firmware)
			<< "\", \"serial\": \"" << JsonEscape(context.drive->serialNumber) << "\"},\n";
	}
	else {
		out << "  \"drive\": null,\n";
	}

	out << "  \"tracks\": [\n";
	for (size_t i = 0; i < disc.tracks.size(); ++i) {
		const auto& track = disc.tracks[i];
		out << "    {\"number\": " << track.trackNumber
			<< ", \"start_lba\": " << track.startLBA
			<< ", \"end_lba\": " << track.endLBA
			<< ", \"pregap_lba\": " << track.pregapLBA
			<< ", \"session\": " << track.session
			<< ", \"type\": \"" << (track.isAudio ? "audio" :
				(track.mode == 2 ? "mode2" : "mode1")) << "\"}";
		out << (i + 1 == disc.tracks.size() ? "\n" : ",\n");
	}
	out << "  ],\n";

	if (context.dataValidation) {
		const auto& data = *context.dataValidation;
		out << "  \"data_validation\": {\"sectors\": " << data.sectors
			<< ", \"valid\": " << data.validSectors
			<< ", \"invalid_edc\": " << data.invalidEdc
			<< ", \"invalid_ecc\": " << data.invalidEcc << "},\n";
	}
	else out << "  \"data_validation\": null,\n";

	if (context.writeOffset) {
		const auto& offset = *context.writeOffset;
		out << "  \"preservation_write_offset\": {\"detected\": "
			<< (offset.detected ? "true" : "false")
			<< ", \"samples\": " << offset.sampleOffset
			<< ", \"confidence_percent\": " << offset.confidencePercent
			<< ", \"evidence\": " << offset.evidenceCount
			<< ", \"method\": \"" << JsonEscape(offset.method)
			<< "\", \"note\": \"" << JsonEscape(offset.note) << "\"},\n";
	}
	else out << "  \"preservation_write_offset\": null,\n";

	if (!context.verificationStatus.empty()) {
		out << "  \"verification\": {\"status\": \""
			<< JsonEscape(context.verificationStatus)
			<< "\", \"method\": \"" << JsonEscape(context.verificationMethod)
			<< "\", \"note\": \"" << JsonEscape(context.verificationNote)
			<< "\", \"affected_tracks\": [";
		for (size_t i = 0; i < context.verificationAffectedTracks.size(); ++i) {
			if (i) out << ", ";
			out << context.verificationAffectedTracks[i];
		}
		out << "]},\n";
	}
	else out << "  \"verification\": null,\n";

	if (context.recovery) {
		const auto& recovery = *context.recovery;
		out << "  \"recovery\": {\"total\": " << recovery.totalSectors
			<< ", \"resumed\": " << (recovery.resumedFromCheckpoint ? "true" : "false")
			<< ", \"resumed_sectors\": " << recovery.resumedSectors
			<< ", \"recovered\": " << recovery.recovered
			<< ", \"partial\": " << recovery.partial
			<< ", \"unrecovered\": " << recovery.unrecovered
			<< ", \"subchannel_failures\": " << recovery.subchannelFailures
			<< ", \"confidence\": " << std::fixed << std::setprecision(3)
			<< recovery.confidence << "},\n";
	}
	else out << "  \"recovery\": null,\n";

	out << "  \"artifacts\": [\n";
	for (size_t i = 0; i < artifacts.size(); ++i) {
		const auto& item = artifacts[i];
		std::string fileName = WideToUtf8(
			std::filesystem::path(item.path).filename().wstring());
		out << "    {\"file\": \"" << JsonEscape(fileName)
			<< "\", \"bytes\": " << item.hashes.size
			<< ", \"crc32\": \"" << std::hex << std::setw(8)
			<< std::setfill('0') << item.hashes.crc32 << std::dec
			<< "\", \"md5\": \"" << item.hashes.md5
			<< "\", \"sha1\": \"" << item.hashes.sha1
			<< "\", \"sha256\": \"" << item.hashes.sha256 << "\"}";
		out << (i + 1 == artifacts.size() ? "\n" : ",\n");
	}
	out << "  ]\n}\n";
	return out.good();
}
