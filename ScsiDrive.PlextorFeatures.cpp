// ============================================================================
// ScsiDrive.PlextorFeatures.cpp - Plextor vendor feature commands
// ============================================================================
// Implements four Plextor-specific features used by PlexTools / QPxTool:
//
//   PoweRec   (opcode 0xED)         Optimal write-power calibration toggle
//   SpeedRead (opcode 0xE9 / 0xBB)  Unlocks max read speed on CD-ROM media
//   SilentMode(opcode 0xE9 / 0xD8)  Caps spin-up / seek aggressiveness
//   TLA       (opcode 0xF1 / mode 9) Top-Level-Assembly hardware revision
//
// All commands are best-effort and silently fail on non-Plextor drives.
// Selector values follow QPxTool's plextor.cpp reference implementation.
// ============================================================================
#include "ScsiDrive.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include <sstream>
#include <iomanip>

namespace {
constexpr BYTE OP_POWEREC          = 0xED;
constexpr BYTE OP_PLEXTOR_FEATURE  = 0xE9;
constexpr BYTE OP_PLEXTOR_EEPROM   = 0xF1;

constexpr BYTE FEATURE_READ        = 0x00;
constexpr BYTE FEATURE_WRITE       = 0x10;

constexpr BYTE SELECTOR_SPEEDREAD  = 0xBB;
constexpr BYTE SELECTOR_SILENT     = 0xD8;
constexpr BYTE SELECTOR_TESTWRITE  = 0xBE;
constexpr BYTE SELECTOR_VARIREC_CD = 0x02;

constexpr BYTE EEPROM_MODE_TLA     = 0x09;
}

// ─── PoweRec ───────────────────────────────────────────────────────────────
// Plextor's automatic optimal write-power calibration. Smarter than the
// generic SEND OPC INFORMATION (0x54) on supported writers.
//
// CDB layout (12 bytes):
//   [0]  0xED
//   [1]  0x00 read | 0x10 disable | 0x11 enable
//   [9]  0x08         (transfer length)
// Response (read mode, 8 bytes):
//   [0..1] state flags (bit 0 of byte 1 = enabled)
//   remaining bytes = drive-internal calibration data
bool ScsiDrive::GetPoweRec(bool& enabled) {
	enabled = false;
	if (!IsOpen() || !IsPlextor()) return false;

	BYTE cdb[12] = {};
	cdb[0] = OP_POWEREC;
	cdb[1] = 0x00;
	cdb[9] = 0x08;

	BYTE buf[8] = {};
	BYTE sk = 0, asc = 0, ascq = 0;
	if (!SendSCSIWithSense(cdb, 12, buf, sizeof(buf), &sk, &asc, &ascq, /*dataIn=*/true)) {
		return false;
	}

	enabled = (buf[1] & 0x01) != 0;
	return true;
}

bool ScsiDrive::SetPoweRec(bool enable) {
	if (!IsOpen() || !IsPlextor()) return false;

	BYTE cdb[12] = {};
	cdb[0] = OP_POWEREC;
	cdb[1] = enable ? 0x11 : 0x10;
	cdb[9] = 0x08;

	BYTE buf[8] = {};
	BYTE sk = 0, asc = 0, ascq = 0;
	return SendSCSIWithSense(cdb, 12, buf, sizeof(buf), &sk, &asc, &ascq, /*dataIn=*/true);
}

// ─── SpeedRead ─────────────────────────────────────────────────────────────
// Plextor PX-712/716/755/760 ship with read speeds capped on CD-ROM media
// (RPC-derived limit). SpeedRead lifts the cap so the drive runs at its
// physical maximum.
//
// CDB layout (10 bytes):
//   [0]  0xE9
//   [1]  0x10                (write-feature)
//   [2]  0xBB                (SpeedRead selector)
//   [3]  0x00 disable | 0x01 enable
bool ScsiDrive::SetSpeedRead(bool enable) {
	if (!IsOpen() || !IsPlextor()) return false;

	BYTE cdb[10] = {};
	cdb[0] = OP_PLEXTOR_FEATURE;
	cdb[1] = FEATURE_WRITE;
	cdb[2] = SELECTOR_SPEEDREAD;
	cdb[3] = enable ? 0x01 : 0x00;

	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 10, nullptr, 0, &sk, &asc, &ascq, /*dataIn=*/false);
	return ok || sk <= 0x01;
}

// ─── SilentMode ────────────────────────────────────────────────────────────
// Reduces drive noise by capping spin-up speed and softening seek motion.
// Useful for long unattended secure / paranoid rip sessions.
//
// CDB layout (12 bytes):
//   [0]  0xE9
//   [1]  0x00 read | 0x10 write
//   [2]  0xD8                (SilentMode selector)
//   [3]  0x01 active | 0x00 inactive (write only)
//   [9]  0x10                (transfer length, read returns 16-byte block)
// Response (read, 16 bytes):
//   [0]  enabled flag (non-zero = silent active)
//   ... remaining bytes are timing parameters not exposed here
bool ScsiDrive::GetSilentMode(bool& enabled) {
	enabled = false;
	if (!IsOpen() || !IsPlextor()) return false;

	BYTE cdb[12] = {};
	cdb[0] = OP_PLEXTOR_FEATURE;
	cdb[1] = FEATURE_READ;
	cdb[2] = SELECTOR_SILENT;
	cdb[9] = 0x10;

	BYTE buf[16] = {};
	BYTE sk = 0, asc = 0, ascq = 0;
	if (!SendSCSIWithSense(cdb, 12, buf, sizeof(buf), &sk, &asc, &ascq, /*dataIn=*/true)) {
		return false;
	}

	enabled = buf[0] != 0;
	return true;
}

bool ScsiDrive::SetSilentMode(bool enable) {
	if (!IsOpen() || !IsPlextor()) return false;

	BYTE cdb[12] = {};
	cdb[0] = OP_PLEXTOR_FEATURE;
	cdb[1] = FEATURE_WRITE;
	cdb[2] = SELECTOR_SILENT;
	cdb[3] = enable ? 0x01 : 0x00;
	cdb[9] = 0x10;

	BYTE buf[16] = {};
	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, buf, sizeof(buf), &sk, &asc, &ascq, /*dataIn=*/false);
	return ok || sk <= 0x01;
}

// ─── TLA (Top-Level Assembly) ──────────────────────────────────────────────
// Plextor stores a hardware-revision string in EEPROM, exposed via opcode
// 0xF1. TLA disambiguates board revisions of the same model (e.g. PX-716A
// TLA #0301 vs #0308) and is what PlexTools displays on the drive-info page.
//
// CDB layout (12 bytes):
//   [0]  0xF1
//   [1]  0x09                (mode = TLA read)
//   [7]  0x00                (block index, unused for TLA)
//   [8]  0x10                (transfer length, 16 bytes)
// Response (16 bytes): null-padded ASCII TLA code (e.g. "0301").
bool ScsiDrive::GetPlextorTLA(std::string& tla) {
	tla.clear();
	if (!IsOpen() || !IsPlextor()) return false;

	BYTE cdb[12] = {};
	cdb[0] = OP_PLEXTOR_EEPROM;
	cdb[1] = EEPROM_MODE_TLA;
	cdb[8] = 0x10;

	BYTE buf[16] = {};
	BYTE sk = 0, asc = 0, ascq = 0;
	if (!SendSCSIWithSense(cdb, 12, buf, sizeof(buf), &sk, &asc, &ascq, /*dataIn=*/true)) {
		return false;
	}

	// Extract printable ASCII run from the start of the response
	std::ostringstream oss;
	for (BYTE b : buf) {
		if (b >= 0x20 && b < 0x7F) oss << static_cast<char>(b);
		else if (b == 0) break;
	}
	tla = oss.str();

	// Trim trailing whitespace
	while (!tla.empty() && (tla.back() == ' ' || tla.back() == '\t')) tla.pop_back();

	return !tla.empty();
}

// ─── TestWrite ─────────────────────────────────────────────────────────────
// When enabled the drive accepts a full write sequence but keeps the laser
// at read power, simulating the burn end-to-end without consuming the
// blank. Pair every enable with a disable after the write completes.
//
// CDB layout (10 bytes):
//   [0]  0xE9
//   [1]  0x10                (write-feature)
//   [2]  0xBE                (TestWrite selector)
//   [3]  0x01 enable | 0x00 disable
bool ScsiDrive::SetPlextorTestWrite(bool enable) {
	if (!IsOpen() || !IsPlextor()) return false;

	BYTE cdb[10] = {};
	cdb[0] = OP_PLEXTOR_FEATURE;
	cdb[1] = FEATURE_WRITE;
	cdb[2] = SELECTOR_TESTWRITE;
	cdb[3] = enable ? 0x01 : 0x00;

	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 10, nullptr, 0, &sk, &asc, &ascq, /*dataIn=*/false);
	return ok || sk <= 0x01;
}

// ─── VariRec (CD) ──────────────────────────────────────────────────────────
// Lets the host nudge laser write-power off the factory default for the
// inserted CD-R. Useful when the firmware's stock strategy mis-burns
// borderline media. Power offset is a signed step (-4..+4 on most drives).
//
// CDB layout (12 bytes):
//   [0]  0xE9
//   [1]  0x00 read | 0x10 write
//   [2]  0x02                (VariRec-CD selector)
//   [3]  enable flag         (write only)
//   [4]  reserved (firmware strategy index, 0 = default)
//   [5]  signed power offset (write only)
//   [9]  0x10                (transfer length, read returns 16-byte block)
// Response (read, 16 bytes):
//   [0]  enabled flag
//   [2]  signed power offset
bool ScsiDrive::GetVariRecCD(bool& enabled, int& powerOffset) {
	enabled = false;
	powerOffset = 0;
	if (!IsOpen() || !IsPlextor()) return false;

	BYTE cdb[12] = {};
	cdb[0] = OP_PLEXTOR_FEATURE;
	cdb[1] = FEATURE_READ;
	cdb[2] = SELECTOR_VARIREC_CD;
	cdb[9] = 0x10;

	BYTE buf[16] = {};
	BYTE sk = 0, asc = 0, ascq = 0;
	if (!SendSCSIWithSense(cdb, 12, buf, sizeof(buf), &sk, &asc, &ascq, /*dataIn=*/true)) {
		return false;
	}

	enabled = buf[0] != 0;
	powerOffset = static_cast<int8_t>(buf[2]);
	return true;
}

bool ScsiDrive::SetVariRecCD(bool enable, int powerOffset) {
	if (!IsOpen() || !IsPlextor()) return false;
	if (powerOffset < -4 || powerOffset > 4) return false;

	BYTE cdb[12] = {};
	cdb[0] = OP_PLEXTOR_FEATURE;
	cdb[1] = FEATURE_WRITE;
	cdb[2] = SELECTOR_VARIREC_CD;
	cdb[3] = enable ? 0x01 : 0x00;
	cdb[5] = static_cast<BYTE>(static_cast<int8_t>(powerOffset));
	cdb[9] = 0x10;

	BYTE buf[16] = {};
	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, buf, sizeof(buf), &sk, &asc, &ascq, /*dataIn=*/false);
	return ok || sk <= 0x01;
}
