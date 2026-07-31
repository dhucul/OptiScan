// ============================================================================
// RecoveryCheckpoint.h - Durable, resumable recovery-rip state
// ============================================================================
#pragma once

#include "CDStructures.h"
#include "RecoveryTypes.h"
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

enum class CheckpointSectorStatus : uint8_t {
	Empty = 0,
	Baseline = 1,
	Clean = 2,
	Recovered = 3,
	Partial = 4,
	Unrecovered = 5
};

struct RecoveryCheckpointEntry {
	DWORD lba = 0;
	int track = 0;
	bool isAudio = true;
	CheckpointSectorStatus status = CheckpointSectorStatus::Empty;
	int passesUsed = 0;
	int confirmedBytes = 0;
	int maxJitterSamples = 0;
	int c2FlaggedBytes = 0;
	bool subchannelValid = false;
};

class RecoveryCheckpoint {
public:
	RecoveryCheckpoint() = default;
	~RecoveryCheckpoint();

	bool Open(const std::wstring& basePath, const DiscInfo& disc, DWORD totalSectors,
		const std::string& currentDrive, int currentDriveOffset,
		uint64_t contentFingerprint);
	bool IsOpen() const { return m_open; }
	bool WasResumed() const { return m_resumed; }
	bool PreservedInvalidFiles() const { return m_preservedInvalidFiles; }
	int ReferenceDriveOffset() const { return m_referenceDriveOffset; }
	const std::string& ReferenceDrive() const { return m_referenceDrive; }
	int LoadedSectorCount() const;

	bool HasSector(size_t index) const;
	bool ReadSector(size_t index, std::vector<BYTE>& data,
		RecoveryCheckpointEntry& entry) const;
	bool WriteSector(size_t index, const std::vector<BYTE>& data,
		const RecoveryCheckpointEntry& entry);
	bool Flush();
	void Close();
	bool RemoveFiles();

	const std::wstring& StatePath() const { return m_statePath; }
	const std::wstring& PartialImagePath() const { return m_partialPath; }

private:
	bool CreateNew(const DiscInfo& disc, DWORD totalSectors,
		const std::string& currentDrive, int currentDriveOffset,
		uint64_t contentFingerprint);
	bool LoadExisting(const DiscInfo& disc, DWORD totalSectors,
		uint64_t contentFingerprint);
	bool WriteHeader();
	bool PreserveExistingFiles();

	std::wstring m_statePath;
	std::wstring m_partialPath;
	std::fstream m_state;
	std::fstream m_partial;
	std::vector<RecoveryCheckpointEntry> m_entries;
	std::vector<uint32_t> m_payloadCrcs;
	uint64_t m_discSignature = 0;
	uint64_t m_contentFingerprint = 0;
	DWORD m_totalSectors = 0;
	int m_referenceDriveOffset = 0;
	std::string m_referenceDrive;
	bool m_includeSubchannel = false;
	int m_selectedSession = 0;
	PregapMode m_pregapMode = PregapMode::Include;
	bool m_open = false;
	bool m_resumed = false;
	bool m_preservedInvalidFiles = false;
};

bool RemoveRecoveryCheckpointFiles(const std::wstring& basePath);
