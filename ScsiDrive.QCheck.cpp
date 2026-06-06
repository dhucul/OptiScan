// ============================================================================
// ScsiDrive.QCheck.cpp - Plextor Q-Check vendor command implementation
// ============================================================================
// Uses 0xE9 (start quality scan) and 0xEB (poll C1/C2/CU results) to perform
// the same hardware-driven CIRC error measurement that QPXTool uses.  The
// drive scans at ~1x internally and reports aggregate error statistics per
// time slice without transferring audio data.
//
// Different Plextor generations return the 0xEB response in different layouts.
// We request a large buffer and auto-detect the field positions by locating
// the advancing LBA within the response on the first valid poll.
// ============================================================================
#include "ScsiDrive.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstring>

// Over-allocate the response buffer so we capture the full response
// regardless of whether the drive prepends a 4-byte SCSI header or not.
// Actual responses are 8–16 bytes; 32 gives room for any variant.
static constexpr DWORD QCHECK_BUFFER_SIZE = 32;

// Field offset into the 0xEB response where LBA/C1/C2/CU/status live.
// -1 = not yet detected.  Set once by the first valid poll response.
static int s_qcheckDataOffset = -1;

// Number of raw hex dumps remaining (diagnostic — first N polls)
static int s_diagDumpsRemaining = 0;

// Hex-dump helper for diagnostics — uses std::cout so output is always
// visible in the same console stream as the rest of the scan output.
static void DumpResponseBytes(const BYTE* data, DWORD size, const char* label) {
	std::cout << "\n  [QCheck " << label << "] ";
	for (DWORD i = 0; i < size; i++) {
		std::cout << std::hex << std::setfill('0') << std::setw(2)
			<< static_cast<int>(data[i]);
		if (i < size - 1) std::cout << " ";
	}
	std::cout << std::dec << std::setfill(' ') << "\n" << std::flush;
}

// Try to find where the LBA field lives in the response buffer.
// The LBA should be a non-zero big-endian 32-bit value that falls within
// a plausible CD range (0 .. ~360000).  We test at each candidate offset
// and pick the first one that yields a plausible LBA followed by at least
// 7 more bytes (C1[2] + C2[2] + CU[2] + status[1]).
static int DetectDataOffset(const BYTE* buffer, DWORD bufferSize) {
	constexpr int candidates[] = { 0, 4, 2, 6 };

	for (int off : candidates) {
		if (off + 11 > static_cast<int>(bufferSize)) continue;

		DWORD lba = (static_cast<DWORD>(buffer[off]) << 24) |
			(static_cast<DWORD>(buffer[off + 1]) << 16) |
			(static_cast<DWORD>(buffer[off + 2]) << 8) |
			static_cast<DWORD>(buffer[off + 3]);

		// Plausible CD LBA: 0 < lba < 360000 (80-min CD at 75 sectors/sec)
		if (lba > 0 && lba < 360000) {
			return off;
		}
	}

	return -1;
}

bool ScsiDrive::SupportsQCheck() {
	if (m_qcheckProbed >= 0)
		return m_qcheckProbed == 1;

	std::string vendor, model;
	GetDriveInfo(vendor, model);

	if (!IsPlextor()) {
		m_qcheckProbed = 0;
		return false;
	}

	s_qcheckDataOffset = -1;
	s_diagDumpsRemaining = 0;

	BYTE cdb[12] = {};
	cdb[0] = 0xE9;
	cdb[9] = 150;

	// Try subcommand 0x00 first (C1 scan — most models)
	cdb[1] = 0x00;
	BYTE senseKey = 0xFF, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, nullptr, 0, &senseKey, &asc, &ascq);
	PlextorQCheckStop();

	if (ok || (senseKey <= 0x01 && senseKey != 0xFF)) {
		m_qcheckProbed = 1;
		return true;
	}

	// Try subcommand 0x01 (C2/combined — PX-755/760)
	cdb[1] = 0x01;
	senseKey = 0xFF; asc = 0; ascq = 0;
	ok = SendSCSIWithSense(cdb, 12, nullptr, 0, &senseKey, &asc, &ascq);
	PlextorQCheckStop();

	if (ok || (senseKey <= 0x01 && senseKey != 0xFF)) {
		m_qcheckProbed = 1;
		return true;
	}

	char dbg[160];
	snprintf(dbg, sizeof(dbg),
		"QCheck: unsupported SK=0x%02X ASC=0x%02X ASCQ=0x%02X\n",
		static_cast<int>(senseKey),
		static_cast<int>(asc),
		static_cast<int>(ascq));
	OutputDebugStringA(dbg);

	m_qcheckProbed = 0;
	return false;
}

bool ScsiDrive::PlextorQCheckStart(DWORD startLBA, DWORD endLBA) {
	s_qcheckDataOffset = -1;
	s_diagDumpsRemaining = 5;

	BYTE cdb[12] = {};
	cdb[0] = 0xE9;

	cdb[2] = static_cast<BYTE>((startLBA >> 24) & 0xFF);
	cdb[3] = static_cast<BYTE>((startLBA >> 16) & 0xFF);
	cdb[4] = static_cast<BYTE>((startLBA >> 8) & 0xFF);
	cdb[5] = static_cast<BYTE>(startLBA & 0xFF);

	cdb[6] = static_cast<BYTE>((endLBA >> 24) & 0xFF);
	cdb[7] = static_cast<BYTE>((endLBA >> 16) & 0xFF);
	cdb[8] = static_cast<BYTE>((endLBA >> 8) & 0xFF);
	cdb[9] = static_cast<BYTE>(endLBA & 0xFF);

	// Try subcommand 0x00 first (C1 scan — most Plextor models)
	cdb[1] = 0x00;
	BYTE senseKey = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, nullptr, 0, &senseKey, &asc, &ascq);
	if (ok || senseKey <= 0x01) {
		std::cout << "  [QCheck] Started with subcommand 0x00 (C1 mode)\n" << std::flush;
		return true;
	}

	// Fall back to subcommand 0x01 (C2 scan / combined mode)
	cdb[1] = 0x01;
	senseKey = 0; asc = 0; ascq = 0;
	ok = SendSCSIWithSense(cdb, 12, nullptr, 0, &senseKey, &asc, &ascq);
	if (ok || senseKey <= 0x01) {
		std::cout << "  [QCheck] Started with subcommand 0x01 (C2/combined mode)\n" << std::flush;
		return true;
	}

	std::cout << "  [QCheck] Both subcommands rejected (SK=0x"
		<< std::hex << static_cast<int>(senseKey) << std::dec << ")\n" << std::flush;
	return false;
}

bool ScsiDrive::PlextorQCheckPoll(int& c1, int& c2, int& cu,
	DWORD& currentLBA, bool& scanDone) {
	BYTE cdb[12] = {};
	cdb[0] = 0xEB;
	cdb[1] = 0x01;

	// Allocation length at CDB bytes 7-8 (16-bit big-endian)
	cdb[7] = static_cast<BYTE>((QCHECK_BUFFER_SIZE >> 8) & 0xFF);
	cdb[8] = static_cast<BYTE>(QCHECK_BUFFER_SIZE & 0xFF);

	std::vector<BYTE> buffer(QCHECK_BUFFER_SIZE, 0);
	BYTE senseKey = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, buffer.data(), QCHECK_BUFFER_SIZE,
		&senseKey, &asc, &ascq);

	// ── DIAGNOSTIC: dump BEFORE any sense-key checks ─────────
	// This ensures we see the raw bytes even when the command "fails"
	// with a non-zero sense key.  Without this, sense 0x05 returns
	// early and the dump never executes.
	if (s_diagDumpsRemaining > 0) {
		std::cout << "\n  [QCheck poll] ok=" << (ok ? "true" : "false")
			<< " SK=0x" << std::hex << std::setfill('0') << std::setw(2)
			<< static_cast<int>(senseKey)
			<< " ASC=0x" << std::setw(2) << static_cast<int>(asc)
			<< " ASCQ=0x" << std::setw(2) << static_cast<int>(ascq)
			<< std::dec << std::setfill(' ') << "\n" << std::flush;
		DumpResponseBytes(buffer.data(), QCHECK_BUFFER_SIZE, "raw 0xEB");
		s_diagDumpsRemaining--;
	}

	if (!ok && senseKey > 0x01) {
		if (senseKey == 0x05) {
			c1 = 0; c2 = 0; cu = 0;
			currentLBA = 0;
			scanDone = false;
			return true;
		}
		return false;
	}

	// Auto-detect field offset on first valid response
	if (s_qcheckDataOffset < 0) {
		s_qcheckDataOffset = DetectDataOffset(buffer.data(), QCHECK_BUFFER_SIZE);

		if (s_qcheckDataOffset < 0) {
			std::cout << "\n  [QCheck] WARNING: Cannot detect response layout\n" << std::flush;
			DumpResponseBytes(buffer.data(), QCHECK_BUFFER_SIZE, "unknown layout");
			c1 = 0; c2 = 0; cu = 0;
			currentLBA = 0;
			scanDone = false;
			return true;
		}

		std::cout << "  [QCheck] Detected data offset = " << s_qcheckDataOffset
			<< " bytes into response\n" << std::flush;
	}

	int off = s_qcheckDataOffset;

	currentLBA = (static_cast<DWORD>(buffer[off + 0]) << 24) |
		(static_cast<DWORD>(buffer[off + 1]) << 16) |
		(static_cast<DWORD>(buffer[off + 2]) << 8) |
		static_cast<DWORD>(buffer[off + 3]);

	c1 = (static_cast<int>(buffer[off + 4]) << 8) | buffer[off + 5];
	c2 = (static_cast<int>(buffer[off + 6]) << 8) | buffer[off + 7];
	cu = (static_cast<int>(buffer[off + 8]) << 8) | buffer[off + 9];

	BYTE statusByte = buffer[off + 10];
	scanDone = (statusByte & 0x03) != 0 && currentLBA > 0;

	if (statusByte == 0xFF) {
		scanDone = false;
	}

	return true;
}

bool ScsiDrive::PlextorQCheckStop() {
	BYTE cdb[12] = {};
	cdb[0] = 0xE9;
	cdb[1] = 0x00;

	BYTE senseKey = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, nullptr, 0, &senseKey, &asc, &ascq);
	return ok || senseKey <= 0x01;
}
