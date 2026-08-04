// ============================================================================
// DriveCharacterization.cpp - Active drive probes and persistent profiles
// ============================================================================
#define NOMINMAX
#include "ScsiDrive.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {

std::string ProfileKey(const std::string& vendor, const std::string& model,
	const std::string& firmware) {
	return vendor + "\x1f" + model + "\x1f" + firmware;
}

bool RawQValid(const BYTE* raw) {
	BYTE q[12] = {};
	for (int i = 0; i < 96; ++i) {
		int byteIndex = i / 8;
		int bitIndex = 7 - (i % 8);
		if (raw[i] & 0x40) q[byteIndex] |= static_cast<BYTE>(1 << bitIndex);
	}
	if ((q[0] & 0x0F) == 0) return false;
	uint16_t calculated = SubchannelCRC16(q, 10);
	uint16_t stored = (static_cast<uint16_t>(q[10]) << 8) | q[11];
	return calculated == stored || static_cast<uint16_t>(calculated ^ 0xFFFF) == stored;
}

const char* ReadMethodName(DriveReadMethod method) {
	switch (method) {
	case DriveReadMethod::ReadCD: return "READ CD (BE)";
	case DriveReadMethod::ReadCDDA: return "READ CD-DA (D8)";
	default: return "Unknown";
	}
}

const char* LayoutName(RawSectorLayout layout) {
	switch (layout) {
	case RawSectorLayout::DataC2Sub: return "data + C2 + subchannel";
	case RawSectorLayout::DataSubC2: return "data + subchannel + C2";
	default: return "unknown";
	}
}

class ProfileWriteLock {
public:
	ProfileWriteLock() {
		m_handle = CreateMutexW(nullptr, FALSE, L"Local\\OptiScanDriveProfileWrite");
		if (!m_handle) return;
		const DWORD waitResult = WaitForSingleObject(m_handle, 15000);
		m_acquired = waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED;
	}

	~ProfileWriteLock() {
		if (m_acquired) ReleaseMutex(m_handle);
		if (m_handle) CloseHandle(m_handle);
	}

	bool Acquired() const { return m_acquired; }

private:
	HANDLE m_handle = nullptr;
	bool m_acquired = false;
};

} // namespace

std::wstring GetDriveCharacterizationProfilePath() {
	wchar_t localAppData[MAX_PATH] = {};
	DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
	std::filesystem::path folder;
	if (length > 0 && length < MAX_PATH)
		folder = std::filesystem::path(localAppData) / L"OptiScan";
	else
		folder = std::filesystem::temp_directory_path() / L"OptiScan";
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	return (folder / L"drive_profiles.tsv").wstring();
}

bool LoadDriveCharacterizationProfile(const std::string& vendor,
	const std::string& model, const std::string& firmware,
	DriveCharacterization& profile) {
	std::ifstream input{ std::filesystem::path(GetDriveCharacterizationProfilePath()) };
	if (!input) return false;
	const std::string wanted = ProfileKey(vendor, model, firmware);
	std::string line;
	while (std::getline(input, line)) {
		if (line.empty() || line[0] == '#') continue;
		std::istringstream row(line);
		DriveCharacterization item;
		int method = 0, layout = 0;
		if (!(row >> std::quoted(item.vendor) >> std::quoted(item.model)
			>> std::quoted(item.firmware) >> std::quoted(item.serialNumber)
			>> method >> layout
			>> item.readCdAudio >> item.readCdda
			>> item.rawSubchannelFunctional >> item.c2Functional
			>> item.stableRepeatedReads >> item.cacheDefeatVerified
			>> item.leadInReadableSectors >> item.leadOutReadableSectors
			>> std::quoted(item.notes))) continue;
		if (ProfileKey(item.vendor, item.model, item.firmware) != wanted) continue;
		item.preferredReadMethod = static_cast<DriveReadMethod>(method);
		item.rawSectorLayout = static_cast<RawSectorLayout>(layout);
		item.performed = true;
		item.loadedFromCache = true;
		item.profileSaved = true;
		profile = std::move(item);
		return true;
	}
	return false;
}

bool SaveDriveCharacterizationProfile(const DriveCharacterization& profile) {
	ProfileWriteLock writeLock;
	if (!writeLock.Acquired()) return false;

	const std::filesystem::path target(GetDriveCharacterizationProfilePath());
	std::vector<DriveCharacterization> profiles;
	DriveCharacterization merged = profile;
	std::ifstream input{ target };
	std::string line;
	while (std::getline(input, line)) {
		if (line.empty() || line[0] == '#') continue;
		std::istringstream row(line);
		DriveCharacterization item;
		int method = 0, layout = 0;
		if (!(row >> std::quoted(item.vendor) >> std::quoted(item.model)
			>> std::quoted(item.firmware) >> std::quoted(item.serialNumber)
			>> method >> layout
			>> item.readCdAudio >> item.readCdda
			>> item.rawSubchannelFunctional >> item.c2Functional
			>> item.stableRepeatedReads >> item.cacheDefeatVerified
			>> item.leadInReadableSectors >> item.leadOutReadableSectors
			>> std::quoted(item.notes))) continue;
		item.preferredReadMethod = static_cast<DriveReadMethod>(method);
		item.rawSectorLayout = static_cast<RawSectorLayout>(layout);
		if (ProfileKey(item.vendor, item.model, item.firmware) !=
			ProfileKey(profile.vendor, profile.model, profile.firmware)) {
			profiles.push_back(std::move(item));
		}
		else if (merged.rawSectorLayout == RawSectorLayout::Unknown) {
			// A transient bad Q frame must not erase a layout that a prior
			// characterization validated for this exact firmware.
			merged.rawSectorLayout = item.rawSectorLayout;
		}
	}
	profiles.push_back(std::move(merged));
	input.close();

	std::filesystem::path temporary = target;
	temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
		std::to_wstring(GetTickCount64()) + L".tmp";
	std::ofstream output(temporary, std::ios::trunc);
	if (!output) return false;
	output << "# OptiScan drive characterization profiles v1\n";
	for (const auto& item : profiles) {
		output << std::quoted(item.vendor) << '\t' << std::quoted(item.model) << '\t'
			<< std::quoted(item.firmware) << '\t' << std::quoted(item.serialNumber) << '\t'
			<< static_cast<int>(item.preferredReadMethod) << '\t'
			<< static_cast<int>(item.rawSectorLayout) << '\t'
			<< item.readCdAudio << '\t' << item.readCdda << '\t'
			<< item.rawSubchannelFunctional << '\t' << item.c2Functional << '\t'
			<< item.stableRepeatedReads << '\t' << item.cacheDefeatVerified << '\t'
			<< item.leadInReadableSectors << '\t' << item.leadOutReadableSectors << '\t'
			<< std::quoted(item.notes) << '\n';
	}
	output.close();
	if (!output.good()) {
		std::error_code error;
		std::filesystem::remove(temporary, error);
		return false;
	}
	if (MoveFileExW(temporary.c_str(), target.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
		return true;
	}
	std::error_code error;
	std::filesystem::remove(temporary, error);
	return false;
}

void ScsiDrive::ApplyCharacterization(const DriveCharacterization& profile) {
	if (profile.rawSectorLayout != RawSectorLayout::Unknown)
		m_rawSectorLayout = profile.rawSectorLayout;
}

void ScsiDrive::LoadCachedCharacterization() {
	BYTE cdb[6] = { 0x12, 0, 0, 0, 96, 0 };
	std::array<BYTE, 96> inquiry{};
	if (!SendSCSI(cdb, 6, inquiry.data(), static_cast<DWORD>(inquiry.size())))
		return;

	std::string vendor(reinterpret_cast<char*>(inquiry.data() + 8), 8);
	std::string model(reinterpret_cast<char*>(inquiry.data() + 16), 16);
	std::string firmware(reinterpret_cast<char*>(inquiry.data() + 32), 4);
	auto trimBack = [](std::string& value) {
		while (!value.empty() &&
			(value.back() == ' ' || value.back() == '\0'))
			value.pop_back();
	};
	trimBack(vendor);
	trimBack(model);
	trimBack(firmware);

	DriveCharacterization profile;
	if (LoadDriveCharacterizationProfile(vendor, model, firmware, profile))
		ApplyCharacterization(profile);
}

bool ScsiDrive::Characterize(DWORD audioLBA, DWORD leadOutLBA,
	const DriveCapabilities& caps, DriveCharacterization& result) {
	result = {};
	result.vendor = caps.vendor;
	result.model = caps.model;
	result.firmware = caps.firmware;
	result.serialNumber = caps.serialNumber;

	std::vector<BYTE> first(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> second(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> afterSeek(AUDIO_SECTOR_SIZE);
	result.readCdAudio = ReadSectorAudioOnly(audioLBA, first.data());
	if (result.readCdAudio) {
		result.preferredReadMethod = DriveReadMethod::ReadCD;
		result.stableRepeatedReads =
			ReadSectorAudioOnly(audioLBA, second.data()) && first == second;
		DWORD farLBA = audioLBA;
		if (leadOutLBA > 0) {
			if (static_cast<uint64_t>(audioLBA) + 1 < leadOutLBA) {
				const uint64_t candidate = static_cast<uint64_t>(audioLBA) + 10000;
				farLBA = static_cast<DWORD>(
					std::min<uint64_t>(candidate, static_cast<uint64_t>(leadOutLBA) - 1));
			}
			else if (audioLBA > 0) {
				farLBA = audioLBA - 1;
			}
		}
		else if (audioLBA <= MAXDWORD - 10000) {
			farLBA = audioLBA + 10000;
		}
		else if (audioLBA > 10000) {
			farLBA = audioLBA - 10000;
		}
		const bool soughtAway = farLBA != audioLBA && SeekToLBA(farLBA);
		result.cacheDefeatVerified = soughtAway &&
			ReadSectorAudioOnly(audioLBA, afterSeek.data()) && first == afterSeek;
	}

	// Probe the vendor READ CD-DA command with DATA+SUB. This is read-only and
	// is especially useful on classic Plextor-family drives.
	std::vector<BYTE> d8(RAW_SECTOR_SIZE);
	BYTE d8Cdb[12] = {};
	d8Cdb[0] = 0xD8;
	d8Cdb[2] = (audioLBA >> 24) & 0xFF;
	d8Cdb[3] = (audioLBA >> 16) & 0xFF;
	d8Cdb[4] = (audioLBA >> 8) & 0xFF;
	d8Cdb[5] = audioLBA & 0xFF;
	d8Cdb[9] = 1;    // four-byte transfer count, big-endian
	d8Cdb[10] = 2;   // DATA + raw subchannel
	result.readCdda = SendSCSI(d8Cdb, 12, d8.data(),
		static_cast<DWORD>(d8.size()), true, 5) &&
		RawQValid(d8.data() + AUDIO_SECTOR_SIZE);
	if (!result.readCdAudio && result.readCdda)
		result.preferredReadMethod = DriveReadMethod::ReadCDDA;

	std::vector<BYTE> subAudio(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> rawSub(SUBCHANNEL_SIZE);
	result.rawSubchannelFunctional =
		ReadSector(audioLBA, subAudio.data(), rawSub.data()) &&
		RawQValid(rawSub.data());

	if (caps.supportsC2ErrorReporting) {
		for (DWORD attempt = 0; attempt < 3; ++attempt) {
			if (audioLBA > MAXDWORD - attempt) break;
			DWORD probeLBA = audioLBA + attempt;
			if (leadOutLBA > 0 && probeLBA >= leadOutLBA) break;
			std::vector<BYTE> full(FULL_SECTOR_WITH_C2);
			BYTE cdb[12] = {};
			cdb[0] = SCSI_READ_CD;
			cdb[1] = m_cddaSectorType;
			cdb[2] = (probeLBA >> 24) & 0xFF;
			cdb[3] = (probeLBA >> 16) & 0xFF;
			cdb[4] = (probeLBA >> 8) & 0xFF;
			cdb[5] = probeLBA & 0xFF;
			cdb[8] = 1;
			cdb[9] = static_cast<BYTE>(m_cddaMainChannelFlags | 0x04);
			cdb[10] = 0x01;
			if (!SendSCSI(cdb, 12, full.data(),
				static_cast<DWORD>(full.size()), true, 5))
				continue;
			const bool c2ThenSub = RawQValid(
				full.data() + AUDIO_SECTOR_SIZE + C2_ERROR_SIZE);
			const bool subThenC2 = RawQValid(full.data() + AUDIO_SECTOR_SIZE);
			if (subThenC2 && !c2ThenSub)
				result.rawSectorLayout = RawSectorLayout::DataSubC2;
			else if (c2ThenSub)
				result.rawSectorLayout = RawSectorLayout::DataC2Sub;
			if (result.rawSectorLayout != RawSectorLayout::Unknown) {
				result.c2Functional = true;
				break;
			}
		}
	}

	for (int sector = 1; sector <= 150; ++sector) {
		std::array<BYTE, AUDIO_SECTOR_SIZE> probe{};
		DWORD negativeLba = static_cast<DWORD>(-sector);
		if (!ReadSectorAudioOnly(negativeLba, probe.data())) break;
		result.leadInReadableSectors = sector;
	}
	for (int sector = 0; sector < 75; ++sector) {
		std::array<BYTE, AUDIO_SECTOR_SIZE> probe{};
		if (leadOutLBA > MAXDWORD - static_cast<DWORD>(sector)) break;
		if (!ReadSectorAudioOnly(leadOutLBA + sector, probe.data())) break;
		result.leadOutReadableSectors = sector + 1;
	}

	result.performed = result.readCdAudio || result.readCdda;
	result.notes = std::string("Preferred ") + ReadMethodName(result.preferredReadMethod) +
		"; layout " + LayoutName(result.rawSectorLayout);
	ApplyCharacterization(result);
	result.profileSaved = result.performed && SaveDriveCharacterizationProfile(result);
	return result.performed;
}
