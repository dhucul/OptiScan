// ============================================================================
// ScsiDrive.PioneerScan.cpp - Pioneer vendor quality scan implementation
//
// Protocol (derived from Pioneer firmware analysis):
//
//   Send scan request:  CDB  3B 02 E1 00 00 00 00 00 20 00  (WRITE BUFFER)
//   Read scan results:  CDB  3C 02 E1 00 00 00 00 00 20 00  (READ BUFFER)
//
// CD scan payload (32 bytes):
//   Byte 0-1:   FF 01          — enable scan mode
//   Byte 4-6:   LBA + 0x6000   — scan start address (big-endian 24-bit)
//   Byte 7:     00
//   Byte 8-10:  sector count   — number of sectors to scan (big-endian 24-bit)
//   Byte 11:    00
//   Byte 12-14: sector count   — duplicated (big-endian 24-bit)
//   Byte 15:    00
//
// Returned results (32 bytes):
//   rd_buf + 5:   E22 / C2 error count (big-endian 16-bit)
//   rd_buf + 13:  BLER / C1 error count (big-endian 16-bit)
//
// Two-phase model: send request (WRITE), then retrieve results (READ).
// Each poll covers one time-slice of ~75 sectors (1 second at 1x).
// ============================================================================
#include "ScsiDrive.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstring>
#include <thread>
#include <chrono>

// Pioneer scan state
static DWORD s_pioneerLBA = 0;
static DWORD s_pioneerEndLBA = 0;
static DWORD s_pioneerCurrentCount = 0;
static int s_pioneerInvalidCdSamples = 0;
static constexpr DWORD PIONEER_SCAN_BUFFER_SIZE = 32;
static constexpr DWORD PIONEER_CD_SECTORS_PER_SLICE = 75;   // 1 second at 1x
static constexpr DWORD PIONEER_LBA_OFFSET = 0x6000;
static constexpr int PIONEER_CD_ERRC_VALID_MAX = 300;

// Build the 10-byte CDB common to both WRITE (0x3B) and READ (0x3C)
static void BuildPioneerCDB(BYTE* cdb, BYTE opcode) {
	memset(cdb, 0, 10);
	cdb[0] = opcode;       // 0x3B = WRITE BUFFER, 0x3C = READ BUFFER
	cdb[1] = 0x02;         // Mode / subcommand
	cdb[2] = 0xE1;         // Feature selector (Pioneer vendor-specific)
	cdb[8] = 0x20;         // Transfer length = 32 bytes
}

// Build a CD scan payload: 32 bytes sent via WRITE BUFFER
static void BuildCDScanPayload(BYTE* payload, DWORD lba, DWORD sectorCount) {
	memset(payload, 0, PIONEER_SCAN_BUFFER_SIZE);

	payload[0] = 0xFF;     // Enable scan mode
	payload[1] = 0x01;

	// QPxTool's Pioneer plugin uses 24-bit fields with a trailing zero byte:
	// FF 01 00 00  00 60 00 00  00 00 4B 00  00 00 4B 00
	// for LBA 0, 75 sectors.
	DWORD addr = lba + PIONEER_LBA_OFFSET;
	payload[4] = static_cast<BYTE>((addr >> 16) & 0xFF);
	payload[5] = static_cast<BYTE>((addr >> 8) & 0xFF);
	payload[6] = static_cast<BYTE>(addr & 0xFF);

	// Sector count (duplicated in two positions)
	payload[8] = static_cast<BYTE>((sectorCount >> 16) & 0xFF);
	payload[9] = static_cast<BYTE>((sectorCount >> 8) & 0xFF);
	payload[10] = static_cast<BYTE>(sectorCount & 0xFF);

	payload[12] = static_cast<BYTE>((sectorCount >> 16) & 0xFF);
	payload[13] = static_cast<BYTE>((sectorCount >> 8) & 0xFF);
	payload[14] = static_cast<BYTE>(sectorCount & 0xFF);
}

static DWORD PioneerSliceCount(DWORD lba, DWORD endLBA) {
	if (lba > endLBA)
		return 0;
	DWORD remaining = endLBA - lba + 1;
	return std::min<DWORD>(PIONEER_CD_SECTORS_PER_SLICE, remaining);
}

bool ScsiDrive::SupportsPioneerScan() {
	if (m_pioneerScanProbed >= 0)
		return m_pioneerScanProbed == 1;

	std::string vendor, model;
	GetDriveInfo(vendor, model);

	// Only probe on Pioneer drives
	bool isPioneer = false;
	for (size_t i = 0; i < vendor.size(); i++) {
		if (toupper(static_cast<unsigned char>(vendor[i])) == 'P') {
			if (vendor.size() - i >= 7 &&
				_strnicmp(vendor.c_str() + i, "PIONEER", 7) == 0) {
				isPioneer = true;
				break;
			}
		}
	}

	if (!isPioneer) {
		m_pioneerScanProbed = 0;
		return false;
	}

	std::cout << "  [PioneerScan] Probing scan support on "
		<< vendor << " " << model << "...\n" << std::flush;

	// Send the same 75-sector startup request QPxTool uses.  A header-only
	// payload can be accepted as a no-op by drives that need address/count.
	BYTE cdb[10] = {};
	BuildPioneerCDB(cdb, 0x3B);

	BYTE payload[PIONEER_SCAN_BUFFER_SIZE] = {};
	BuildCDScanPayload(payload, 0, PIONEER_CD_SECTORS_PER_SLICE);

	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 10, payload, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq, /*dataIn=*/false);

	char dbg[256];
	snprintf(dbg, sizeof(dbg),
		"PioneerScan: 0x3B probe ok=%d sk=0x%02X asc=0x%02X ascq=0x%02X\n",
		ok, sk, asc, ascq);
	OutputDebugStringA(dbg);

	if (!ok && sk > 0x01) {
		// WRITE BUFFER rejected outright
		if (asc == 0x20) {
			std::cout << "  [PioneerScan] Drive does not recognize opcode 0x3B (ASC=0x20)\n" << std::flush;
		}
		else {
			std::cout << "  [PioneerScan] Probe rejected (SK=0x"
				<< std::hex << static_cast<int>(sk)
				<< " ASC=0x" << static_cast<int>(asc)
				<< " ASCQ=0x" << static_cast<int>(ascq)
				<< std::dec << ")\n" << std::flush;
		}
		m_pioneerScanProbed = 0;
		return false;
	}

	// WRITE BUFFER accepted — mirror QPxTool's init probe by immediately
	// reading one result block.  Pioneer responses do not have a documented
	// status byte, so transport success is the safest compatibility gate.
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	BYTE readCdb[10] = {};
	BuildPioneerCDB(readCdb, 0x3C);

	BYTE rdBuf[PIONEER_SCAN_BUFFER_SIZE] = {};
	sk = 0; asc = 0; ascq = 0;
	bool readOk = SendSCSIWithSense(readCdb, 10, rdBuf, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq);

	snprintf(dbg, sizeof(dbg),
		"PioneerScan: 0x3C verify ok=%d sk=0x%02X c1=%u c2=%u byte0=0x%02X\n",
		readOk, sk,
		static_cast<unsigned>((rdBuf[13] << 8) | rdBuf[14]),
		static_cast<unsigned>((rdBuf[5] << 8) | rdBuf[6]),
		rdBuf[0]);
	OutputDebugStringA(dbg);

	// Clean up — don't leave the drive in scan mode after probing
	PioneerScanStop();

	if (readOk) {
		std::cout << "  [PioneerScan] Drive supports 0x3B/0x3C quality scan\n" << std::flush;
		m_pioneerScanProbed = 1;
		return true;
	}

	std::cout << "  [PioneerScan] Drive accepted WRITE BUFFER but rejected READ BUFFER"
		<< " (SK=0x" << std::hex << static_cast<int>(sk)
		<< " ASC=0x" << static_cast<int>(asc)
		<< " ASCQ=0x" << static_cast<int>(ascq)
		<< std::dec << ")\n" << std::flush;
	m_pioneerScanProbed = 0;
	return false;
}

bool ScsiDrive::PioneerScanStart(DWORD startLBA, DWORD endLBA) {
	s_pioneerLBA = startLBA;
	s_pioneerEndLBA = endLBA;
	s_pioneerCurrentCount = PioneerSliceCount(startLBA, endLBA);
	s_pioneerInvalidCdSamples = 0;
	if (s_pioneerCurrentCount == 0)
		return false;

	std::cout << "  [PioneerScan] Starting CD scan from LBA "
		<< startLBA << " to " << endLBA << "\n" << std::flush;

	// Issue the first WRITE BUFFER with the starting position
	BYTE cdb[10] = {};
	BuildPioneerCDB(cdb, 0x3B);

	BYTE payload[PIONEER_SCAN_BUFFER_SIZE] = {};
	BuildCDScanPayload(payload, startLBA, s_pioneerCurrentCount);

	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 10, payload, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq, /*dataIn=*/false);

	if (!ok && sk > 0x01) {
		std::cout << "  [PioneerScan] Start command failed (SK=0x"
			<< std::hex << static_cast<int>(sk) << std::dec << ")\n" << std::flush;
		return false;
	}

	return true;
}

bool ScsiDrive::PioneerScanPoll(int& c1, int& c2, int& cu,
	DWORD& currentLBA, bool& scanDone) {

	// Phase 1: READ BUFFER — retrieve results of the last scan slice
	BYTE readCdb[10] = {};
	BuildPioneerCDB(readCdb, 0x3C);

	BYTE rdBuf[PIONEER_SCAN_BUFFER_SIZE] = {};
	BYTE sk = 0, asc = 0, ascq = 0;

	bool ok = SendSCSIWithSense(readCdb, 10, rdBuf, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq);

	if (!ok && sk > 0x01) {
		scanDone = true;
		return false;
	}

	// Extract error counts from the response buffer:
	//   BLER/C1 at offset 13, E22/C2 at offset 5 (big-endian 16-bit).
	// QPxTool's Pioneer plugin treats CD samples over 300 as invalid
	// firmware garbage, not real error counts.  Without this guard, some
	// Pioneer drives report large bogus E22 values that look like heavy C2.
	int rawC1 = (static_cast<int>(rdBuf[13]) << 8) | rdBuf[14];
	int rawC2 = (static_cast<int>(rdBuf[5]) << 8) | rdBuf[6];
	if (rawC1 > PIONEER_CD_ERRC_VALID_MAX || rawC2 > PIONEER_CD_ERRC_VALID_MAX) {
		char dbg[160];
		snprintf(dbg, sizeof(dbg),
			"PioneerScan: ignoring out-of-range CD sample lba=%lu c1=%d c2=%d\n",
			static_cast<unsigned long>(s_pioneerLBA), rawC1, rawC2);
		OutputDebugStringA(dbg);
		c1 = 0;
		c2 = 0;
		s_pioneerInvalidCdSamples++;
	}
	else {
		c1 = rawC1;
		c2 = rawC2;
	}

	// Pioneer doesn't report CU separately in CD mode
	cu = 0;

	currentLBA = s_pioneerLBA;

	// Check if this slice reaches the requested end.  Avoid scheduling a
	// follow-up request past the audio range; the final block may be shorter
	// than 75 sectors.
	if (s_pioneerCurrentCount == 0 ||
		static_cast<unsigned long long>(s_pioneerLBA) + s_pioneerCurrentCount - 1 >= s_pioneerEndLBA) {
		scanDone = true;
		return true;
	}

	scanDone = false;

	// Advance LBA for next slice before issuing the next WRITE
	s_pioneerLBA += s_pioneerCurrentCount;
	s_pioneerCurrentCount = PioneerSliceCount(s_pioneerLBA, s_pioneerEndLBA);
	if (s_pioneerCurrentCount == 0) {
		scanDone = true;
		return true;
	}

	// Phase 2: WRITE BUFFER — send next scan slice request
	BYTE writeCdb[10] = {};
	BuildPioneerCDB(writeCdb, 0x3B);

	BYTE payload[PIONEER_SCAN_BUFFER_SIZE] = {};
	BuildCDScanPayload(payload, s_pioneerLBA, s_pioneerCurrentCount);

	ok = SendSCSIWithSense(writeCdb, 10, payload, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq, /*dataIn=*/false);

	if (!ok && sk > 0x01) {
		// Can't continue scanning, but we did get valid data this round
		scanDone = true;
	}

	return true;
}

bool ScsiDrive::PioneerScanStop() {
	// Send a zero payload to terminate the scan session
	BYTE cdb[10] = {};
	BuildPioneerCDB(cdb, 0x3B);

	BYTE payload[PIONEER_SCAN_BUFFER_SIZE] = {};
	// payload is all zeros — no FF 01 header = stop

	BYTE sk = 0, asc = 0, ascq = 0;
	SendSCSIWithSense(cdb, 10, payload, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq, /*dataIn=*/false);

	s_pioneerLBA = 0;
	s_pioneerEndLBA = 0;
	s_pioneerCurrentCount = 0;
	if (s_pioneerInvalidCdSamples > 0) {
		std::cout << "\n  [PioneerScan] Ignored " << s_pioneerInvalidCdSamples
			<< " out-of-range firmware sample"
			<< (s_pioneerInvalidCdSamples == 1 ? "" : "s")
			<< " (>300).\n" << std::flush;
	}
	s_pioneerInvalidCdSamples = 0;
	return true;
}
