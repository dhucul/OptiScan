// ============================================================================
// ScsiDrive.Capabilities.cpp - Capability and feature detection
// ============================================================================
#include "ScsiDrive.h"
#include "DriveCapabilityParsing.h"
#include <vector>
#include <algorithm>
#include <cctype>

namespace {
std::string ToUpperAscii(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	return value;
}
}

bool ScsiDrive::CheckC2Support() {
	std::string vendor, model;
	GetDriveInfo(vendor, model);

	char dbg[256];
	snprintf(dbg, sizeof(dbg), "CheckC2Support: vendor='%s' model='%s'\n", vendor.c_str(), model.c_str());
	OutputDebugStringA(dbg);

	const std::string vendorUpper = ToUpperAscii(vendor);
	const std::string modelUpper = ToUpperAscii(model);

	// HL-DT-ST/LG slim USB GP-series drives accept MMC READ CD with C2
	// requested, but some firmware returns a non-audio status/padding pattern
	// in the 296-byte C2 area for every clean sector.  That makes OptiScan
	// report a whole disc as C2-damaged while audio-only rippers like
	// cdparanoia complete cleanly.  Treat MMC C2 as unavailable on this family
	// and let secure/paranoid reads rely on reread consensus instead.
	if (vendorUpper.find("HL-DT-ST") != std::string::npos &&
		modelUpper.find("DVDRAM GP") != std::string::npos) {
		m_c2Mode = C2Mode::NotSupported;
		m_c1BlockErrorsAvailable = false;
		m_c2Functional = false;
		OutputDebugStringA("CheckC2Support: HL-DT-ST DVDRAM GP-series reports unreliable MMC C2; disabling C2.\n");
		return false;
	}

	// Try Plextor D8 vendor command — works on genuine Plextor drives and
	// many Lite-On drives that share the same chipset/firmware (rebrands).
	// QPXTools uses the same approach: probe D8 on both vendors.
	if (vendorUpper.find("PLEXTOR") != std::string::npos ||
		vendorUpper.find("LITE-ON") != std::string::npos ||
		vendorUpper.find("LITEON") != std::string::npos) {
		BYTE cdb[12] = { 0xD8, 0, 0, 0, 0, 0, 0, 0, 1, 0x02, 0, 0 };
		std::vector<BYTE> buffer(SECTOR_WITH_C2_SIZE);
		BYTE sk = 0, a = 0, aq = 0;
		bool ok = SendSCSIWithSense(cdb, 12, buffer.data(), SECTOR_WITH_C2_SIZE, &sk, &a, &aq);

		snprintf(dbg, sizeof(dbg), "CheckC2Support: D8 probe ok=%d sk=0x%02X asc=0x%02X ascq=0x%02X\n",
			ok, sk, a, aq);
		OutputDebugStringA(dbg);

		if (ok || sk == 0x01) {
			m_c2Mode = C2Mode::PlextorD8;
			m_c1BlockErrorsAvailable = true;
			OutputDebugStringA("CheckC2Support: PlextorD8 mode, C1 available\n");
			return true;
		}

		OutputDebugStringA("CheckC2Support: D8 rejected, falling through to standard MMC\n");
	}
	else {
		OutputDebugStringA("CheckC2Support: Not a Plextor/LiteOn vendor, skipping D8\n");
	}

	// Discover the accepted CD-DA read form at a normal sector before the C2
	// probe, so drives that reject the CD-DA-typed 0x04 form (e.g. Pioneer BD
	// burners) are probed with the form they actually accept (sector type 0x00)
	// rather than being mis-reported as having no C2 support.
	EnsureCddaReadForm(0);

	// C2 + block error bits (byte 9 bits 2:1 = 10b -> 0xFC)
	// MMC spec: ErrorPointers returns 296 bytes — 294 C2 pointers + C1/C2
	// block error counts in bytes 294-295.  The drive accepted the 296-byte
	// layout, so bytes 294-295 are structurally present.  A short probe
	// cannot distinguish "drive doesn't populate the field" from "disc is
	// pristine and C1 genuinely = 0".  Enable C1 collection and let the
	// full scan (200k+ sectors) validate — if the drive reports C1, we will
	// see it over a full disc.
	BYTE cdb1[12] = { SCSI_READ_CD, m_cddaSectorType, 0, 0, 0, 0, 0, 0, 1, static_cast<BYTE>(m_cddaMainChannelFlags | 0x04), 0x00, 0 };
	std::vector<BYTE> buffer(SECTOR_WITH_C2_SIZE);
	BYTE sk = 0, a = 0, aq = 0;
	bool ok = SendSCSIWithSense(cdb1, 12, buffer.data(), SECTOR_WITH_C2_SIZE, &sk, &a, &aq);
	if (!ok && sk != 0x01 && !m_cddaReadFormProbed && m_cddaMainChannelFlags == 0xF8) {
		cdb1[9] = 0x10 | 0x04;
		ok = SendSCSIWithSense(cdb1, 12, buffer.data(), SECTOR_WITH_C2_SIZE, &sk, &a, &aq);
		if (ok || sk == 0x01) {
			m_cddaMainChannelFlags = 0x10;
			m_cddaReadFormProbed = true;
			OutputDebugStringA("CheckC2Support: 0xF8+C2 rejected; using 0x10+C2.\n");
		}
	}

	snprintf(dbg, sizeof(dbg), "CheckC2Support: ErrorPointers probe ok=%d sk=0x%02X\n", ok, sk);
	OutputDebugStringA(dbg);

	if (ok || sk == 0x01) {
		m_c2Mode = C2Mode::ErrorPointers;
		// ErrorPointers defines bytes 294-295 as C1/C2 block error counts,
		// but many drives (especially MediaTek-based) accept the 296-byte
		// layout without populating these fields.  Use heuristic probe;
		// for reliable C1 data, use Q-Check (0xE9/0xEB) instead.
		m_c1BlockErrorsAvailable = ProbeC1BlockErrors();
		snprintf(dbg, sizeof(dbg), "CheckC2Support: ErrorPointers mode, C1=%d\n",
			m_c1BlockErrorsAvailable);
		OutputDebugStringA(dbg);
		return true;
	}

	// C2 error block (byte 9 bits 2:1 = 01b -> 0xFA)
	BYTE cdb2[12] = { SCSI_READ_CD, m_cddaSectorType, 0, 0, 0, 0, 0, 0, 1, static_cast<BYTE>(m_cddaMainChannelFlags | 0x02), 0x00, 0 };
	ok = SendSCSIWithSense(cdb2, 12, buffer.data(), SECTOR_WITH_C2_SIZE, &sk, &a, &aq);
	if (!ok && sk != 0x01 && !m_cddaReadFormProbed && m_cddaMainChannelFlags == 0xF8) {
		cdb2[9] = 0x10 | 0x02;
		ok = SendSCSIWithSense(cdb2, 12, buffer.data(), SECTOR_WITH_C2_SIZE, &sk, &a, &aq);
		if (ok || sk == 0x01) {
			m_cddaMainChannelFlags = 0x10;
			m_cddaReadFormProbed = true;
			OutputDebugStringA("CheckC2Support: 0xF8+C2 block rejected; using 0x10+C2.\n");
		}
	}
	if (ok || sk == 0x01) {
		m_c2Mode = C2Mode::ErrorBlock;
		m_c1BlockErrorsAvailable = false;  // ErrorBlock has different layout
		OutputDebugStringA("CheckC2Support: ErrorBlock mode, C1 not available\n");
		return true;
	}

	m_c2Mode = C2Mode::NotSupported;
	m_c1BlockErrorsAvailable = false;
	OutputDebugStringA("CheckC2Support: No C2 mode supported\n");
	return false;
}

bool ScsiDrive::GetDriveInfo(std::string& vendor, std::string& model) {
	BYTE cdb[6] = { 0x12, 0, 0, 0, 96, 0 };
	std::vector<BYTE> buffer(96, 0);
	if (!SendSCSI(cdb, 6, buffer.data(), 96)) return false;

	vendor = std::string(reinterpret_cast<char*>(&buffer[8]), 8);
	model = std::string(reinterpret_cast<char*>(&buffer[16]), 16);
	while (!vendor.empty() && vendor.back() == ' ') vendor.pop_back();
	while (!model.empty() && model.back() == ' ') model.pop_back();
	return true;
}

bool ScsiDrive::GetModePage2A(std::vector<BYTE>& pageData) {
	pageData.clear();

	// Return a normalized vector that starts at page byte 0. Remembering which
	// MODE SENSE form succeeded is essential: guessing from response byte 0
	// fails when a valid MODE SENSE(10) response is longer than 255 bytes.
	{
		static constexpr size_t BUFFER_SIZE = 512;
		std::vector<BYTE> response(BUFFER_SIZE, 0);
		BYTE cdb[10] = { 0x5A, 0x08, 0x2A, 0, 0, 0, 0,
			static_cast<BYTE>(BUFFER_SIZE >> 8), static_cast<BYTE>(BUFFER_SIZE), 0 };
		if (SendSCSI(cdb, 10, response.data(), static_cast<DWORD>(response.size()))) {
			const size_t declared = static_cast<size_t>(
				DriveCapabilityParsing::ReadBE16(response.data())) + 2;
			const size_t available = (std::min)(response.size(), declared);
			const size_t pageOffset = 8 + DriveCapabilityParsing::ReadBE16(response.data() + 6);
			if (pageOffset + 2 <= available
				&& (response[pageOffset] & 0x3F) == 0x2A) {
				const size_t pageBytes = static_cast<size_t>(response[pageOffset + 1]) + 2;
				if (pageOffset + pageBytes <= available) {
					pageData.assign(response.begin() + pageOffset,
						response.begin() + pageOffset + pageBytes);
					return true;
				}
			}
		}
	}

	// Legacy fallback for drives that implement MODE SENSE(6) only.
	{
		static constexpr size_t BUFFER_SIZE = 255;
		std::vector<BYTE> response(BUFFER_SIZE, 0);
		BYTE cdb[6] = { 0x1A, 0x08, 0x2A, 0, static_cast<BYTE>(BUFFER_SIZE), 0 };
		if (!SendSCSI(cdb, 6, response.data(), static_cast<DWORD>(response.size())))
			return false;
		const size_t declared = static_cast<size_t>(response[0]) + 1;
		const size_t available = (std::min)(response.size(), declared);
		const size_t pageOffset = 4 + response[3];
		if (pageOffset + 2 > available || (response[pageOffset] & 0x3F) != 0x2A)
			return false;
		const size_t pageBytes = static_cast<size_t>(response[pageOffset + 1]) + 2;
		if (pageOffset + pageBytes > available)
			return false;
		pageData.assign(response.begin() + pageOffset,
			response.begin() + pageOffset + pageBytes);
		return true;
	}
}

bool ScsiDrive::TestOverread(bool leadIn) {
	BYTE audio[AUDIO_SECTOR_SIZE];

	// Discover the accepted CD-DA read form at a NORMAL sector (LBA 0) — not at
	// the overread LBA below, which is the very capability under test. A drive
	// that rejects the CD-DA-typed 0x04 form (e.g. Pioneer BD burners) would
	// otherwise fail the overread probe for the wrong reason.
	EnsureCddaReadForm(0);

	BYTE cdb[12] = {};
	cdb[0] = SCSI_READ_CD;
	cdb[1] = m_cddaSectorType;
	cdb[8] = 1;
	cdb[9] = 0x10;

	if (leadIn) {
		// The sector immediately before LBA 0 is enough to prove lead-in
		// overread. Testing -150 instead wrongly rejects drives that expose the
		// boundary needed for offset correction but not two full lead-in seconds.
		DWORD negLBA = static_cast<DWORD>(-1);
		cdb[2] = (negLBA >> 24) & 0xFF;
		cdb[3] = (negLBA >> 16) & 0xFF;
		cdb[4] = (negLBA >> 8) & 0xFF;
		cdb[5] = negLBA & 0xFF;
	}
	else {
		// Read actual lead-out LBA from TOC (format 0, track 0xAA)
		// Request LBA format. With MSF=1 (the old 0x02 value in byte 1), bytes
		// 8..11 are 00/MM/SS/FF and cannot be parsed as a 32-bit LBA.
		BYTE tocCdb[10] = { 0x43, 0x00, 0, 0, 0, 0, 0xAA, 0, 12, 0 };
		BYTE tocBuf[12] = {};
		if (!SendSCSI(tocCdb, 10, tocBuf, 12))
			return false;
		DWORD leadOutLBA = DriveCapabilityParsing::ReadBE32(tocBuf + 8);
		// The TOC lead-out address is already the first sector beyond program
		// area; do not skip another sector before testing the boundary.
		cdb[2] = (leadOutLBA >> 24) & 0xFF;
		cdb[3] = (leadOutLBA >> 16) & 0xFF;
		cdb[4] = (leadOutLBA >> 8) & 0xFF;
		cdb[5] = leadOutLBA & 0xFF;
	}

	return SendSCSI(cdb, 12, audio, AUDIO_SECTOR_SIZE);
}

bool ScsiDrive::DetectCapabilities(DriveCapabilities& caps) {
	caps = DriveCapabilities{};

	// Single INQUIRY call for vendor, model, and firmware revision
	BYTE inqCdb[6] = { 0x12, 0, 0, 0, 96, 0 };
	std::vector<BYTE> inqBuffer(96, 0);
	if (!SendSCSI(inqCdb, 6, inqBuffer.data(), 96))
		return false;

	caps.vendor = std::string(reinterpret_cast<char*>(&inqBuffer[8]), 8);
	caps.model = std::string(reinterpret_cast<char*>(&inqBuffer[16]), 16);
	caps.firmware = std::string(reinterpret_cast<char*>(&inqBuffer[32]), 4);
	auto trimBack = [](std::string& s) {
		while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
			s.pop_back();
		};
	trimBack(caps.vendor);
	trimBack(caps.model);
	trimBack(caps.firmware);

	// GET CONFIGURATION RT=2 returns the requested feature descriptor at byte
	// 8 and always carries the current profile in header bytes 6-7.
	auto queryConfiguration = [&](WORD feature, std::vector<BYTE>& response) {
		static constexpr WORD ALLOCATION_LENGTH = 512;
		BYTE cdb[10] = { 0x46, 0x02,
			static_cast<BYTE>(feature >> 8), static_cast<BYTE>(feature),
			0, 0, 0,
			static_cast<BYTE>(ALLOCATION_LENGTH >> 8),
			static_cast<BYTE>(ALLOCATION_LENGTH), 0 };
		response.assign(ALLOCATION_LENGTH, 0);
		if (!SendSCSI(cdb, 10, response.data(), ALLOCATION_LENGTH))
			return false;
		const uint64_t declared = static_cast<uint64_t>(
			DriveCapabilityParsing::ReadBE32(response.data())) + 4;
		response.resize(static_cast<size_t>((std::min<uint64_t>)(
			declared, response.size())));
		return response.size() >= 8;
	};

	auto queryFeature = [&](WORD feature, std::vector<BYTE>& descriptor) {
		std::vector<BYTE> response;
		if (!queryConfiguration(feature, response) || response.size() < 12
			|| DriveCapabilityParsing::ReadBE16(response.data() + 8) != feature)
			return false;
		const size_t descriptorBytes = 4 + response[11];
		if (8 + descriptorBytes > response.size())
			return false;
		descriptor.assign(response.begin() + 8,
			response.begin() + 8 + descriptorBytes);
		return true;
	};

	// Feature 0000h is the authoritative, media-independent list of every
	// profile supported by the drive. It fixes false negatives for DVD+R/RW
	// and avoids inferring whole-drive capability from only the loaded medium.
	{
		std::vector<BYTE> profiles;
		if (queryConfiguration(0x0000, profiles))
			DriveCapabilityParsing::ParseProfileListResponse(
				profiles.data(), profiles.size(), caps);
	}

	// VPD page 0x80: Unit Serial Number
	BYTE vpd80Cdb[6] = { 0x12, 0x01, 0x80, 0, 64, 0 };
	std::vector<BYTE> vpd80Buffer(64, 0);
	if (SendSCSI(vpd80Cdb, 6, vpd80Buffer.data(), 64)) {
		// Validate VPD page code to confirm drive actually supports this page
		if (vpd80Buffer[1] == 0x80) {
			const size_t len = DriveCapabilityParsing::ReadBE16(vpd80Buffer.data() + 2);
			if (len > 0 && len <= vpd80Buffer.size() - 4) {
				caps.serialNumber = std::string(reinterpret_cast<char*>(&vpd80Buffer[4]), len);
				trimBack(caps.serialNumber);
			}
		}
	}

	// MMC Feature 0108h is a standard serial-number source on drives that omit
	// VPD page 80h. Prefer it over transport-specific Windows/ATA fallbacks.
	if (caps.serialNumber.empty()) {
		std::vector<BYTE> serialFeature;
		if (queryFeature(0x0108, serialFeature) && serialFeature.size() > 4) {
			caps.serialNumber.assign(
				reinterpret_cast<const char*>(serialFeature.data() + 4),
				serialFeature.size() - 4);
			trimBack(caps.serialNumber);
		}
	}

	// Fallback 1: query serial number via Windows storage descriptor
	// Many optical drives don't support VPD 0x80 but Windows can still
	// retrieve the serial through the storage stack (IDENTIFY/INQUIRY).
	if (caps.serialNumber.empty() && m_handle != INVALID_HANDLE_VALUE) {
		STORAGE_PROPERTY_QUERY query = {};
		query.PropertyId = StorageDeviceProperty;
		query.QueryType = PropertyStandardQuery;

		BYTE descBuf[1024] = {};
		DWORD ret = 0;
		if (DeviceIoControl(m_handle, IOCTL_STORAGE_QUERY_PROPERTY,
			&query, sizeof(query), descBuf, sizeof(descBuf), &ret, nullptr)) {
			if (ret < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties))
				ret = 0;
			auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(descBuf);
			if (ret != 0 && desc->SerialNumberOffset && desc->SerialNumberOffset < ret
				&& descBuf[desc->SerialNumberOffset]) {
				const char* serial = reinterpret_cast<const char*>(
					descBuf + desc->SerialNumberOffset);
				const size_t maxLength = ret - desc->SerialNumberOffset;
				const size_t length = strnlen_s(serial, maxLength);
				caps.serialNumber.assign(serial, length);
				trimBack(caps.serialNumber);
			}
		}
	}

	// Fallback 2: ATA IDENTIFY PACKET DEVICE (0xA1)
	// SATA/ATAPI optical drives store the serial number at words 10-19
	// (bytes 20-39) of the identify response, byte-swapped per ATA spec.
	if (caps.serialNumber.empty() && m_handle != INVALID_HANDLE_VALUE) {
		struct {
			ATA_PASS_THROUGH_EX header;
			BYTE data[512];
		} ataCmd = {};

		ataCmd.header.Length = sizeof(ATA_PASS_THROUGH_EX);
		ataCmd.header.AtaFlags = ATA_FLAGS_DATA_IN | ATA_FLAGS_DRDY_REQUIRED;
		ataCmd.header.DataTransferLength = 512;
		ataCmd.header.DataBufferOffset = offsetof(decltype(ataCmd), data);
		ataCmd.header.TimeOutValue = 5;
		ataCmd.header.CurrentTaskFile[6] = 0xA1; // IDENTIFY PACKET DEVICE

		DWORD ret = 0;
		if (DeviceIoControl(m_handle, IOCTL_ATA_PASS_THROUGH,
			&ataCmd, sizeof(ataCmd), &ataCmd, sizeof(ataCmd), &ret, nullptr)
			&& ret >= ataCmd.header.DataBufferOffset + 40) {
			// Words 10-19 (bytes 20-39): serial number, byte-swapped pairs
			char serial[21] = {};
			for (int i = 0; i < 20; i += 2) {
				serial[i]     = static_cast<char>(ataCmd.data[20 + i + 1]);
				serial[i + 1] = static_cast<char>(ataCmd.data[20 + i]);
			}
			serial[20] = '\0';
			caps.serialNumber = serial;
			trimBack(caps.serialNumber);
		}
	}

	// Parse Mode Page 2A (CD/DVD Capabilities and Mechanical Status)
	std::vector<BYTE> pageData;
	if (GetModePage2A(pageData))
		DriveCapabilityParsing::ParseModePage2A(
			pageData.data(), pageData.size(), caps);

	// GET CONFIGURATION feature descriptors refine the legacy mode page and
	// expose fields it cannot represent (RAW mastering, CD-Text and BD).
	// Feature 0107h deliberately is not used as a BUP signal: it means Real
	// Time Streaming, not buffer-underrun protection.
	for (WORD feature : { static_cast<WORD>(0x001E), static_cast<WORD>(0x001F),
		static_cast<WORD>(0x002D), static_cast<WORD>(0x002E),
		static_cast<WORD>(0x0040), static_cast<WORD>(0x0041) }) {
		std::vector<BYTE> descriptor;
		if (queryFeature(feature, descriptor))
			DriveCapabilityParsing::ApplyFeatureDescriptor(
				descriptor.data(), descriptor.size(), feature, caps);
	}

	// ────────────────────────────────────────────────────────────────
	// GET PERFORMANCE - query supported speeds (Type 0x03)
	// ────────────────────────────────────────────────────────────────
	// Type 03h returns 16-byte Write Speed Descriptors; bytes 12..15 carry
	// the 32-bit kB/s value. For read speeds, probe standard CD requests and
	// record the actual values the drive reports back.
	static constexpr WORD MAX_PERF_DESCRIPTORS = 128;
	static constexpr int SPEED_BUFFER_SIZE = static_cast<int>(
		DriveCapabilityParsing::GET_PERFORMANCE_HEADER_SIZE
		+ MAX_PERF_DESCRIPTORS * DriveCapabilityParsing::WRITE_SPEED_DESCRIPTOR_SIZE);

	// ── Read speeds: probe by setting speed and reading back actual ─
	// Type 0x03 only returns write speed descriptors, so we probe the
	// drive at standard CD multipliers and record what it actually sets.
	if (caps.supportsCDDA) {
		static const int probeMultipliers[] = { 1, 2, 4, 8, 10, 12, 16, 20, 24, 32, 40, 48, 52 };
		WORD savedRead = m_currentSpeed;
		WORD savedWrite = CD_SPEED_MAX;
		WORD reportedRead = 0, reportedWrite = 0;
		if (GetActualSpeed(reportedRead, reportedWrite)) {
			if (reportedRead > 0 && reportedRead != CD_SPEED_MAX) {
				savedRead = reportedRead;
				caps.currentReadSpeedKB = reportedRead;
			}
			if (reportedWrite > 0 && reportedWrite != CD_SPEED_MAX) {
				savedWrite = reportedWrite;
				caps.currentWriteSpeedKB = reportedWrite;
			}
		}

		for (int mult : probeMultipliers) {
			WORD target = static_cast<WORD>(mult * CD_SPEED_1X);
			BYTE setCdb[12] = {};
			setCdb[0] = SCSI_SET_CD_SPEED;
			setCdb[2] = (target >> 8) & 0xFF;
			setCdb[3] = target & 0xFF;
			setCdb[4] = 0xFF;
			setCdb[5] = 0xFF;
			if (!SendSCSI(setCdb, 12, nullptr, 0, false))
				continue;

			WORD actualRead = 0, actualWrite = 0;
			if (GetActualSpeed(actualRead, actualWrite)
				&& actualRead > 0 && actualRead != CD_SPEED_MAX) {
				char dbgStr[128];
				snprintf(dbgStr, sizeof(dbgStr), "  Probe %dx: requested=%d, actual=%d kB/s\n",
					mult, target, actualRead);
				OutputDebugStringA(dbgStr);
				caps.supportedReadSpeeds.push_back(actualRead);
			}
		}

		// Restore the original speed setting
		BYTE restoreCdb[12] = {};
		restoreCdb[0] = SCSI_SET_CD_SPEED;
		restoreCdb[2] = (savedRead >> 8) & 0xFF;
		restoreCdb[3] = savedRead & 0xFF;
		restoreCdb[4] = (savedWrite >> 8) & 0xFF;
		restoreCdb[5] = savedWrite & 0xFF;
		SendSCSI(restoreCdb, 12, nullptr, 0, false);
	}

	// ── Write speeds: GET PERFORMANCE Type 03h ────────────────────
	// Data Type (byte 1) is reserved for Type 03h, and bytes 8-9 contain a
	// maximum descriptor count rather than an allocation length.
	const bool canWrite = caps.writesCDR || caps.writesCDRW || caps.writesDVD
		|| caps.writesDVDRAM || caps.writesBD;
	if (canWrite) {
		BYTE wperfCdb[12] = { 0xAC, 0x00, 0, 0, 0, 0, 0, 0,
			static_cast<BYTE>(MAX_PERF_DESCRIPTORS >> 8),
			static_cast<BYTE>(MAX_PERF_DESCRIPTORS), 0x03, 0 };
		std::vector<BYTE> wperfBuffer(SPEED_BUFFER_SIZE, 0);
		if (SendSCSI(wperfCdb, 12, wperfBuffer.data(), SPEED_BUFFER_SIZE)) {
			DriveCapabilityParsing::ParseWriteSpeedDescriptors(
				wperfBuffer.data(), wperfBuffer.size(), caps.supportedWriteSpeeds);
		}
	}

	// Deduplicate and sort (GET PERFORMANCE is the authoritative source)
	auto dedup = [](std::vector<int>& v) {
		std::sort(v.begin(), v.end());
		v.erase(std::unique(v.begin(), v.end()), v.end());
		};
	dedup(caps.supportedReadSpeeds);
	dedup(caps.supportedWriteSpeeds);

	if (!caps.supportedReadSpeeds.empty())
		caps.maxReadSpeedKB = (std::max)(caps.maxReadSpeedKB,
			caps.supportedReadSpeeds.back());
	if (!caps.supportedWriteSpeeds.empty())
		caps.maxWriteSpeedKB = (std::max)(caps.maxWriteSpeedKB,
			caps.supportedWriteSpeeds.back());

	// ── Buffer size fallback: READ BUFFER CAPACITY (0x5C) ────────────
	// Mode Page 2A byte 12-13 is the usual source, but some drives (and some
	// USB bridge chipsets) report zero there. Ask the drive directly with
	// READ BUFFER CAPACITY(10): the 12-byte parameter data carries the total
	// buffer length in bytes at offset 4-7. Convert to KB and sanity-clamp to
	// a plausible optical-drive cache range so a garbage response can't skew
	// the accuracy grade. This is the Pioneer opcode chart's section-6 command.
	if (caps.bufferSizeKB == 0) {
		BYTE rbcCdb[10] = { 0x5C, 0, 0, 0, 0, 0, 0, 0, 12, 0 };
		BYTE rbc[12] = {};
		if (SendSCSI(rbcCdb, 10, rbc, sizeof(rbc))) {
			DWORD totalBytes = (static_cast<DWORD>(rbc[4]) << 24)
				| (static_cast<DWORD>(rbc[5]) << 16)
				| (static_cast<DWORD>(rbc[6]) << 8)
				|  static_cast<DWORD>(rbc[7]);
			DWORD kb = totalBytes / 1024;
			// Real optical-drive buffers are ~64 KB .. 128 MB.
			if (kb >= 64 && kb <= 128u * 1024u)
				caps.bufferSizeKB = static_cast<int>(kb);
		}
	}

	DWORD ret = 0;
	const bool storageReady = m_handle != INVALID_HANDLE_VALUE
		&& DeviceIoControl(m_handle, IOCTL_STORAGE_CHECK_VERIFY,
			nullptr, 0, nullptr, 0, &ret, nullptr) != 0;
	caps.mediaPresent = caps.currentMediaProfile != 0 || storageReady;
	if (!caps.mediaPresent)
		caps.currentMediaType = "None";
	else if (caps.currentMediaType.empty() || caps.currentMediaType == "None")
		caps.currentMediaType = "Unknown";

	// Media-dependent probes are meaningful only after a CD-DA sector can be
	// read. Without suitable media, retain standards-advertised capability
	// flags instead of turning an empty tray into a false "NO" result.
	const bool mayContainCD = caps.currentMediaProfile == 0
		|| DriveCapabilityParsing::IsCDProfile(caps.currentMediaProfile);
	if (caps.mediaPresent && mayContainCD) {
		BYTE rawBuffer[AUDIO_SECTOR_SIZE] = {};
		if (ReadCdAudio(0, 1, 0x00, rawBuffer, AUDIO_SECTOR_SIZE)) {
			caps.activeCDReadProbesPerformed = true;
			caps.supportsRawRead = true;
			caps.supportsC2ErrorReporting = CheckC2Support();

			int qTrack = 0, qIndex = 0;
			caps.supportsSubchannelQ = ReadSectorQAnyType(0, qTrack, qIndex);
			caps.supportsOverreadLeadIn = TestOverread(true);
			caps.supportsOverreadLeadOut = TestOverread(false);
		}

		// READ TOC format 5 is the command-level fallback when Feature 001Eh
		// does not advertise CD-Text. Only GOOD/recovered status proves support;
		// unrelated medium errors must not be converted into a positive result.
		if (!caps.supportsCDText) {
			BYTE tocCdb[10] = { 0x43, 0, 0x05, 0, 0, 0, 0, 0, 4, 0 };
			BYTE tocBuf[4] = {};
			BYTE sk = 0, asc = 0, ascq = 0;
			if (SendSCSIWithSense(tocCdb, 10, tocBuf, sizeof(tocBuf),
				&sk, &asc, &ascq))
				caps.supportsCDText = true;
		}
	}

	DriveCharacterization cachedProfile;
	if (LoadDriveCharacterizationProfile(caps.vendor, caps.model,
		caps.firmware, cachedProfile)) {
		ApplyCharacterization(cachedProfile);
	}

	return true;
}

bool ScsiDrive::IsPlextor() {
	std::string vendor, model;
	if (!GetDriveInfo(vendor, model)) return false;
	return vendor.find("PLEXTOR") != std::string::npos ||
		vendor.find("LITE-ON") != std::string::npos ||
		vendor.find("LITEON") != std::string::npos;
}

bool ScsiDrive::IsPioneer() {
	std::string vendor, model;
	if (!GetDriveInfo(vendor, model)) return false;
	return vendor.find("PIONEER") != std::string::npos;
}

bool ScsiDrive::SupportsC1BlockErrors() const {
	return m_c1BlockErrorsAvailable;
}

bool ScsiDrive::ProbeC1BlockErrors() {
	static constexpr int SAMPLES_PER_ZONE = 25;

	BYTE audio[AUDIO_SECTOR_SIZE];
	C2ReadOptions opts;
	opts.countBytes = false;
	opts.defeatCache = false;
	opts.multiPass = false;

	if (!IsOpen()) return false;

	// Get actual disc length from TOC to avoid reading past end
	DWORD discLength = 0;
	BYTE tocCdb[10] = { 0x43, 0x00, 0, 0, 0, 0, 0xAA, 0, 12, 0 };
	BYTE tocBuf[12] = {};
	if (SendSCSI(tocCdb, 10, tocBuf, 12)) {
		discLength = (static_cast<DWORD>(tocBuf[8]) << 24) |
			(static_cast<DWORD>(tocBuf[9]) << 16) |
			(static_cast<DWORD>(tocBuf[10]) << 8) | tocBuf[11];
	}
	if (discLength == 0) discLength = 200000;  // fallback

	// Probe at start, middle, and 75% — all within disc bounds
	DWORD probeLBAs[] = { 0, discLength / 2, discLength * 3 / 4 };

	for (DWORD baseLBA : probeLBAs) {
		for (int i = 0; i < SAMPLES_PER_ZONE; i++) {
			DWORD lba = baseLBA + i;
			if (lba >= discLength) break;
			int c2Errors = 0;
			int c1Block = 0;
			int c2Block = 0;

			bool ok = ReadSectorWithC2Ex(lba, audio, nullptr, c2Errors, nullptr, opts, 
				nullptr, nullptr, nullptr, &c1Block, &c2Block);

			if (ok) {
				if (c1Block > 0) {
					char msg[64];
					snprintf(msg, 64, "ProbeC1: LBA %d returned C1=%d\n", lba, c1Block);
					OutputDebugStringA(msg);
					return true;
				}
			}
		}
	}

	OutputDebugStringA("ProbeC1: All samples returned 0 C1 errors. C1 reporting not supported.\n");
	return false;
}

bool ScsiDrive::ProbeC2Liveness() {
	constexpr int TOTAL_SAMPLES = 200;
	constexpr int ZONES = 4;
	constexpr int SAMPLES_PER_ZONE = TOTAL_SAMPLES / ZONES;
	constexpr DWORD ZONE_STARTS[] = { 0, 50000, 150000, 250000 };

	std::vector<BYTE> buffer(SECTOR_WITH_C2_SIZE);
	C2ReadOptions opts;
	opts.countBytes = false;

	WORD savedSpeed = m_currentSpeed;  // Save current speed
	SetSpeed(0);  // Max speed — best chance of seeing C2

	int totalBytesChecked = 0;
	int nonZeroBytes = 0;

	for (int z = 0; z < ZONES; z++) {
		for (int i = 0; i < SAMPLES_PER_ZONE; i++) {
			DWORD lba = ZONE_STARTS[z] + i * 37;  // stride to avoid sequential read-ahead
			int c2Errors = 0;
			BYTE c2Raw[C2_ERROR_SIZE] = {};

			if (!ReadSectorWithC2Ex(lba, buffer.data(), nullptr, c2Errors,
				c2Raw, opts)) {
				continue;  // Read failure — skip but don't abort
			}

			for (int b = 0; b < C2_POINTER_BYTES; b++) {
				totalBytesChecked++;
				if (c2Raw[b] != 0) nonZeroBytes++;
			}
		}
	}

	SetSpeed(savedSpeed);  // Restore previous speed

	char dbg[128];
	snprintf(dbg, sizeof(dbg), "ProbeC2Liveness: checked %d bytes, %d non-zero\n",
		totalBytesChecked, nonZeroBytes);
	OutputDebugStringA(dbg);

	// If we checked a meaningful sample and saw at least one non-zero C2 byte,
	// the drive can report C2 errors.
	return nonZeroBytes > 0;
}
