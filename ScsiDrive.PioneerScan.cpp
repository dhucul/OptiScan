// ============================================================================
// ScsiDrive.PioneerScan.cpp - Pioneer vendor quality scan implementation
//
// Protocol (ported from QPxTool's PIONEER plugin, plugins/pioneer/qscan_cmd.cpp):
//
//   Send scan request:  CDB  3B 02 E1 00 00 00 00 00 20 00  (WRITE BUFFER)
//   Read scan results:  CDB  3C 02 E1 00 00 00 00 00 20 00  (READ BUFFER)
//
// CD scan payload (32 bytes sent via WRITE BUFFER):
//   Byte 0-1:   FF 01          — enable scan mode
//   Byte 4-6:   LBA + 0x6000   — scan start address (big-endian 24-bit*)
//   Byte 8-10:  sector count   — number of sectors to scan (big-endian 24-bit)
//   Byte 12-14: sector count   — duplicated (big-endian 24-bit)
//   (*) QPxTool writes (lba+0x6000) into bytes 4-5 and the *raw* lba low byte
//       into byte 6; since 0x6000's low byte is 0 the two are identical, so a
//       plain (lba+0x6000) big-endian 24-bit field reproduces it exactly.
//
// Returned results (32 bytes read via READ BUFFER):
//   rd_buf + 5:   E22 diagnostic count (big-endian 16-bit)
//   rd_buf + 13:  BLER / C1 error count (big-endian 16-bit)
//
// CRITICAL SEQUENCING (this is what makes it work on strict drives such as the
// Pioneer BDR-S13U, and is exactly what QPxTool does):
//
//   1. SEEK to the scan start LBA *before* the first WRITE BUFFER.  QPxTool's
//      cmd_cd_errc_init() issues seek(dev, 0) first.  Without positioning the
//      head / spinning the disc up to the start, picky drives answer the scan
//      command with CHECK CONDITION or return all-zero counters — which looks
//      exactly like "the drive doesn't report C2".
//
//   2. Pair WRITE-then-READ within a single poll, then advance the LBA — the
//      same order as QPxTool's cmd_cd_errc_block (cmd_cd_errc_read followed
//      immediately by cmd_cd_errc_getdata, then lba += 75).  Do NOT pipeline a
//      read of the previous slice ahead of the next write.
//
//   3. Never send a "stop" command.  QPxTool's cmd_scan_end is commented out;
//      it simply stops issuing scan commands.  An all-zero WRITE BUFFER to the
//      vendor 0xE1 feature is undocumented and can disturb the scan engine, so
//      PioneerScanStop() only resets host-side state.
//
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

// Build a CD scan payload: 32 bytes sent via WRITE BUFFER.
//
// Mirrors QPxTool cmd_cd_errc_read() for lba/sects.  QPxTool example for
// LBA 0, 75 sectors:  FF 01 00 00  00 60 00 00  00 00 4B 00  00 00 4B 00
static void BuildCDScanPayload(BYTE* payload, DWORD lba, DWORD sectorCount) {
	memset(payload, 0, PIONEER_SCAN_BUFFER_SIZE);

	payload[0] = 0xFF;     // Enable scan mode
	payload[1] = 0x01;

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

// Issue one WRITE BUFFER (0x3B) requesting a scan of [lba, lba+count) and then
// one READ BUFFER (0x3C) to retrieve the result.  This is QPxTool's
// cmd_cd_errc_read + cmd_cd_errc_getdata, performed back-to-back.  Returns
// true if both transports succeeded; fills c1/e22 with the parsed counters
// (already passed through the >300 firmware-garbage guard).
bool ScsiDrive::PioneerScanReadSlice(DWORD lba, DWORD count, int& c1, int& e22,
	bool* outValid) {
	c1 = 0;
	e22 = 0;
	if (outValid) *outValid = false;

	// Phase 1: WRITE BUFFER — request a scan of this slice.
	BYTE writeCdb[10] = {};
	BuildPioneerCDB(writeCdb, 0x3B);

	BYTE payload[PIONEER_SCAN_BUFFER_SIZE] = {};
	BuildCDScanPayload(payload, lba, count);

	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(writeCdb, 10, payload, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq, /*dataIn=*/false);
	// SendSCSIWithSense already treats NO SENSE / RECOVERED ERROR as success.
	// Any false return is therefore a real transport or command failure.  Do
	// not accept an empty sense buffer (SK=0) as a clean, all-zero sample.
	if (!ok)
		return false;

	// Phase 2: READ BUFFER — retrieve the result of the scan just requested.
	BYTE readCdb[10] = {};
	BuildPioneerCDB(readCdb, 0x3C);

	BYTE rdBuf[PIONEER_SCAN_BUFFER_SIZE] = {};
	sk = 0; asc = 0; ascq = 0;
	ok = SendSCSIWithSense(readCdb, 10, rdBuf, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq);
	if (!ok)
		return false;

	// BLER/C1 at offset 13, E22 at offset 5 (big-endian 16-bit). E22 is
	// correctable second-decoder activity; it is not an E32/CU measurement.
	// QPxTool treats CD samples over 300 as invalid firmware garbage.
	int rawC1 = (static_cast<int>(rdBuf[13]) << 8) | rdBuf[14];
	int rawE22 = (static_cast<int>(rdBuf[5]) << 8) | rdBuf[6];
	if (rawC1 > PIONEER_CD_ERRC_VALID_MAX || rawE22 > PIONEER_CD_ERRC_VALID_MAX) {
		char dbg[160];
		snprintf(dbg, sizeof(dbg),
			"PioneerScan: ignoring out-of-range CD sample lba=%lu c1=%d e22=%d\n",
			static_cast<unsigned long>(lba), rawC1, rawE22);
		OutputDebugStringA(dbg);
		s_pioneerInvalidCdSamples++;
	}
	else {
		c1 = rawC1;
		e22 = rawE22;
		if (outValid) *outValid = true;
	}

	return true;
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

	// Mirror QPxTool's cmd_cd_errc_init(): SEEK to the start, then a single
	// WRITE+READ slice.  The seek is the step that lets strict drives (e.g.
	// BDR-S13U) actually answer the vendor scan command instead of returning
	// CHECK CONDITION / zeroes.
	if (!SeekToLBA(0)) {
		if (LastSeekFoundNoMedium())
			std::cout << "  [PioneerScan] No disc in this drive.\n" << std::flush;
		else
			std::cout << "  [PioneerScan] Could not seek to the scan start.\n" << std::flush;
		// A missing/not-ready disc or transient seek failure says nothing about
		// firmware support. Leave the probe uncached so a later ready session can
		// retry instead of being permanently routed to the unreliable C2 path.
		return false;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(150));

	int c1 = 0, e22 = 0;
	bool valid = false;
	bool ok = PioneerScanReadSlice(0, PIONEER_CD_SECTORS_PER_SLICE, c1, e22, &valid);

	char dbg[256];
	snprintf(dbg, sizeof(dbg),
		"PioneerScan: init slice ok=%d c1=%d e22=%d invalid=%d\n",
		ok, c1, e22, s_pioneerInvalidCdSamples);
	OutputDebugStringA(dbg);

	// Reset the probe's invalid-sample bookkeeping so it doesn't leak into the
	// real scan's summary.
	s_pioneerInvalidCdSamples = 0;

	if (ok) {
		m_pioneerScanProbed = 1;
		return true;
	}

	std::cout << "  [PioneerScan] Drive rejected the vendor scan command sequence.\n" << std::flush;
	m_pioneerScanProbed = 0;
	return false;
}

bool ScsiDrive::PioneerScanStart(DWORD startLBA, DWORD endLBA) {
	if (PioneerSliceCount(startLBA, endLBA) == 0)
		return false;

	// QPxTool seeks to the scan start before the first WRITE BUFFER
	// (cmd_cd_errc_init -> seek(dev, 0)).  Position the head and let the disc
	// settle so the first slice returns real counters rather than zeroes.
	if (!SeekToLBA(startLBA)) {
		if (LastSeekFoundNoMedium()) {
			// The vendor scan CDBs answer GOOD with an empty tray, so this seek
			// is the first and only place a missing disc shows up. Say so.
			std::cout << "  [PioneerScan] No disc in this drive; scan not started.\n"
				<< std::flush;
		}
		else {
			std::cout << "  [PioneerScan] Could not seek to LBA " << startLBA
				<< " (SK=0x" << std::hex << static_cast<int>(m_lastSeekSenseKey)
				<< " ASC=0x" << static_cast<int>(m_lastSeekASC)
				<< " ASCQ=0x" << static_cast<int>(m_lastSeekASCQ) << std::dec
				<< "); scan not started.\n" << std::flush;
		}
		return false;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(150));

	s_pioneerLBA = startLBA;
	s_pioneerEndLBA = endLBA;
	s_pioneerInvalidCdSamples = 0;
	return true;
}

bool ScsiDrive::PioneerScanPoll(int& c1, int& e22, int& cu,
	DWORD& currentLBA, bool& scanDone, bool* outValid) {

	c1 = 0;
	e22 = 0;
	cu = 0;            // Pioneer doesn't report CU separately in CD mode
	scanDone = false;
	if (outValid) *outValid = false;

	DWORD count = PioneerSliceCount(s_pioneerLBA, s_pioneerEndLBA);
	if (count == 0) {
		scanDone = true;
		return true;
	}

	// Skip firmware-garbage slices instead of returning them as pristine 0/0
	// measurements. Keep advancing until a valid slice or the end of the scan.
	bool valid = false;
	while (count != 0) {
		const DWORD sliceLBA = s_pioneerLBA;
		if (!PioneerScanReadSlice(sliceLBA, count, c1, e22, &valid)) {
			scanDone = true;
			return false;
		}

		currentLBA = sliceLBA;
		const bool finalSlice = static_cast<unsigned long long>(sliceLBA) + count - 1 >=
			s_pioneerEndLBA;
		if (finalSlice) scanDone = true;
		else s_pioneerLBA += count;
		if (valid || finalSlice) break;
		count = PioneerSliceCount(s_pioneerLBA, s_pioneerEndLBA);
	}

	if (outValid) *outValid = valid;
	return true;
}

bool ScsiDrive::PioneerScanStop() {
	// QPxTool never issues a "stop" — its cmd_scan_end is commented out and it
	// simply stops sending scan commands.  An all-zero WRITE BUFFER to the
	// vendor 0xE1 feature is undocumented and can wedge the scan engine, so we
	// only reset host-side state here.
	s_pioneerLBA = 0;
	s_pioneerEndLBA = 0;
	if (s_pioneerInvalidCdSamples > 0) {
		std::cout << "\n  [PioneerScan] Ignored " << s_pioneerInvalidCdSamples
			<< " out-of-range firmware sample"
			<< (s_pioneerInvalidCdSamples == 1 ? "" : "s")
			<< " (>300).\n" << std::flush;
	}
	s_pioneerInvalidCdSamples = 0;
	return true;
}
