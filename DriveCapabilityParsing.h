// ============================================================================
// DriveCapabilityParsing.h - Pure MMC capability-response parsing helpers
// ============================================================================
#pragma once

#include "DriveTypes.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace DriveCapabilityParsing {

constexpr size_t GET_PERFORMANCE_HEADER_SIZE = 8;
constexpr size_t WRITE_SPEED_DESCRIPTOR_SIZE = 16;

inline uint16_t ReadBE16(const BYTE* p) {
	return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t ReadBE32(const BYTE* p) {
	return (static_cast<uint32_t>(p[0]) << 24)
		| (static_cast<uint32_t>(p[1]) << 16)
		| (static_cast<uint32_t>(p[2]) << 8)
		| static_cast<uint32_t>(p[3]);
}

inline bool IsCDProfile(WORD profile) {
	return profile == 0x0008 || profile == 0x0009 || profile == 0x000A
		|| profile == 0x0020 || profile == 0x0021 || profile == 0x0022;
}

inline bool IsWritableDVDProfile(WORD profile) {
	switch (profile) {
	case 0x0011: case 0x0012: case 0x0013: case 0x0014:
	case 0x0015: case 0x0016: case 0x001A: case 0x001B:
	case 0x002A: case 0x002B:
		return true;
	default:
		return false;
	}
}

inline bool IsDVDProfile(WORD profile) {
	return profile == 0x0010 || IsWritableDVDProfile(profile);
}

inline bool IsWritableBDProfile(WORD profile) {
	return profile == 0x0041 || profile == 0x0042 || profile == 0x0043;
}

inline bool IsBDProfile(WORD profile) {
	return profile == 0x0040 || IsWritableBDProfile(profile);
}

// Convert write-performance values to an x-rate only when the mounted
// profile identifies the applicable transfer-rate family. A read-only CD-ROM
// still establishes the CD base for write speeds reported by the recorder.
inline int WriteSpeedBaseKB(WORD profile) {
	if (IsWritableBDProfile(profile)) return 4495;
	if (IsWritableDVDProfile(profile)) return 1385;
	if (profile == 0x0008 || profile == 0x0009 || profile == 0x000A)
		return 176;
	return 0;
}

inline const char* WriteSpeedFamilyName(WORD profile) {
	if (IsWritableBDProfile(profile)) return "BD";
	if (IsWritableDVDProfile(profile)) return "DVD";
	if (profile == 0x0008 || profile == 0x0009 || profile == 0x000A)
		return "CD";
	return nullptr;
}

inline std::string MediaProfileName(WORD profile) {
	switch (profile) {
	case 0x0000: return "None";
	case 0x0008: return "CD-ROM";
	case 0x0009: return "CD-R";
	case 0x000A: return "CD-RW";
	case 0x0010: return "DVD-ROM";
	case 0x0011: return "DVD-R Sequential";
	case 0x0012: return "DVD-RAM";
	case 0x0013: return "DVD-RW Restricted Overwrite";
	case 0x0014: return "DVD-RW Sequential";
	case 0x0015: return "DVD-R DL Sequential";
	case 0x0016: return "DVD-R DL Layer Jump";
	case 0x001A: return "DVD+RW";
	case 0x001B: return "DVD+R";
	case 0x0020: return "DDCD-ROM";
	case 0x0021: return "DDCD-R";
	case 0x0022: return "DDCD-RW";
	case 0x002A: return "DVD+RW DL";
	case 0x002B: return "DVD+R DL";
	case 0x0040: return "BD-ROM";
	case 0x0041: return "BD-R Sequential";
	case 0x0042: return "BD-R Random Recording";
	case 0x0043: return "BD-RE";
	case 0x0050: return "HD DVD-ROM";
	case 0x0051: return "HD DVD-R";
	case 0x0052: return "HD DVD-RAM";
	default: {
		char text[32];
		snprintf(text, sizeof(text), "Unknown profile 0x%04X", profile);
		return text;
	}
	}
}

inline void ApplyProfile(WORD profile, DriveCapabilities& caps) {
	switch (profile) {
	case 0x0009:
		caps.readsCDR = true;
		caps.writesCDR = true;
		break;
	case 0x000A:
		caps.readsCDR = true;
		caps.readsCDRW = true;
		caps.writesCDRW = true;
		break;
	case 0x0010:
		caps.readsDVD = true;
		break;
	case 0x0011: case 0x0013: case 0x0014: case 0x0015: case 0x0016:
	case 0x001A: case 0x001B: case 0x002A: case 0x002B:
		caps.readsDVD = true;
		caps.writesDVD = true;
		break;
	case 0x0012:
		caps.readsDVD = true;
		caps.writesDVD = true;
		caps.writesDVDRAM = true;
		break;
	case 0x0040:
		caps.readsBD = true;
		break;
	case 0x0041: case 0x0042: case 0x0043:
		caps.readsBD = true;
		caps.writesBD = true;
		break;
	default:
		break;
	}
}

// Parse a GET CONFIGURATION response requested from Feature 0000h (Profile
// List). The response header identifies the loaded medium; the list identifies
// every profile implemented by the drive, independent of the inserted disc.
inline bool ParseProfileListResponse(const BYTE* data, size_t size,
	DriveCapabilities& caps, std::vector<WORD>* profiles = nullptr) {
	if (!data || size < 12)
		return false;

	const uint32_t dataLength = ReadBE32(data);
	if (dataLength < 8)
		return false;
	const uint64_t declaredTotal64 = static_cast<uint64_t>(dataLength) + 4;
	const size_t declaredTotal = static_cast<size_t>((std::min<uint64_t>)(
		declaredTotal64, (std::numeric_limits<size_t>::max)()));
	const size_t available = (std::min)(size, declaredTotal);

	if (ReadBE16(data + 8) != 0x0000)
		return false;
	const size_t additionalLength = data[11];
	const size_t descriptorEnd = 12 + additionalLength;
	if (descriptorEnd > available || (additionalLength % 4) != 0)
		return false;

	// Commit only after the complete response has been validated. Callers can
	// safely retain an earlier capability snapshot when a bridge returns a
	// truncated or wrong-feature response.
	const WORD currentProfile = ReadBE16(data + 6);
	caps.currentMediaProfile = currentProfile;
	caps.currentMediaType = MediaProfileName(currentProfile);

	if (profiles)
		profiles->clear();
	for (size_t offset = 12; offset + 4 <= descriptorEnd; offset += 4) {
		const WORD profile = ReadBE16(data + offset);
		ApplyProfile(profile, caps);
		if (profiles)
			profiles->push_back(profile);
	}
	return true;
}

// Parse a normalized Mode Page 2Ah (page byte 0 at data[0]).
inline bool ParseModePage2A(const BYTE* data, size_t size, DriveCapabilities& caps) {
	if (!data || size < 2 || (data[0] & 0x3F) != 0x2A)
		return false;
	const size_t pageBytes = (std::min)(size, static_cast<size_t>(data[1]) + 2);
	if (pageBytes < 4)
		return false;

	// Byte 2: DVD-RAM, DVD-R, and DVD-ROM are distinct advertised bits.
	caps.readsCDR |= (data[2] & 0x01) != 0;
	caps.readsCDRW |= (data[2] & 0x02) != 0;
	caps.readsDVD |= (data[2] & 0x38) != 0;

	caps.writesCDR |= (data[3] & 0x01) != 0;
	caps.writesCDRW |= (data[3] & 0x02) != 0;
	caps.supportsTestWrite |= (data[3] & 0x04) != 0;
	caps.writesDVD |= (data[3] & 0x10) != 0;
	caps.writesDVDRAM |= (data[3] & 0x20) != 0;

	if (pageBytes > 4) {
		caps.supportsAudioPlay |= (data[4] & 0x01) != 0;
		caps.supportsCompositeOutput |= (data[4] & 0x02) != 0;
		caps.supportsMultiSession |= (data[4] & 0x40) != 0;
		caps.supportsBufferUnderrunProtection |= (data[4] & 0x80) != 0;
	}
	if (pageBytes > 5) {
		caps.supportsCDDA |= (data[5] & 0x01) != 0;
		caps.supportsAccurateStream |= (data[5] & 0x02) != 0;
		caps.supportsSubchannelRaw |= (data[5] & 0x04) != 0;
		caps.supportsSubchannelDeinterleaved |= (data[5] & 0x08) != 0;
		caps.supportsC2ErrorReporting |= (data[5] & 0x10) != 0;
	}
	if (pageBytes > 6) {
		caps.supportsLockMedia |= (data[6] & 0x01) != 0;
		caps.supportsEject |= (data[6] & 0x08) != 0;
		caps.loadingMechanism = (data[6] >> 5) & 0x07;
		caps.isChanger = caps.loadingMechanism == 4 || caps.loadingMechanism == 5;
	}
	if (pageBytes > 7) {
		caps.supportsSeparateVolume |= (data[7] & 0x01) != 0;
		caps.supportsSeparateMute |= (data[7] & 0x02) != 0;
	}

	auto legacySpeed = [](const BYTE* p) -> int {
		const WORD value = ReadBE16(p);
		return value == 0 || value == 0xFFFF ? 0 : static_cast<int>(value);
	};
	if (pageBytes >= 16) {
		caps.maxReadSpeedKB = (std::max)(caps.maxReadSpeedKB, legacySpeed(data + 8));
		caps.bufferSizeKB = static_cast<int>(ReadBE16(data + 12));
		caps.currentReadSpeedKB = legacySpeed(data + 14);
	}

	if (pageBytes >= 22) {
		caps.maxWriteSpeedKB = (std::max)(caps.maxWriteSpeedKB, legacySpeed(data + 18));
		caps.currentWriteSpeedKB = legacySpeed(data + 20);
	}
	if (pageBytes >= 30) {
		// MMC-3+ adds a newer selected-write-speed field but retains the legacy
		// maximum at bytes 18-19.
		const int selected = legacySpeed(data + 28);
		if (selected > 0) caps.currentWriteSpeedKB = selected;
	}

	if (pageBytes >= 32) {
		const size_t reportedCount = ReadBE16(data + 30);
		const size_t availableCount = (pageBytes - 32) / 4;
		const size_t count = (std::min)(reportedCount, availableCount);
		for (size_t i = 0; i < count; ++i) {
			const int speed = static_cast<int>(ReadBE16(data + 32 + i * 4 + 2));
			if (speed > 0 && speed != 0xFFFF)
				caps.supportedWriteSpeeds.push_back(speed);
		}
	}
	return true;
}

// GET PERFORMANCE Type 03h uses 16-byte Write Speed Descriptors. The write
// speed is a 32-bit kB/s value at descriptor bytes 12..15. Treating these as
// 4-byte descriptors mistakes End LBA and Read Speed fields for extra speeds.
inline bool ParseWriteSpeedDescriptors(const BYTE* data, size_t size,
	std::vector<int>& speeds) {
	if (!data || size < GET_PERFORMANCE_HEADER_SIZE)
		return false;
	const uint32_t dataLength = ReadBE32(data);
	if (dataLength < 4)
		return false;
	const uint64_t declaredTotal64 = static_cast<uint64_t>(dataLength) + 4;
	const size_t declaredTotal = static_cast<size_t>((std::min<uint64_t>)(
		declaredTotal64, (std::numeric_limits<size_t>::max)()));
	const size_t available = (std::min)(size, declaredTotal);
	if (available < GET_PERFORMANCE_HEADER_SIZE)
		return false;

	const size_t descriptorBytes = available - GET_PERFORMANCE_HEADER_SIZE;
	const size_t count = descriptorBytes / WRITE_SPEED_DESCRIPTOR_SIZE;
	for (size_t i = 0; i < count; ++i) {
		const size_t offset = GET_PERFORMANCE_HEADER_SIZE
			+ i * WRITE_SPEED_DESCRIPTOR_SIZE;
		const uint32_t speed = ReadBE32(data + offset + 12);
		if (speed > 0 && speed <= static_cast<uint32_t>((std::numeric_limits<int>::max)()))
			speeds.push_back(static_cast<int>(speed));
	}
	return true;
}

// Apply the feature-specific bits used by the capability report. `descriptor`
// starts at the two-byte feature code, not at the GET CONFIGURATION header.
inline bool ApplyFeatureDescriptor(const BYTE* descriptor, size_t size,
	WORD expectedFeature, DriveCapabilities& caps) {
	if (!descriptor || size < 4 || ReadBE16(descriptor) != expectedFeature)
		return false;
	const size_t total = 4 + descriptor[3];
	if (total > size)
		return false;

	switch (expectedFeature) {
	case 0x001E: // CD Read
		if (total < 8) return false;
		caps.supportsRawRead = true; // Feature 001Eh requires READ CD (BEh).
		caps.supportsC2ErrorReporting |= (descriptor[4] & 0x02) != 0;
		caps.supportsCDText |= (descriptor[4] & 0x01) != 0;
		break;
	case 0x001F: // DVD Read
		caps.readsDVD = true;
		break;
	case 0x002D: // CD Track at Once
		caps.supportsWriteTAO = true;
		if (total >= 8) {
			caps.supportsBufferUnderrunProtection |= (descriptor[4] & 0x40) != 0;
			caps.supportsTestWrite |= (descriptor[4] & 0x04) != 0;
		}
		break;
	case 0x002E: // CD Mastering
		if (total < 8) return false;
		caps.supportsBufferUnderrunProtection |= (descriptor[4] & 0x40) != 0;
		caps.supportsWriteSAO |= (descriptor[4] & 0x20) != 0;
		caps.supportsWriteRAW |= (descriptor[4] & 0x18) != 0;
		caps.supportsTestWrite |= (descriptor[4] & 0x04) != 0;
		caps.supportsWriteCDText |= (descriptor[4] & 0x01) != 0;
		break;
	case 0x0040: // BD Read
		caps.readsBD = true;
		break;
	case 0x0041: // BD Write
		caps.readsBD = true;
		caps.writesBD = true;
		break;
	default:
		break;
	}
	return true;
}

} // namespace DriveCapabilityParsing
