// ============================================================================
// Preservation.h - Archival verification and reporting helpers
// ============================================================================
#pragma once

#include "CDStructures.h"
#include "DriveTypes.h"
#include "RecoveryTypes.h"
#include <cstdint>
#include <string>
#include <vector>

enum class RawDataMode {
	Unknown = 0,
	Mode1,
	Mode2Formless,
	Mode2Form1,
	Mode2Form2
};

struct RawSectorValidation {
	RawDataMode mode = RawDataMode::Unknown;
	bool syncValid = false;
	bool addressValid = false;
	bool subheaderValid = true;
	bool edcPresent = false;
	bool edcValid = false;
	bool eccPresent = false;
	bool eccValid = false;
	bool valid = false;
	int addressDeltaSectors = 0;
};

struct DataValidationSummary {
	int tracks = 0;
	int sectors = 0;
	int validSectors = 0;
	int invalidSync = 0;
	int invalidAddress = 0;
	int invalidSubheader = 0;
	int invalidEdc = 0;
	int invalidEcc = 0;
	int mode1Sectors = 0;
	int mode2FormlessSectors = 0;
	int mode2Form1Sectors = 0;
	int mode2Form2Sectors = 0;
	std::vector<DWORD> invalidLBAs;

	bool AllValid() const { return sectors > 0 && validSectors == sectors; }
};

struct PreservationOffsetResult {
	bool detected = false;
	int sampleOffset = 0;
	int confidencePercent = 0;
	int evidenceCount = 0;
	std::string method;
	std::string note;
};

struct FileHashSet {
	uint64_t size = 0;
	uint32_t crc32 = 0;
	std::string md5;
	std::string sha1;
	std::string sha256;
};

struct PreservationManifestContext {
	std::string workflow;
	std::vector<std::wstring> artifacts;
	std::string verificationStatus;
	std::string verificationMethod;
	std::string verificationNote;
	std::vector<int> verificationAffectedTracks;
	const DriveCapabilities* drive = nullptr;
	const RecoveryRipResult* recovery = nullptr;
	const DataValidationSummary* dataValidation = nullptr;
	const PreservationOffsetResult* writeOffset = nullptr;
};

uint32_t PreservationCRC32(const BYTE* data, size_t size, uint32_t seed = 0);
bool HashFileForPreservation(const std::wstring& path, FileHashSet& hashes);
RawSectorValidation ValidateRawDataSector(const BYTE* sector, DWORD expectedLBA);
bool ValidateDataTracks(DiscInfo& disc, DataValidationSummary& summary);
PreservationOffsetResult AnalyzePreservationWriteOffset(const DiscInfo& disc);
uint64_t CalculateDiscSignature(const DiscInfo& disc,
	uint64_t contentFingerprint = 0);
bool WriteDataValidationReport(const DataValidationSummary& summary,
	const std::wstring& filename);
std::vector<std::wstring> CollectPreservationArtifacts(
	const std::wstring& basePath, const DiscInfo& disc);
bool WritePreservationManifest(const DiscInfo& disc,
	const PreservationManifestContext& context, const std::wstring& filename);
