// ============================================================================
// ScsiDrive.LiteOnFeTe.cpp - LiteOn/MediaTek focus/tracking-error (FE/TE) scan
//
// Servo-level physical measurement, reverse-engineered from Vinpower's
// libqscan_liteon.dll (cmd_cd_fete_init / cmd_fete_get_data / cmd_fete_get_position):
//   Init:      DF 08 01, byte[4]=0x02 (CD servo mode), LBA in CDB[5..7]
//   Get data:  DF 08 02, byte[3]=0x01, LBA in CDB[4..7]  -> 16-byte slice
//   Position:  DF 02 09                                   -> current head position
//   End:       none required
//
// HARDWARE NOTE (PLEXTOR PX-891SAF PLUS): the DF/08 opcode is accepted but
// returns sk=5 asc=24 (INVALID FIELD IN CDB) when CDB[4..7] is left zero — the
// LBA MUST be populated. DF/02/09 returns GOOD with data. Like the C1/C2 and
// jitter scans, the drive only measures the position the host is READing, so
// each poll drives the head first (LiteOnScanDriveHead).
//
// The reply field layout (FE at bytes[4..5], TE at bytes[6..7], big-endian) is
// TENTATIVE — decoded by analogy to the jitter/beta slice and to be confirmed on
// hardware. Values are surfaced as-is; treat magnitudes as relative, not absolute.
// ============================================================================
#include "ScsiDrive.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <thread>
#include <chrono>

static DWORD s_feteLBA = 0;
static bool  s_feteSawData = false;

bool ScsiDrive::SupportsLiteOnFeTe() {
	if (m_liteonFeTeProbed >= 0)
		return m_liteonFeTeProbed == 1;

	// FE/TE is a MediaTek/LiteOn servo feature — gate on the 0xDF C1/C2 scan so
	// we don't probe unrelated drives with vendor CDBs.
	if (!SupportsLiteOnScan()) { m_liteonFeTeProbed = 0; return false; }

	std::string vendor, model;
	GetDriveInfo(vendor, model);
	char dbg[256];
	snprintf(dbg, sizeof(dbg), "LiteOnFeTe: Probing on '%s' '%s'\n",
		vendor.c_str(), model.c_str());
	OutputDebugStringA(dbg);

	// Drive the head to a real position first — DF/08 rejects a zero LBA with
	// asc=24, so probing at a spun-up sector is necessary to distinguish "opcode
	// unsupported" (asc=20) from "needs a valid position" (asc=24).
	SeekToLBA(0);
	LiteOnScanDriveHead(0, 16);

	BYTE cdb[12] = {};
	cdb[0] = 0xDF; cdb[1] = 0x08; cdb[2] = 0x01; cdb[4] = 0x02;   // init, CD servo mode
	std::vector<BYTE> buf(0x10, 0);
	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x10, &sk, &asc, &ascq);
	snprintf(dbg, sizeof(dbg), "LiteOnFeTe: DF/08/01 init ok=%d sk=0x%02X asc=0x%02X ascq=0x%02X\n",
		ok, sk, asc, ascq);
	OutputDebugStringA(dbg);

	if (!ok && sk > 0x01) {
		if (asc == 0x20)
			std::cout << "  [LiteOnFeTe] asc=0x20: firmware does not implement DF/08\n";
		else if (asc == 0x24)
			std::cout << "  [LiteOnFeTe] asc=0x24: DF/08 recognised but field rejected\n";
		std::cout << std::flush;
		m_liteonFeTeProbed = 0;
		return false;
	}

	// Trial read to confirm the drive implements DF/08. Acceptance of the getdata
	// command is the support signal; non-zero servo values only confirm it. A disc
	// that isn't fully up to speed can momentarily read back zero, and requiring
	// non-zero here wrongly reported a capable drive as unsupported until a rescan
	// (same class of bug as the 0xDF C1/C2 probe).
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	memset(cdb, 0, 12);
	cdb[0] = 0xDF; cdb[1] = 0x08; cdb[2] = 0x02; cdb[3] = 0x01;   // get data (LBA=0)
	std::fill(buf.begin(), buf.end(), BYTE(0));
	bool gdOk = SendSCSIWithSense(cdb, 12, buf.data(), 0x10, &sk, &asc, &ascq);

	bool hasData = false;
	for (int i = 1; i < 8; i++) if (buf[i]) { hasData = true; break; }
	if (hasData || gdOk || sk <= 0x01) {
		std::cout << "  [LiteOnFeTe] Drive supports DF/08 focus/tracking-error scan\n" << std::flush;
		m_liteonFeTeProbed = 1;
		return true;
	}

	std::cout << "  [LiteOnFeTe] DF/08/02 getdata rejected\n" << std::flush;
	m_liteonFeTeProbed = 0;
	return false;
}

bool ScsiDrive::LiteOnFeTeStart(DWORD startLBA, DWORD /*endLBA*/) {
	s_feteLBA = startLBA;
	s_feteSawData = false;
	SeekToLBA(startLBA);

	BYTE cdb[12] = {};
	cdb[0] = 0xDF; cdb[1] = 0x08; cdb[2] = 0x01; cdb[4] = 0x02;   // CD servo mode
	cdb[5] = static_cast<BYTE>((startLBA >> 16) & 0xFF);
	cdb[6] = static_cast<BYTE>((startLBA >> 8) & 0xFF);
	cdb[7] = static_cast<BYTE>(startLBA & 0xFF);
	std::vector<BYTE> buf(0x10, 0);
	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x10, &sk, &asc, &ascq);
	return ok || sk <= 0x01;
}

bool ScsiDrive::LiteOnFeTePoll(int& fe, int& te, DWORD& currentLBA, bool& scanDone) {
	// Drive the head over this interval so the servo measures FE/TE here.
	LiteOnScanDriveHead(s_feteLBA, 75);

	BYTE cdb[12] = {};
	cdb[0] = 0xDF; cdb[1] = 0x08; cdb[2] = 0x02; cdb[3] = 0x01;   // get data
	cdb[4] = static_cast<BYTE>((s_feteLBA >> 24) & 0xFF);
	cdb[5] = static_cast<BYTE>((s_feteLBA >> 16) & 0xFF);
	cdb[6] = static_cast<BYTE>((s_feteLBA >> 8) & 0xFF);
	cdb[7] = static_cast<BYTE>(s_feteLBA & 0xFF);

	std::vector<BYTE> buf(0x10, 0);
	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x10, &sk, &asc, &ascq);
	if (!ok && sk > 0x01) { scanDone = true; return false; }

	// Tentative slice layout (verify on hardware): bytes[1..3]=MSF position,
	// bytes[4..5]=focus error, bytes[6..7]=tracking error (big-endian).
	DWORD msfLBA = static_cast<DWORD>(buf[1]) * 60 * 75
		+ static_cast<DWORD>(buf[2]) * 75
		+ static_cast<DWORD>(buf[3]);

	fe = (static_cast<int>(buf[4]) << 8) | buf[5];
	te = (static_cast<int>(buf[6]) << 8) | buf[7];

	if (msfLBA != 0) s_feteSawData = true;
	currentLBA = msfLBA ? msfLBA : s_feteLBA;
	scanDone = (msfLBA == 0 && s_feteSawData);
	s_feteLBA = currentLBA + 75;
	return true;
}

bool ScsiDrive::LiteOnFeTeStop() {
	// No explicit stop command in the DF/08 servo flow.
	return true;
}
