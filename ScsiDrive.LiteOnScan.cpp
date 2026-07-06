// ============================================================================
// ScsiDrive.LiteOnScan.cpp - LiteOn/MediaTek CD quality scan
//
// OLD-method (0xDF) protocol HARDWARE-VERIFIED on a PLEXTOR PX-891SAF PLUS
// (a MediaTek/PLDS drive) by reverse-engineering Vinpower's libqscan_liteon.dll
// and running a live C1/C2 scan of a burned CD-R. The init sequence, the
// per-block command triplet, and the C1/C2/CU field offsets below all match.
//
// CRITICAL: a MediaTek/PLDS drive tallies C1/C2 in its CIRC decoder ONLY for
// sectors the host actively reads — it does NOT scan the disc autonomously
// after the DF/A0 arm. So each interval must READ the disc (LiteOnScanDriveHead)
// before DF/82/05 is read, otherwise the counters stay idle at zero (which is
// exactly the "0xDF accepted but trial reads returned all zeros" failure mode).
//
// NEW method (0xF3):  (from QPXTool; not exercised on the PX-891, which is OLD-path)
//   Init:  seek to startLBA, then 0xF3/0x0E probe (16-byte read)
//   Block: 0xF3/0x0E — returns 16 bytes per time slice:
//          byte[1]=min, byte[2]=sec, byte[3]=frame (MSF position)
//          bytes[4-5]=BLER/C1 (BE16), bytes[6-7]=E22/C2 (BE16)
//   End:   no explicit stop needed
//
// OLD method (0xDF):
//   Init:  seek to startLBA, then 0xDF/0xA3, 0xDF/0xA0 sequence (5 commands)
//   Block: READ the interval (drive the head), then
//          0xDF/0x82/0x09 (latch), 0xDF/0x82/0x05 (getdata), 0xDF/0x97 (reset)
//          bytes[0-1]=C1/BLER (BE16), bytes[2-3]=C2/E22 (BE16), byte[4]=CU
//          LBA += 75 per block
//   End:   0xDF/0xA3/0x01
//
// Both methods read at the current drive speed (not locked to 1x).
// At 8x a 72-min disc takes ~9 minutes — same as BLER scan.
#include "ScsiDrive.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstring>
#include <thread>
#include <chrono>

// Which method the drive supports
static bool s_liteonNewMethod = false;

// Current LBA tracking for old method
static DWORD s_liteonLBA = 0;

// ── Head-driving reads (shared by the C1/C2, jitter and FE/TE scans) ────────
// The MediaTek/PLDS error counters only advance for sectors the host reads, so
// the scan must sweep the disc itself. These reads exist purely to move the
// head; the returned data is discarded (only the drive's internal tally,
// fetched via DF/82/05 etc., matters).

void ScsiDrive::LiteOnScanDriveHead(DWORD lba, DWORD sectors) {
	// Chunk <= 16 sectors per read (16 audio sectors = 0x9300 B) to stay under
	// the 16-bit ATAPI transfer ceiling (0xFFFE). Errors are ignored — a
	// defective sector is itself a scan result the drive counts.
	constexpr DWORD CHUNK = 16;
	std::vector<BYTE> buf(CHUNK * AUDIO_SECTOR_SIZE);
	for (DWORD off = 0; off < sectors; off += CHUNK) {
		DWORD n = (sectors - off < CHUNK) ? (sectors - off) : CHUNK;
		ReadCdAudio(lba + off, n, 0x00, buf.data(), n * AUDIO_SECTOR_SIZE);
	}
}

bool ScsiDrive::SupportsLiteOnScan() {
	if (m_liteonScanProbed >= 0)
		return m_liteonScanProbed == 1;

	std::string vendor, model;
	GetDriveInfo(vendor, model);

	char dbg[256];
	snprintf(dbg, sizeof(dbg), "LiteOnScan: Probing on '%s' '%s'\n", vendor.c_str(), model.c_str());
	OutputDebugStringA(dbg);

	// Try NEW method first: seek to 0, then probe 0xF3/0x0E
	SeekToLBA(0);

	BYTE cdb[12] = {};
	cdb[0] = 0xF3;
	cdb[1] = 0x0E;
	std::vector<BYTE> buf(0x10, 0);
	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x10, &sk, &asc, &ascq);

	snprintf(dbg, sizeof(dbg), "LiteOnScan: 0xF3/0x0E probe ok=%d sk=0x%02X asc=0x%02X\n", ok, sk, asc);
	OutputDebugStringA(dbg);

	if (ok || sk <= 0x01) {
		// Verify the response contains actual ERROR-MEASUREMENT data (C1/C2 in
		// bytes 4-7), not merely an MSF position (bytes 1-3). The new (0xF3)
		// method must scan autonomously to be usable here — this poll path does
		// not drive the head. A MediaTek/PLDS drive that only reports position
		// (or all zeros) without host reads correctly falls through to the OLD
		// (0xDF) method below, which DOES drive the head.
		bool hasData = false;
		for (int i = 4; i <= 7; i++) {
			if (buf[i] != 0) { hasData = true; break; }
		}

		if (!hasData) {
			// Drive may still be seeking — retry once after a delay
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			std::fill(buf.begin(), buf.end(), BYTE(0));
			memset(cdb, 0, 12); cdb[0] = 0xF3; cdb[1] = 0x0E;
			ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x10, &sk, &asc, &ascq);
			if (ok || sk <= 0x01) {
				for (int i = 4; i <= 7; i++) {
					if (buf[i] != 0) { hasData = true; break; }
				}
			}
		}

		if (hasData) {
			s_liteonNewMethod = true;
			m_liteonScanProbed = 1;
			return true;
		}

		snprintf(dbg, sizeof(dbg), "LiteOnScan: 0xF3 accepted but response all zeros\n");
		OutputDebugStringA(dbg);
		// Fall through to try OLD method
	}

	// Try OLD method: 0xDF/0xA3 init sequence
	s_liteonNewMethod = false;
	SeekToLBA(0);

	memset(cdb, 0, 12);
	cdb[0] = 0xDF;
	cdb[1] = 0xA3;
	std::vector<BYTE> buf256(256, 0);
	ok = SendSCSIWithSense(cdb, 12, buf256.data(), 256, &sk, &asc, &ascq);

	snprintf(dbg, sizeof(dbg), "LiteOnScan: 0xDF/0xA3 probe ok=%d sk=0x%02X asc=0x%02X\n", ok, sk, asc);
	OutputDebugStringA(dbg);

	if (ok || sk <= 0x01) {
		// Probe accepted — verify the drive actually produces scan data
		// by completing the init sequence and doing trial reads.

		// Steps B-E of old-method init (same as LiteOnScanStart)
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA0; cdb[4] = 0x02;
		SendSCSIWithSense(cdb, 12, buf256.data(), 256, &sk, &asc, &ascq);

		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA0;
		SendSCSIWithSense(cdb, 12, buf256.data(), 256, &sk, &asc, &ascq);

		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA0; cdb[4] = 0x04;
		SendSCSIWithSense(cdb, 12, buf256.data(), 256, &sk, &asc, &ascq);

		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA0; cdb[4] = 0x02;
		SendSCSIWithSense(cdb, 12, buf256.data(), 256, &sk, &asc, &ascq);

		// Trial reads — check a few blocks for non-zero C1/C2/CU.
		// CRITICAL: a MediaTek/PLDS drive tallies errors only for sectors the
		// host reads, so each trial must drive the head first (same as the real
		// scan). Without this the counters stay zero and a fully-capable drive
		// (e.g. the PX-891SAF PLUS) is wrongly reported as unsupported. Sampling
		// a few regions makes a hit likely even on a clean disc.
		bool hasData = false;
		for (int trial = 0; trial < 3 && !hasData; trial++) {
			LiteOnScanDriveHead(static_cast<DWORD>(trial) * 150, 75);

			// Latch interval counters
			memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0x82; cdb[2] = 0x09;
			SendSCSIWithSense(cdb, 12, buf256.data(), 256, &sk, &asc, &ascq);

			// Get data
			std::fill(buf256.begin(), buf256.end(), BYTE(0));
			memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0x82; cdb[2] = 0x05;
			SendSCSIWithSense(cdb, 12, buf256.data(), 256, &sk, &asc, &ascq);

			// Check C1 (bytes 0-1), C2 (bytes 2-3), CU (byte 4)
			for (int i = 0; i <= 4; i++) {
				if (buf256[i] != 0) { hasData = true; break; }
			}

			// Reset interval
			memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0x97;
			SendSCSIWithSense(cdb, 12, buf256.data(), 256, &sk, &asc, &ascq);
		}

		// Stop the scan session
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA3; cdb[2] = 0x01;
		SendSCSIWithSense(cdb, 12, buf256.data(), 256, &sk, &asc, &ascq);

		if (hasData) {
			m_liteonScanProbed = 1;
			return true;
		}

		snprintf(dbg, sizeof(dbg), "LiteOnScan: 0xDF accepted but trial reads returned all zeros\n");
		OutputDebugStringA(dbg);
	}

	OutputDebugStringA("LiteOnScan: No supported scan commands found\n");
	m_liteonScanProbed = 0;
	return false;
}

bool ScsiDrive::LiteOnScanStart(DWORD startLBA, DWORD /*endLBA*/) {
	s_liteonLBA = startLBA;

	if (s_liteonNewMethod) {
		SeekToLBA(startLBA);          // ✅ seeks

		BYTE cdb[12] = {};
		cdb[0] = 0xF3;
		cdb[1] = 0x0E;
		std::vector<BYTE> buf(0x10, 0);
		BYTE sk = 0, asc = 0, ascq = 0;
		bool ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x10, &sk, &asc, &ascq);

		char dbg[128];
		snprintf(dbg, sizeof(dbg), "LiteOnScanStart(new): startLBA=%lu ok=%d sk=0x%02X\n",
			(unsigned long)startLBA, ok, sk);
		OutputDebugStringA(dbg);
		return ok || sk <= 0x01;
	}
	else {
		// OLD: full 5-command init sequence from QPXTool
		SeekToLBA(startLBA);

		std::vector<BYTE> buf(256, 0);
		BYTE cdb[12] = {};
		BYTE sk = 0, asc = 0, ascq = 0;

		// Step A: 0xDF/0xA3
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA3;
		if (!SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq) && sk > 0x01)
			return false;

		// Step B: 0xDF/0xA0 with byte[4]=0x02
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA0; cdb[4] = 0x02;
		if (!SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq) && sk > 0x01)
			return false;

		// Step C: 0xDF/0xA0
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA0;
		if (!SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq) && sk > 0x01)
			return false;

		// Step D: 0xDF/0xA0 with byte[4]=0x04
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA0; cdb[4] = 0x04;
		if (!SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq) && sk > 0x01)
			return false;

		// Step E: 0xDF/0xA0 with byte[4]=0x02
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA0; cdb[4] = 0x02;
		if (!SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq) && sk > 0x01)
			return false;

		OutputDebugStringA("LiteOnScanStart(old): init sequence complete\n");
		return true;
	}
}

bool ScsiDrive::LiteOnScanPoll(int& c1, int& c2, int& cu,
	DWORD& currentLBA, bool& scanDone) {
	BYTE sk = 0, asc = 0, ascq = 0;

	if (s_liteonNewMethod) {
		// NEW: each 0xF3/0x0E call returns one time slice
		BYTE cdb[12] = {};
		cdb[0] = 0xF3;
		cdb[1] = 0x0E;
		std::vector<BYTE> buf(0x10, 0);

		bool ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x10, &sk, &asc, &ascq);
		if (!ok && sk > 0x01) {
			scanDone = true;
			return false;
		}

		// LBA from MSF: byte[1]=min, byte[2]=sec, byte[3]=frame
		currentLBA = static_cast<DWORD>(buf[1]) * 60 * 75
			+ static_cast<DWORD>(buf[2]) * 75
			+ static_cast<DWORD>(buf[3]);

		c1 = (static_cast<int>(buf[4]) << 8) | buf[5];   // BLER
		c2 = (static_cast<int>(buf[6]) << 8) | buf[7];   // E22
		cu = 0;

		// Scan done when LBA stops advancing or returns 0 after data
		scanDone = (currentLBA == 0 && s_liteonLBA > 0);
		s_liteonLBA = currentLBA;
		return true;
	}
	else {
		// OLD: drive the head over this interval, then read the tallied counts.
		// Without the read the MediaTek counters never advance (verified on the
		// PX-891SAF PLUS). One interval = one CD second = 75 sectors.
		LiteOnScanDriveHead(s_liteonLBA, 75);

		std::vector<BYTE> buf(256, 0);
		BYTE cdb[12] = {};

		// 1. Latch interval counters: 0xDF/0x82/0x09
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0x82; cdb[2] = 0x09;
		bool ok = SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq);
		if (!ok && sk > 0x01) { scanDone = true; return false; }

		// 2. Get data: 0xDF/0x82/0x05
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0x82; cdb[2] = 0x05;
		ok = SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq);
		if (!ok && sk > 0x01) { scanDone = true; return false; }

		c1 = (static_cast<int>(buf[0]) << 8) | buf[1];   // BLER
		c2 = (static_cast<int>(buf[2]) << 8) | buf[3];   // E22
		cu = buf[4];                                       // E32/CU

		// 3. Reset interval: 0xDF/0x97
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0x97;
		SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq);

		s_liteonLBA += 75;
		currentLBA = s_liteonLBA;
		scanDone = false;
		return true;
	}
}

bool ScsiDrive::LiteOnScanStop() {
	if (!s_liteonNewMethod) {
		// OLD method: send end command
		BYTE cdb[12] = {};
		cdb[0] = 0xDF;
		cdb[1] = 0xA3;
		cdb[2] = 0x01;
		std::vector<BYTE> buf(256, 0);
		BYTE sk = 0, asc = 0, ascq = 0;
		SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq);
	}
	// NEW method: no explicit stop needed
	return true;
}
