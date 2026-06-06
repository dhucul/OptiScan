// ============================================================================
// ScsiDrive.Capabilities.cpp - Capability and feature detection
// ============================================================================
#include "ScsiDrive.h"
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
	BYTE cdb[10] = {};
	cdb[0] = 0x5A;
	cdb[1] = 0x08;  // DBD = 1 — disable block descriptors
	cdb[2] = 0x2A;
	cdb[7] = 0x00;
	cdb[8] = 0xFF;

	pageData.resize(255, 0);
	if (!SendSCSI(cdb, 10, pageData.data(), 255)) {
		BYTE cdb6[6] = {};
		cdb6[0] = 0x1A;
		cdb6[1] = 0x08;  // DBD = 1 — disable block descriptors
		cdb6[2] = 0x2A;
		cdb6[4] = 0xFF;

		if (!SendSCSI(cdb6, 6, pageData.data(), 255)) {
			return false;
		}
	}
	return true;
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
		DWORD negLBA = static_cast<DWORD>(-150);
		cdb[2] = (negLBA >> 24) & 0xFF;
		cdb[3] = (negLBA >> 16) & 0xFF;
		cdb[4] = (negLBA >> 8) & 0xFF;
		cdb[5] = negLBA & 0xFF;
	}
	else {
		// Read actual lead-out LBA from TOC (format 0, track 0xAA)
		BYTE tocCdb[10] = { 0x43, 0x02, 0, 0, 0, 0, 0xAA, 0, 12, 0 };
		BYTE tocBuf[12] = {};
		DWORD leadOutLBA = 400000; // fallback
		if (SendSCSI(tocCdb, 10, tocBuf, 12)) {
			leadOutLBA = (tocBuf[8] << 24) | (tocBuf[9] << 16) |
				(tocBuf[10] << 8) | tocBuf[11];
		}
		// Test one sector past the lead-out
		leadOutLBA += 1;
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

	// VPD page 0x80: Unit Serial Number
	BYTE vpd80Cdb[6] = { 0x12, 0x01, 0x80, 0, 64, 0 };
	std::vector<BYTE> vpd80Buffer(64, 0);
	if (SendSCSI(vpd80Cdb, 6, vpd80Buffer.data(), 64)) {
		// Validate VPD page code to confirm drive actually supports this page
		if (vpd80Buffer[1] == 0x80) {
			int len = vpd80Buffer[3];
			if (len > 0 && len < 60) {
				caps.serialNumber = std::string(reinterpret_cast<char*>(&vpd80Buffer[4]), len);
				trimBack(caps.serialNumber);
			}
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
			auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(descBuf);
			if (desc->SerialNumberOffset && desc->SerialNumberOffset < ret
				&& descBuf[desc->SerialNumberOffset]) {
				caps.serialNumber = reinterpret_cast<char*>(descBuf + desc->SerialNumberOffset);
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
	if (GetModePage2A(pageData)) {
		if (pageData.size() >= 2) {
			size_t offset = (pageData[0] == 0) ? 8 : 4;

			while (offset + 2 < pageData.size()) {
				if ((pageData[offset] & 0x3F) == 0x2A) {
					int pageLen = pageData[offset + 1];
					size_t pageEnd = offset + 2 + pageLen;

					if (pageEnd > pageData.size())
						break;

					BYTE* page = &pageData[offset];

					if (pageLen >= 4) {
						// Byte 2: Read capabilities
						caps.readsCDR = (page[2] & 0x01) != 0;
						caps.readsCDRW = (page[2] & 0x02) != 0;
						caps.readsDVD = (page[2] & 0x08) != 0;

						// Byte 3: Write capabilities
						caps.writesCDR = (page[3] & 0x01) != 0;
						caps.writesCDRW = (page[3] & 0x02) != 0;
						caps.supportsTestWrite = (page[3] & 0x04) != 0;
						caps.writesDVD = (page[3] & 0x10) != 0;
						caps.writesDVDRAM = (page[3] & 0x20) != 0;
					}

					if (pageLen >= 5) {
						// Byte 4: General device capabilities
						caps.supportsDigitalAudioPlay = (page[4] & 0x01) != 0;
						caps.supportsCompositeOutput = (page[4] & 0x02) != 0;
						caps.supportsMultiSession = (page[4] & 0x40) != 0;
						caps.supportsBufferUnderrunProtection = (page[4] & 0x80) != 0;
					}

					if (pageLen >= 6) {
						// Byte 5: CD-DA / subchannel capabilities
						caps.supportsCDDA = (page[5] & 0x01) != 0;
						caps.supportsAccurateStream = (page[5] & 0x02) != 0;
						caps.supportsSubchannelRaw = (page[5] & 0x04) != 0;
						caps.supportsSubchannelQ = (page[5] & 0x08) != 0;
					}

					if (pageLen >= 7) {
						// Byte 6: Mechanical capabilities
						caps.supportsLockMedia = (page[6] & 0x01) != 0;
						caps.supportsEject = (page[6] & 0x08) != 0;
						caps.isChanger = (page[6] & 0x10) != 0;
						caps.loadingMechanism = (page[6] >> 5) & 0x07;
					}

					if (pageLen >= 8) {
						// Byte 7: Volume / changer capabilities
						caps.supportsSeparateVolume = (page[7] & 0x01) != 0;
						caps.supportsSeparateMute = (page[7] & 0x02) != 0;
					}

					if (pageLen >= 15) {
						// Bytes 8-9: Max read speed (kB/s)
						caps.maxReadSpeedKB = (page[8] << 8) | page[9];
						// Bytes 14-15: Current read speed (kB/s)
						caps.currentReadSpeedKB = (page[14] << 8) | page[15];
						
						char dbgStr[256];
						snprintf(dbgStr, sizeof(dbgStr), "Mode Page 2A: maxReadSpeedKB=%d, currentReadSpeedKB=%d\n", 
							caps.maxReadSpeedKB, caps.currentReadSpeedKB);
						OutputDebugStringA(dbgStr);
					}

					if (pageLen >= 25) {
						// Bytes 24-25: Current write speed (kB/s) — MMC-3+
						caps.currentWriteSpeedKB = (page[24] << 8) | page[25];
						char dbgStr[256];
						snprintf(dbgStr, sizeof(dbgStr), "Mode Page 2A: currentWriteSpeedKB=%d\n", caps.currentWriteSpeedKB);
						OutputDebugStringA(dbgStr);
					}

					// Write speed descriptors (MMC-3+): starting at byte 28
					// Bytes 26-27: total length (in bytes) of descriptor table
					if (pageLen >= 28) {
						int wsdTotalLen = (page[26] << 8) | page[27];
						int numWsd = wsdTotalLen / 4;
						char dbgStr[256];
						snprintf(dbgStr, sizeof(dbgStr), "Mode Page 2A: wsdTotalLen=%d, numWsd=%d\n", wsdTotalLen, numWsd);
						OutputDebugStringA(dbgStr);
						
						caps.maxWriteSpeedKB = 0;
						for (int w = 0; w < numWsd && (28 + w * 4 + 3) < pageLen + 2; w++) {
							int woff = 28 + w * 4;
							int wspeed = (page[woff + 2] << 8) | page[woff + 3];
							if (wspeed > 0) {
								snprintf(dbgStr, sizeof(dbgStr), "  Mode Page 2A WSD[%d]: speed=%d\n", w, wspeed);
								OutputDebugStringA(dbgStr);
								caps.supportedWriteSpeeds.push_back(wspeed);
								if (wspeed > caps.maxWriteSpeedKB)
									caps.maxWriteSpeedKB = wspeed;
							}
						}
					}

					break;
				}
				offset += 2 + pageData[offset + 1];
			}
		}
	}

	// ----------------------------------------------------------------
	// GET CONFIGURATION - query write mode features (TAO, SAO, RAW)
	// ----------------------------------------------------------------
	// Feature 0x002D: CD Track at Once
	{
		std::vector<BYTE> feat;
		BYTE cdb[10] = { 0x46, 0x02, 0x00, 0x2D, 0, 0, 0, 0x00, 64, 0 };
		feat.resize(64, 0);
		if (SendSCSI(cdb, 10, feat.data(), 64)) {
			int dataLen = (feat[0] << 24) | (feat[1] << 16) | (feat[2] << 8) | feat[3];
			if (dataLen >= 12) {
				WORD code = (feat[8] << 8) | feat[9];
				if (code == 0x002D)
					caps.supportsWriteTAO = true;
			}
		}
	}

	// Feature 0x002E: CD Mastering (SAO/DAO)
	{
		std::vector<BYTE> feat;
		BYTE cdb[10] = { 0x46, 0x02, 0x00, 0x2E, 0, 0, 0, 0x00, 64, 0 };
		feat.resize(64, 0);
		if (SendSCSI(cdb, 10, feat.data(), 64)) {
			int dataLen = (feat[0] << 24) | (feat[1] << 16) | (feat[2] << 8) | feat[3];
			if (dataLen >= 12) {
				WORD code = (feat[8] << 8) | feat[9];
				if (code == 0x002E) {
					caps.supportsWriteSAO = true;
					// Bit 0: R-W sub-code in lead-in (CD-Text write support)
					caps.supportsWriteCDText = (feat[12] & 0x01) != 0;
				}
			}
		}
	}

	// Feature 0x0001: Core Feature — check current profile for BD
	{
		std::vector<BYTE> feat;
		BYTE cdb[10] = { 0x46, 0x02, 0x00, 0x01, 0, 0, 0, 0x00, 64, 0 };
		feat.resize(64, 0);
		if (SendSCSI(cdb, 10, feat.data(), 64)) {
			WORD currentProfile = (feat[6] << 8) | feat[7];
			if (currentProfile >= 0x0040 && currentProfile <= 0x004F) {
				caps.readsBD = true;
				// BD-R (0x0041/0x0042) or BD-RE (0x0043) = writable profiles
				if (currentProfile >= 0x0041)
					caps.writesBD = true;
			}
		}
	}

	// Feature 0x0040: BD Read — detect BD read capability even without BD media
	if (!caps.readsBD) {
		std::vector<BYTE> feat;
		BYTE cdb[10] = { 0x46, 0x02, 0x00, 0x40, 0, 0, 0, 0x00, 64, 0 };
		feat.resize(64, 0);
		if (SendSCSI(cdb, 10, feat.data(), 64)) {
			int dataLen = (feat[0] << 24) | (feat[1] << 16) | (feat[2] << 8) | feat[3];
			if (dataLen >= 12) {
				WORD code = (feat[8] << 8) | feat[9];
				if (code == 0x0040)
					caps.readsBD = true;
			}
		}
	}

	// Feature 0x0041: BD Write — detect BD write capability even without BD media
	if (!caps.writesBD) {
		std::vector<BYTE> feat;
		BYTE cdb[10] = { 0x46, 0x02, 0x00, 0x41, 0, 0, 0, 0x00, 64, 0 };
		feat.resize(64, 0);
		if (SendSCSI(cdb, 10, feat.data(), 64)) {
			int dataLen = (feat[0] << 24) | (feat[1] << 16) | (feat[2] << 8) | feat[3];
			if (dataLen >= 12) {
				WORD code = (feat[8] << 8) | feat[9];
				if (code == 0x0041)
					caps.writesBD = true;
			}
		}
	}

	// Feature 0x0107: CD-RW Media Write (Real-Time Streaming / BUP fallback)
	// Some drives report BUP here rather than in Mode Page 2A
	if (!caps.supportsBufferUnderrunProtection) {
		std::vector<BYTE> feat;
		BYTE cdb[10] = { 0x46, 0x02, 0x01, 0x07, 0, 0, 0, 0x00, 64, 0 };
		feat.resize(64, 0);
		if (SendSCSI(cdb, 10, feat.data(), 64)) {
			int dataLen = (feat[0] << 24) | (feat[1] << 16) | (feat[2] << 8) | feat[3];
			if (dataLen >= 12) {
				WORD code = (feat[8] << 8) | feat[9];
				if (code == 0x0107)
					caps.supportsBufferUnderrunProtection = true;
			}
		}
	}

	// ────────────────────────────────────────────────────────────────
	// GET PERFORMANCE - query supported speeds (Type 0x03)
	// ────────────────────────────────────────────────────────────────
	// Type 0x03 returns Write Speed Descriptors (4 bytes each) regardless
	// of the Write bit in byte 1.  For read speeds we probe with SET CD SPEED.
	static constexpr int MAX_PERF_DESCRIPTORS = 128;
	static constexpr int PERF_HEADER_SIZE = 8;
	static constexpr int SPEED_DESC_SIZE = 4;
	static constexpr int SPEED_BUFFER_SIZE = PERF_HEADER_SIZE + MAX_PERF_DESCRIPTORS * SPEED_DESC_SIZE;

	auto parseSpeedDescriptors = [&](BYTE* buf, int bufSize, std::vector<int>& out) {
		if (bufSize < PERF_HEADER_SIZE)
			return;
		int dataLen = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
		int numDesc = (dataLen - 4) / SPEED_DESC_SIZE;
		if (numDesc > MAX_PERF_DESCRIPTORS)
			numDesc = MAX_PERF_DESCRIPTORS;
		
		for (int i = 0; i < numDesc; i++) {
			int off = PERF_HEADER_SIZE + i * SPEED_DESC_SIZE;
			if (off + SPEED_DESC_SIZE > bufSize)
				break;
			BYTE flags = buf[off];
			int speed = (buf[off + 2] << 8) | buf[off + 3];
			
			char dbgStr[128];
			snprintf(dbgStr, sizeof(dbgStr), "  Descriptor[%d]: flags=0x%02X, speed=%d kB/s\n", i, flags, speed);
			OutputDebugStringA(dbgStr);
			
			if (speed > 0)
				out.push_back(speed);
		}
		};

	// ── Read speeds: probe by setting speed and reading back actual ─
	// Type 0x03 only returns write speed descriptors, so we probe the
	// drive at standard CD multipliers and record what it actually sets.
	{
		static const int probeMultipliers[] = { 1, 2, 4, 8, 10, 12, 16, 20, 24, 32, 40, 48, 52 };
		WORD savedSpeed = m_currentSpeed;

		for (int mult : probeMultipliers) {
			WORD target = static_cast<WORD>(mult * CD_SPEED_1X);
			BYTE setCdb[12] = {};
			setCdb[0] = SCSI_SET_CD_SPEED;
			setCdb[2] = (target >> 8) & 0xFF;
			setCdb[3] = target & 0xFF;
			setCdb[4] = 0xFF;
			setCdb[5] = 0xFF;
			BYTE dummy[4] = {};
			SendSCSI(setCdb, 12, dummy, 0, false);

			WORD actualRead = 0, actualWrite = 0;
			if (GetActualSpeed(actualRead, actualWrite) && actualRead > 0) {
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
		restoreCdb[2] = (savedSpeed >> 8) & 0xFF;
		restoreCdb[3] = savedSpeed & 0xFF;
		restoreCdb[4] = 0xFF;
		restoreCdb[5] = 0xFF;
		BYTE dummy[4] = {};
		SendSCSI(restoreCdb, 12, dummy, 0, false);
	}

	// ── Write speeds: Type 0x03, Write=1 ──────────────────────────
	BYTE wperfCdb[12] = { 0xAC, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0x03, 0 };
	int allocLen = SPEED_BUFFER_SIZE;
	wperfCdb[6] = (allocLen >> 24) & 0xFF;
	wperfCdb[7] = (allocLen >> 16) & 0xFF;
	wperfCdb[8] = (allocLen >> 8) & 0xFF;
	wperfCdb[9] = allocLen & 0xFF;
	std::vector<BYTE> wperfBuffer(SPEED_BUFFER_SIZE, 0);
	if (SendSCSI(wperfCdb, 12, wperfBuffer.data(), SPEED_BUFFER_SIZE)) {
		int dataLen = (wperfBuffer[0] << 24) | (wperfBuffer[1] << 16) | (wperfBuffer[2] << 8) | wperfBuffer[3];
		int numDesc = (dataLen - 4) / SPEED_DESC_SIZE;
		OutputDebugStringA((std::string("Write speeds: dataLen=") + std::to_string(dataLen) + " numDesc=" + std::to_string(numDesc) + "\n").c_str());
		parseSpeedDescriptors(wperfBuffer.data(), SPEED_BUFFER_SIZE, caps.supportedWriteSpeeds);
	}

	// Deduplicate and sort (GET PERFORMANCE is the authoritative source)
	auto dedup = [](std::vector<int>& v) {
		std::sort(v.begin(), v.end());
		v.erase(std::unique(v.begin(), v.end()), v.end());
		};
	dedup(caps.supportedReadSpeeds);
	dedup(caps.supportedWriteSpeeds);

	// Derive max write speed from descriptors if Mode Page 2A didn't report it
	if (caps.maxWriteSpeedKB == 0 && !caps.supportedWriteSpeeds.empty()) {
		caps.maxWriteSpeedKB = caps.supportedWriteSpeeds.back();
	}

	DWORD ret;
	caps.mediaPresent = DeviceIoControl(m_handle, IOCTL_STORAGE_CHECK_VERIFY,
		nullptr, 0, nullptr, 0, &ret, nullptr) != 0;

	caps.supportsC2ErrorReporting = CheckC2Support();

	// Probe raw CD-DA read at LBA 0 through the shared helper so the result
	// agrees with the actual rip path — including the 0xF8->0x10 fallback for
	// drives (e.g. Hitachi-LG HL-DT-ST) that reject the richer read form. This
	// also caches the accepted form before the first real read.
	BYTE rawBuffer[AUDIO_SECTOR_SIZE];
	caps.supportsRawRead = ReadCdAudio(0, 1, 0x00, rawBuffer, AUDIO_SECTOR_SIZE);

	// Feature 0x001E: CD Read (includes CD-Text read capability)
	{
		std::vector<BYTE> feat;
		BYTE cdb[10] = { 0x46, 0x02, 0x00, 0x1E, 0, 0, 0, 0x00, 64, 0 };
		feat.resize(64, 0);
		if (SendSCSI(cdb, 10, feat.data(), 64)) {
			int dataLen = (feat[0] << 24) | (feat[1] << 16) | (feat[2] << 8) | feat[3];
			if (dataLen >= 12) {
				WORD code = (feat[8] << 8) | feat[9];
				if (code == 0x001E)
					caps.supportsCDText = (feat[12] & 0x04) != 0;
			}
		}

		// Fallback: probe with READ TOC format 5 (CD-Text)
		if (!caps.supportsCDText && caps.mediaPresent) {
			BYTE tocCdb[10] = {};
			tocCdb[0] = 0x43;
			tocCdb[2] = 0x05;
			tocCdb[7] = 0x00;
			tocCdb[8] = 4;
			std::vector<BYTE> tocBuf(4, 0);
			BYTE sk = 0, a = 0, aq = 0;
			if (SendSCSIWithSense(tocCdb, 10, tocBuf.data(), 4, &sk, &a, &aq)) {
				caps.supportsCDText = true;
			}
			else if (sk != 0x05) {
				caps.supportsCDText = true;
			}
		}

		// Plextor vendor override — these drives always support CD-Text
		if (!caps.supportsCDText && caps.vendor.find("PLEXTOR") != std::string::npos) {
			caps.supportsCDText = true;
		}

		// LiteOn vendor override — most LiteOn drives support CD-Text
		// The iHAS series and many LiteOn CD/DVD drives support CD-Text reading
		if (!caps.supportsCDText && 
			(caps.vendor.find("LITE-ON") != std::string::npos || 
			 caps.vendor.find("LITEON") != std::string::npos)) {
			caps.supportsCDText = true;
		}

		// Optiarc vendor override — Sony NEC Optiarc drives support CD-Text
		// reading via READ TOC format 5 but often don't advertise it in
		// Feature 0x001E.
		if (!caps.supportsCDText) {
			std::string vendorUpper = caps.vendor;
			std::transform(vendorUpper.begin(), vendorUpper.end(), vendorUpper.begin(), ::toupper);
			if (vendorUpper.find("OPTIARC") != std::string::npos) {
				caps.supportsCDText = true;
			}
		}
	}

	// CD-Text write capability was already extracted from Feature 0x002E
	// bit 0 (R-W sub-code in lead-in).  Only vendor overrides below.

	// Plextor writers always support CD-Text writing
	if (!caps.supportsWriteCDText
		&& caps.vendor.find("PLEXTOR") != std::string::npos
		&& caps.writesCDR) {
		caps.supportsWriteCDText = true;
	}

	caps.supportsOverreadLeadIn = TestOverread(true);
	caps.supportsOverreadLeadOut = TestOverread(false);

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
	BYTE tocCdb[10] = { 0x43, 0x02, 0, 0, 0, 0, 0xAA, 0, 12, 0 };
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
