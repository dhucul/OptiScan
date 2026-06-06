// ============================================================================
// ScsiDrive.LiteOnJitter.cpp - LiteOn/MediaTek jitter & beta scan
//
// Protocol derived from QPXTool (cmd_cd_jb_init / cmd_cd_jb_block):
//   Init:  seek to startLBA, then 0xDF/0x1B with CDB[2]=0xA1
//          (some firmwares use 0xA1 only, others accept 0x80 — try fallback)
//   Block: 0xDF/0x1B with CDB[2]=0xA2, CDB[4..7]=LBA (big-endian),
//          returns 12 bytes per slice. Tentative layout (verify on hw):
//            buf[1..3] = MSF (min/sec/frame) of current slice
//            buf[4..5] = jitter (BE16)
//            buf[6..7] = beta   (BE16, signed) — pit/land asymmetry
//   End:   no explicit stop required (mirrors the new-method BLER path).
//
// Mutually exclusive with LiteOnScan* on the same drive — don't run both
// measurement sessions concurrently.
// ============================================================================
#include "ScsiDrive.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <thread>
#include <chrono>

static DWORD s_jitterLBA = 0;
static bool  s_jitterSawData = false;   // first non-zero MSF latches this

bool ScsiDrive::SupportsLiteOnJitter() {
	if (m_liteonJitterProbed >= 0)
		return m_liteonJitterProbed == 1;

	// Gate on the BLER probe — jitter is a LiteOn/MediaTek feature and we
	// don't want to bother non-LiteOn drives with vendor-specific CDBs.
	if (!SupportsLiteOnScan()) { m_liteonJitterProbed = 0; return false; }

	std::string vendor, model;
	GetDriveInfo(vendor, model);
	char dbg[256];
	snprintf(dbg, sizeof(dbg), "LiteOnJitter: Probing on '%s' '%s'\n",
		vendor.c_str(), model.c_str());
	OutputDebugStringA(dbg);

	SeekToLBA(0);

	BYTE cdb[12] = {};
	cdb[0] = 0xDF; cdb[1] = 0x1B; cdb[2] = 0xA1;
	std::vector<BYTE> buf(0x0C, 0);
	BYTE sk = 0, asc = 0, ascq = 0;

	bool ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x0C, &sk, &asc, &ascq);
	snprintf(dbg, sizeof(dbg), "LiteOnJitter: 0xDF/0x1B/0xA1 probe ok=%d sk=0x%02X asc=0x%02X ascq=0x%02X\n",
		ok, sk, asc, ascq);
	OutputDebugStringA(dbg);

	BYTE firstSk = sk, firstAsc = asc, firstAscq = ascq;   // remember for diagnostic

	if (!ok && sk > 0x01) {
		// Try the 0x80 variant some MediaTek firmwares use
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0x1B; cdb[2] = 0x80;
		ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x0C, &sk, &asc, &ascq);
		snprintf(dbg, sizeof(dbg), "LiteOnJitter: 0xDF/0x1B/0x80 fallback ok=%d sk=0x%02X asc=0x%02X ascq=0x%02X\n",
			ok, sk, asc, ascq);
		OutputDebugStringA(dbg);
	}
	if (!ok && sk > 0x01) {
		// Surface the rejection so the user can diagnose without a debugger.
		std::cout << "  [LiteOnJitter] Drive rejected 0xDF/0x1B init"
			<< "  (0xA1: sk=0x" << std::hex << (int)firstSk
			<< " asc=0x" << (int)firstAsc << " ascq=0x" << (int)firstAscq
			<< "; 0x80: sk=0x" << (int)sk
			<< " asc=0x" << (int)asc << " ascq=0x" << (int)ascq << std::dec << ")\n";
		if (firstAsc == 0x20 || asc == 0x20)
			std::cout << "  [LiteOnJitter] asc=0x20: firmware does not implement this opcode\n";
		else if (firstAsc == 0x24 || asc == 0x24)
			std::cout << "  [LiteOnJitter] asc=0x24: opcode recognised but field rejected\n";
		std::cout << "  [LiteOnJitter] This drive likely uses the new (0xF3) command\n"
			<< "                 family, which has no known jitter equivalent.\n"
			<< std::flush;
		m_liteonJitterProbed = 0;
		return false;
	}

	// Trial block read — confirm the drive actually produces jitter data
	// rather than just accepting the init command.
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	memset(cdb, 0, 12);
	cdb[0] = 0xDF; cdb[1] = 0x1B; cdb[2] = 0xA2;
	// LBA = 0 in CDB[4..7] (already zeroed)
	std::fill(buf.begin(), buf.end(), BYTE(0));
	SendSCSIWithSense(cdb, 12, buf.data(), 0x0C, &sk, &asc, &ascq);

	bool hasData = false;
	for (int i = 1; i < 8; i++) if (buf[i]) { hasData = true; break; }
	if (hasData) {
		std::cout << "  [LiteOnJitter] Drive supports 0xDF/0x1B jitter scan\n";
		m_liteonJitterProbed = 1;
		return true;
	}

	OutputDebugStringA("LiteOnJitter: init accepted but trial block returned zeros\n");
	std::cout << "  [LiteOnJitter] Drive accepts 0xDF/0x1B but does not produce data\n"
		<< std::flush;
	m_liteonJitterProbed = 0;
	return false;
}

bool ScsiDrive::LiteOnJitterStart(DWORD startLBA, DWORD /*endLBA*/) {
	s_jitterLBA = startLBA;
	s_jitterSawData = false;
	SeekToLBA(startLBA);

	BYTE cdb[12] = {};
	cdb[0] = 0xDF; cdb[1] = 0x1B; cdb[2] = 0xA1;
	std::vector<BYTE> buf(0x0C, 0);
	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x0C, &sk, &asc, &ascq);

	if (!ok && sk > 0x01) {
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0x1B; cdb[2] = 0x80;
		ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x0C, &sk, &asc, &ascq);
	}
	return ok || sk <= 0x01;
}

bool ScsiDrive::LiteOnJitterPoll(int& jitter, int& beta,
	DWORD& currentLBA, bool& scanDone) {
	BYTE cdb[12] = {};
	cdb[0] = 0xDF; cdb[1] = 0x1B; cdb[2] = 0xA2;
	cdb[4] = static_cast<BYTE>((s_jitterLBA >> 24) & 0xFF);
	cdb[5] = static_cast<BYTE>((s_jitterLBA >> 16) & 0xFF);
	cdb[6] = static_cast<BYTE>((s_jitterLBA >> 8) & 0xFF);
	cdb[7] = static_cast<BYTE>((s_jitterLBA) & 0xFF);

	std::vector<BYTE> buf(0x0C, 0);
	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x0C, &sk, &asc, &ascq);
	if (!ok && sk > 0x01) { scanDone = true; return false; }

	DWORD msfLBA = static_cast<DWORD>(buf[1]) * 60 * 75
		+ static_cast<DWORD>(buf[2]) * 75
		+ static_cast<DWORD>(buf[3]);

	jitter = (static_cast<int>(buf[4]) << 8) | buf[5];
	int16_t betaSigned = static_cast<int16_t>((buf[6] << 8) | buf[7]);
	beta = betaSigned;

	if (msfLBA != 0) s_jitterSawData = true;

	currentLBA = msfLBA ? msfLBA : s_jitterLBA;
	// Only treat msfLBA==0 as end-of-disc after the drive has produced at
	// least one valid sample.  Without this latch, a transient zero on the
	// first poll (drive still seeking) would terminate the scan immediately
	// whenever startLBA > 0.
	scanDone = (msfLBA == 0 && s_jitterSawData);
	s_jitterLBA = currentLBA + 75;   // advance one second
	return true;
}

bool ScsiDrive::LiteOnJitterStop() {
	// No explicit stop in QPXTool's flow for 0xDF/0x1B sessions.
	return true;
}
