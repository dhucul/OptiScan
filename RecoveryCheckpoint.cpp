// ============================================================================
// RecoveryCheckpoint.cpp - Durable, resumable recovery-rip state
// ============================================================================
#define NOMINMAX
#include "RecoveryCheckpoint.h"
#include "Constants.h"
#include "Preservation.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>

namespace {

#pragma pack(push, 1)
struct DiskHeader {
	char magic[8];
	uint32_t version;
	uint64_t discSignature;
	uint32_t totalSectors;
	int32_t referenceDriveOffset;
	uint8_t includeSubchannel;
	uint8_t selectedSession;
	uint8_t pregapMode;
	uint8_t reserved;
	char referenceDrive[128];
	uint64_t contentFingerprint;
	uint32_t headerCrc32;
};

struct DiskEntry {
	uint32_t lba;
	int32_t track;
	uint8_t isAudio;
	uint8_t status;
	uint8_t subchannelValid;
	uint8_t reserved;
	int32_t passesUsed;
	int32_t confirmedBytes;
	int32_t maxJitterSamples;
	int32_t c2FlaggedBytes;
	uint32_t payloadCrc32;
	uint32_t entryCrc32;
};
#pragma pack(pop)

constexpr char kMagic[8] = { 'O', 'P', 'T', 'R', 'C', 'V', '3', '\0' };
constexpr uint32_t kVersion = 3;

std::streamoff EntryOffset(size_t index) {
	return static_cast<std::streamoff>(sizeof(DiskHeader)) +
		static_cast<std::streamoff>(index * sizeof(DiskEntry));
}

std::streamoff DataOffset(size_t index) {
	return static_cast<std::streamoff>(index * RAW_SECTOR_SIZE);
}

DiskEntry ToDisk(const RecoveryCheckpointEntry& entry) {
	DiskEntry disk{};
	disk.lba = entry.lba;
	disk.track = entry.track;
	disk.isAudio = entry.isAudio ? 1 : 0;
	disk.status = static_cast<uint8_t>(entry.status);
	disk.subchannelValid = entry.subchannelValid ? 1 : 0;
	disk.passesUsed = entry.passesUsed;
	disk.confirmedBytes = entry.confirmedBytes;
	disk.maxJitterSamples = entry.maxJitterSamples;
	disk.c2FlaggedBytes = entry.c2FlaggedBytes;
	return disk;
}

RecoveryCheckpointEntry FromDisk(const DiskEntry& disk) {
	RecoveryCheckpointEntry entry;
	entry.lba = disk.lba;
	entry.track = disk.track;
	entry.isAudio = disk.isAudio != 0;
	entry.status = disk.status <= static_cast<uint8_t>(CheckpointSectorStatus::Unrecovered)
		? static_cast<CheckpointSectorStatus>(disk.status)
		: CheckpointSectorStatus::Empty;
	entry.subchannelValid = disk.subchannelValid != 0;
	entry.passesUsed = disk.passesUsed;
	entry.confirmedBytes = disk.confirmedBytes;
	entry.maxJitterSamples = disk.maxJitterSamples;
	entry.c2FlaggedBytes = disk.c2FlaggedBytes;
	return entry;
}

uint32_t HeaderCrc(const DiskHeader& header) {
	return PreservationCRC32(reinterpret_cast<const BYTE*>(&header),
		offsetof(DiskHeader, headerCrc32));
}

uint32_t EntryCrc(const DiskEntry& entry) {
	return PreservationCRC32(reinterpret_cast<const BYTE*>(&entry),
		offsetof(DiskEntry, entryCrc32));
}

bool EntryFieldsValid(const DiskEntry& entry) {
	return entry.isAudio <= 1 &&
		entry.subchannelValid <= 1 &&
		entry.status <= static_cast<uint8_t>(CheckpointSectorStatus::Unrecovered) &&
		entry.passesUsed >= 0 &&
		entry.confirmedBytes >= 0 &&
		entry.confirmedBytes <= AUDIO_SECTOR_SIZE &&
		entry.maxJitterSamples >= 0 &&
		entry.c2FlaggedBytes >= 0 &&
		entry.c2FlaggedBytes <= AUDIO_SECTOR_SIZE;
}

} // namespace

RecoveryCheckpoint::~RecoveryCheckpoint() {
	Close();
}

bool RecoveryCheckpoint::Open(const std::wstring& basePath, const DiscInfo& disc,
	DWORD totalSectors, const std::string& currentDrive, int currentDriveOffset,
	uint64_t contentFingerprint) {
	Close();
	m_preservedInvalidFiles = false;
	m_statePath = basePath + L".recovery.state";
	m_partialPath = basePath + L".recovery.partial.bin";

	std::error_code stateError;
	std::error_code partialError;
	const bool stateExists = std::filesystem::exists(
		std::filesystem::path(m_statePath), stateError);
	const bool partialExists = std::filesystem::exists(
		std::filesystem::path(m_partialPath), partialError);
	if (stateError || partialError) return false;

	if (stateExists || partialExists) {
		std::error_code stateTypeError;
		std::error_code partialTypeError;
		if (stateExists && partialExists &&
			std::filesystem::is_regular_file(std::filesystem::path(m_statePath), stateTypeError) &&
			std::filesystem::is_regular_file(std::filesystem::path(m_partialPath), partialTypeError) &&
			!stateTypeError && !partialTypeError &&
			LoadExisting(disc, totalSectors, contentFingerprint)) {
			m_resumed = true;
			m_open = true;
			return true;
		}
		Close();
		m_statePath = basePath + L".recovery.state";
		m_partialPath = basePath + L".recovery.partial.bin";
		if (!PreserveExistingFiles()) return false;
		m_preservedInvalidFiles = true;
	}

	if (!CreateNew(disc, totalSectors, currentDrive, currentDriveOffset,
		contentFingerprint))
		return false;
	m_open = true;
	return true;
}

bool RecoveryCheckpoint::CreateNew(const DiscInfo& disc, DWORD totalSectors,
	const std::string& currentDrive, int currentDriveOffset,
	uint64_t contentFingerprint) {
	m_contentFingerprint = contentFingerprint;
	m_discSignature = CalculateDiscSignature(disc, contentFingerprint);
	m_totalSectors = totalSectors;
	m_referenceDriveOffset = currentDriveOffset;
	m_referenceDrive = currentDrive;
	m_includeSubchannel = disc.includeSubchannel;
	m_selectedSession = disc.selectedSession;
	m_pregapMode = disc.pregapMode;
	m_entries.assign(totalSectors, {});
	m_payloadCrcs.assign(totalSectors, 0);

	m_state.open(std::filesystem::path(m_statePath),
		std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
	m_partial.open(std::filesystem::path(m_partialPath),
		std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
	if (!m_state || !m_partial || !WriteHeader()) {
		Close();
		return false;
	}
	return true;
}

bool RecoveryCheckpoint::LoadExisting(const DiscInfo& disc, DWORD totalSectors,
	uint64_t contentFingerprint) {
	m_state.open(std::filesystem::path(m_statePath),
		std::ios::binary | std::ios::in | std::ios::out);
	m_partial.open(std::filesystem::path(m_partialPath),
		std::ios::binary | std::ios::in | std::ios::out);
	if (!m_state || !m_partial) return false;

	DiskHeader header{};
	m_state.read(reinterpret_cast<char*>(&header), sizeof(header));
	if (!m_state || std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 ||
		header.version != kVersion ||
		header.headerCrc32 != HeaderCrc(header) ||
		header.contentFingerprint != contentFingerprint ||
		header.discSignature != CalculateDiscSignature(disc, contentFingerprint) ||
		header.totalSectors != totalSectors ||
		header.includeSubchannel != (disc.includeSubchannel ? 1 : 0) ||
		header.selectedSession != static_cast<uint8_t>(disc.selectedSession) ||
		header.pregapMode != static_cast<uint8_t>(disc.pregapMode)) {
		return false;
	}

	m_discSignature = header.discSignature;
	m_contentFingerprint = header.contentFingerprint;
	m_totalSectors = header.totalSectors;
	m_referenceDriveOffset = header.referenceDriveOffset;
	m_includeSubchannel = header.includeSubchannel != 0;
	m_selectedSession = header.selectedSession;
	m_pregapMode = static_cast<PregapMode>(header.pregapMode);
	m_referenceDrive.assign(header.referenceDrive,
		strnlen_s(header.referenceDrive, sizeof(header.referenceDrive)));
	m_entries.assign(totalSectors, {});
	m_payloadCrcs.assign(totalSectors, 0);

	m_partial.seekg(0, std::ios::end);
	std::streamoff partialBytes = m_partial.tellg();
	m_partial.clear();

	for (size_t i = 0; i < m_entries.size(); ++i) {
		DiskEntry disk{};
		m_state.seekg(EntryOffset(i));
		m_state.read(reinterpret_cast<char*>(&disk), sizeof(disk));
		if (!m_state) {
			m_state.clear();
			break;
		}
		auto entry = FromDisk(disk);
		if (disk.entryCrc32 != EntryCrc(disk) || !EntryFieldsValid(disk))
			continue;
		if (entry.status != CheckpointSectorStatus::Empty &&
			partialBytes >= DataOffset(i) + RAW_SECTOR_SIZE) {
			std::array<BYTE, RAW_SECTOR_SIZE> storage{};
			m_partial.seekg(DataOffset(i));
			m_partial.read(reinterpret_cast<char*>(storage.data()), storage.size());
			if (!m_partial) {
				m_partial.clear();
				continue;
			}
			if (PreservationCRC32(storage.data(), storage.size()) !=
				disk.payloadCrc32) {
				continue;
			}
			m_entries[i] = entry;
			m_payloadCrcs[i] = disk.payloadCrc32;
		}
	}
	m_state.clear();
	m_partial.clear();
	return true;
}

bool RecoveryCheckpoint::WriteHeader() {
	DiskHeader header{};
	std::memcpy(header.magic, kMagic, sizeof(kMagic));
	header.version = kVersion;
	header.discSignature = m_discSignature;
	header.contentFingerprint = m_contentFingerprint;
	header.totalSectors = m_totalSectors;
	header.referenceDriveOffset = m_referenceDriveOffset;
	header.includeSubchannel = m_includeSubchannel ? 1 : 0;
	header.selectedSession = static_cast<uint8_t>(m_selectedSession);
	header.pregapMode = static_cast<uint8_t>(m_pregapMode);
	strncpy_s(header.referenceDrive, sizeof(header.referenceDrive),
		m_referenceDrive.c_str(), _TRUNCATE);
	header.headerCrc32 = HeaderCrc(header);

	m_state.seekp(0);
	m_state.write(reinterpret_cast<const char*>(&header), sizeof(header));
	return m_state.good();
}

int RecoveryCheckpoint::LoadedSectorCount() const {
	return static_cast<int>(std::count_if(m_entries.begin(), m_entries.end(),
		[](const RecoveryCheckpointEntry& entry) {
			return entry.status != CheckpointSectorStatus::Empty;
		}));
}

bool RecoveryCheckpoint::HasSector(size_t index) const {
	return index < m_entries.size() &&
		m_entries[index].status != CheckpointSectorStatus::Empty;
}

bool RecoveryCheckpoint::ReadSector(size_t index, std::vector<BYTE>& data,
	RecoveryCheckpointEntry& entry) const {
	if (!HasSector(index)) return false;
	auto& partial = const_cast<std::fstream&>(m_partial);
	std::array<BYTE, RAW_SECTOR_SIZE> storage{};
	partial.clear();
	partial.seekg(DataOffset(index));
	partial.read(reinterpret_cast<char*>(storage.data()), storage.size());
	if (!partial) {
		partial.clear();
		return false;
	}
	if (index >= m_payloadCrcs.size() ||
		PreservationCRC32(storage.data(), storage.size()) !=
		m_payloadCrcs[index]) {
		return false;
	}
	entry = m_entries[index];
	const size_t size = m_includeSubchannel ? RAW_SECTOR_SIZE : AUDIO_SECTOR_SIZE;
	data.assign(storage.begin(), storage.begin() + size);
	return true;
}

bool RecoveryCheckpoint::WriteSector(size_t index, const std::vector<BYTE>& data,
	const RecoveryCheckpointEntry& entry) {
	if (!m_open || index >= m_entries.size() || data.size() < AUDIO_SECTOR_SIZE)
		return false;

	std::array<BYTE, RAW_SECTOR_SIZE> storage{};
	std::copy_n(data.begin(), std::min(data.size(), storage.size()), storage.begin());
	m_partial.clear();
	m_partial.seekp(DataOffset(index));
	m_partial.write(reinterpret_cast<const char*>(storage.data()), storage.size());
	if (!m_partial) return false;

	DiskEntry disk = ToDisk(entry);
	disk.payloadCrc32 = PreservationCRC32(storage.data(), storage.size());
	disk.entryCrc32 = EntryCrc(disk);
	m_state.clear();
	m_state.seekp(EntryOffset(index));
	m_state.write(reinterpret_cast<const char*>(&disk), sizeof(disk));
	if (!m_state) return false;
	m_entries[index] = entry;
	m_payloadCrcs[index] = disk.payloadCrc32;
	return true;
}

bool RecoveryCheckpoint::Flush() {
	if (!m_open) return false;
	m_partial.flush();
	m_state.flush();
	return m_partial.good() && m_state.good();
}

void RecoveryCheckpoint::Close() {
	if (m_state.is_open()) m_state.close();
	if (m_partial.is_open()) m_partial.close();
	m_entries.clear();
	m_payloadCrcs.clear();
	m_open = false;
	m_resumed = false;
}

bool RecoveryCheckpoint::PreserveExistingFiles() {
	const std::filesystem::path state(m_statePath);
	const std::filesystem::path partial(m_partialPath);
	std::wstring suffix = L".invalid";
	for (int attempt = 0; attempt < 100; ++attempt) {
		const std::wstring numbered = attempt == 0
			? suffix : suffix + L"." + std::to_wstring(attempt);
		const std::filesystem::path stateBackup(m_statePath + numbered);
		const std::filesystem::path partialBackup(m_partialPath + numbered);
		std::error_code stateCheckError;
		std::error_code partialCheckError;
		const bool stateBackupExists =
			std::filesystem::exists(stateBackup, stateCheckError);
		const bool partialBackupExists =
			std::filesystem::exists(partialBackup, partialCheckError);
		if (stateCheckError || partialCheckError) return false;
		if (stateBackupExists || partialBackupExists) {
			continue;
		}

		std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moved;
		for (const auto& pair : {
			std::pair{ state, stateBackup }, std::pair{ partial, partialBackup } }) {
			std::error_code existsError;
			if (!std::filesystem::exists(pair.first, existsError)) {
				if (existsError) return false;
				continue;
			}
			std::error_code moveError;
			std::filesystem::rename(pair.first, pair.second, moveError);
			if (moveError) {
				for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
					std::error_code rollbackError;
					std::filesystem::rename(it->second, it->first, rollbackError);
				}
				return false;
			}
			moved.push_back(pair);
		}
		return true;
	}
	return false;
}

bool RecoveryCheckpoint::RemoveFiles() {
	Close();
	std::error_code stateError;
	std::error_code partialError;
	const bool stateExists = std::filesystem::exists(std::filesystem::path(m_statePath), stateError);
	const bool partialExists = std::filesystem::exists(std::filesystem::path(m_partialPath), partialError);
	bool stateRemoved = !stateError && (!stateExists ||
		std::filesystem::remove(std::filesystem::path(m_statePath), stateError));
	bool partialRemoved = !partialError && (!partialExists ||
		std::filesystem::remove(std::filesystem::path(m_partialPath), partialError));
	return stateRemoved && partialRemoved && !stateError && !partialError;
}

bool RemoveRecoveryCheckpointFiles(const std::wstring& basePath) {
	std::error_code stateError;
	std::error_code partialError;
	const std::filesystem::path state(basePath + L".recovery.state");
	const std::filesystem::path partial(basePath + L".recovery.partial.bin");
	const bool stateExists = std::filesystem::exists(state, stateError);
	const bool partialExists = std::filesystem::exists(partial, partialError);
	bool stateRemoved = !stateError && (!stateExists || std::filesystem::remove(state, stateError));
	bool partialRemoved = !partialError && (!partialExists || std::filesystem::remove(partial, partialError));
	return stateRemoved && partialRemoved && !stateError && !partialError;
}
