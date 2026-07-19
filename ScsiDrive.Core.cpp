// ============================================================================
// ScsiDrive.Core.cpp - Core SCSI drive communication
// ============================================================================
#include "ScsiDrive.h"
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
	// Allow reasonable tolerance because many drives quantize/approximate speed.
	static bool IsSpeedClose(WORD actualKbPerSec, WORD requestedKbPerSec) {
		if (requestedKbPerSec == 0 || actualKbPerSec == 0) return false;

		int requested = static_cast<int>(requestedKbPerSec);
		int actual = static_cast<int>(actualKbPerSec);
		int delta = std::abs(actual - requested);

		// Max of 1x (~176 KB/s) or 25% tolerance
		int tolerance = (std::max<int>)(static_cast<int>(CD_SPEED_1X), requested / 4);
		return delta <= tolerance;
	}

	static void DebugSpeedMessage(const char* msg) {
		OutputDebugStringA(msg);
	}

	// Logs the speed the drive actually reported (from MODE SENSE 0x2A, in
	// KB/s) alongside the requested multiplier, converting KB/s to approximate
	// x via CD_SPEED_1X (~176 KB/s). `verified` selects the wording: a matched
	// set vs. a quantized mismatch. A non-positive `multiplier` is a "max"
	// request, printed as "max" rather than "0x". The write half is omitted
	// when the drive reports 0 KB/s (common on a read-only operation) to avoid
	// noisy "~0x".
	static void LogActualSpeed(int multiplier, int writeMultiplier,
		WORD actualRead, WORD actualWrite, bool verified) {
		// Round to nearest, not truncate: drives report e.g. 700 KB/s for 4x,
		// and 700/176 truncates to 3. Round so 700 -> ~4x.
		auto kbpsToX = [](WORD kbps) -> int {
			if (CD_SPEED_1X == 0) return 0;
			return (static_cast<int>(kbps) + CD_SPEED_1X / 2) / CD_SPEED_1X;
		};

		// The drive may report the MMC "maximum" sentinel (0xFFFF) as the current
		// speed before it settles; show it as "max" instead of a bogus ~372x.
		const bool readIsMax = (actualRead == CD_SPEED_MAX);
		const bool writeIsMax = (actualWrite == CD_SPEED_MAX);
		const int readX = kbpsToX(actualRead);
		const int writeX = kbpsToX(actualWrite);

		char requested[24];
		if (multiplier <= 0)
			snprintf(requested, sizeof(requested), "max (write=%d)", writeMultiplier);
		else
			snprintf(requested, sizeof(requested), "%dx (write=%d)", multiplier, writeMultiplier);

		const char* verb = verified ? "drive set" : "drive reported";

		char readField[32];
		if (readIsMax)
			snprintf(readField, sizeof(readField), "read max");
		else
			snprintf(readField, sizeof(readField), "read ~%dx (%u KB/s)",
				readX, static_cast<unsigned>(actualRead));

		char dbg[256];
		if (actualWrite == 0) {
			// Read-only operation: drive reports no write speed. Omit the write half.
			snprintf(dbg, sizeof(dbg),
				"TrySetSpeedAndVerify: requested %s; %s %s.\n",
				requested, verb, readField);
		}
		else if (writeIsMax) {
			snprintf(dbg, sizeof(dbg),
				"TrySetSpeedAndVerify: requested %s; %s %s, write max.\n",
				requested, verb, readField);
		}
		else {
			snprintf(dbg, sizeof(dbg),
				"TrySetSpeedAndVerify: requested %s; %s %s, write ~%dx (%u KB/s).\n",
				requested, verb, readField,
				writeX, static_cast<unsigned>(actualWrite));
		}
		DebugSpeedMessage(dbg);
	}
}

// ── Open (lines 30–42) ──────────────────────────────────────────────────
// Opens \\.\X: as a raw device handle with GENERIC_READ | GENERIC_WRITE,
// enabling IOCTL_SCSI_PASS_THROUGH_DIRECT.  Resets cached capability
// probes (Q-Check, LiteOn scan) because the handle may target a
// different physical drive than last time.
//
// ── SendSCSI (lines 51–77) ──────────────────────────────────────────────
// Sends an arbitrary SCSI CDB via SCSI_PASS_THROUGH_DIRECT.  Allocates
// a buffer for the SPT structure plus 32 bytes of sense data, fills in
// the CDB, transfer direction, timeout, and data pointer, then calls
// DeviceIoControl.  Returns true on SCSI status GOOD (0x00); on CHECK
// CONDITION (0x02) the sense bytes are available to callers that use
// SendSCSIWithSense.

bool ScsiDrive::Open(wchar_t driveLetter) {
	std::wstring path = L"\\\\.\\" + std::wstring(1, driveLetter) + L":";
	m_handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

	if (m_handle != INVALID_HANDLE_VALUE) {
		m_driveLetter = driveLetter;   // remember so Reopen() can re-target
		// Reset cached probe results — new handle may be a different drive
		m_qcheckProbed = -1;
		m_liteonScanProbed = -1;
		m_liteonJitterProbed = -1;   // these two were omitted, so a jitter/FE-TE
		m_liteonFeTeProbed = -1;     // verdict leaked across a drive switch
		m_pioneerScanProbed = -1;
		m_pioneerSpeedMode = 0;
		m_cddaMainChannelFlags = 0xF8;  // Re-probe the CD-DA read form per drive
		m_cddaSectorType = 0x04;        // Re-probe the expected sector type too
		m_cddaReadFormProbed = false;
		m_cddaFormDiscovered = false;
		m_lastReadSenseKey = 0;
		m_lastReadASC = 0;
		m_lastReadASCQ = 0;
		m_lowestHonoredSpeed = -1;       // Re-probe the speed floor per drive/media
		m_c2Functional = true;
	}

	return m_handle != INVALID_HANDLE_VALUE;
}

void ScsiDrive::Close() {
	if (m_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(m_handle);
		m_handle = INVALID_HANDLE_VALUE;
	}
	// Closing the handle releases any drive-held door lock; drop the ref-count so
	// a stale value can't suppress the lock CDB after the next Open().
	m_doorLockCount = 0;
}

bool ScsiDrive::Reopen() {
	if (m_driveLetter == 0) return false;   // never opened — nothing to re-target
	wchar_t letter = m_driveLetter;          // Close() leaves m_driveLetter intact,
	Close();                                 // but capture it first to be safe
	return Open(letter);
}

bool ScsiDrive::SendSCSI(void* cdb, BYTE cdbLength, void* buffer, DWORD bufferSize,
	bool dataIn, DWORD timeoutSec) {
	if (m_handle == INVALID_HANDLE_VALUE) return false;

	constexpr DWORD SENSE_SIZE = 32;
	std::vector<BYTE> sptdBuffer(sizeof(SCSI_PASS_THROUGH_DIRECT) + SENSE_SIZE);
	auto* sptd = reinterpret_cast<SCSI_PASS_THROUGH_DIRECT*>(sptdBuffer.data());
	ZeroMemory(sptd, sptdBuffer.size());

	sptd->Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
	sptd->CdbLength = cdbLength;
	sptd->SenseInfoLength = SENSE_SIZE;
	// When buffer is null or size is 0, use SCSI_IOCTL_DATA_UNSPECIFIED — using
	// DATA_OUT with a null buffer causes some SCSI miniport drivers to silently
	// fail or drop the command entirely. SEND OPC INFORMATION (0x54) hits this
	// path. Same fix already applied to SendSCSIWithSense below.
	if (buffer == nullptr || bufferSize == 0) {
		sptd->DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
		sptd->DataTransferLength = 0;
		sptd->DataBuffer = nullptr;
	}
	else {
		sptd->DataIn = dataIn ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT;
		sptd->DataTransferLength = bufferSize;
		sptd->DataBuffer = buffer;
	}
	sptd->TimeOutValue = timeoutSec;
	sptd->SenseInfoOffset = sizeof(SCSI_PASS_THROUGH_DIRECT);
	memcpy(sptd->Cdb, cdb, cdbLength);

	DWORD bytesReturned;
	BOOL result = DeviceIoControl(m_handle, IOCTL_SCSI_PASS_THROUGH_DIRECT,
		sptd, static_cast<DWORD>(sptdBuffer.size()),
		sptd, static_cast<DWORD>(sptdBuffer.size()),
		&bytesReturned, nullptr);

	if (!result) return false;
	if (sptd->ScsiStatus == 0) return true;
	if (sptd->ScsiStatus == 0x02) {
		BYTE* sense = sptdBuffer.data() + sizeof(SCSI_PASS_THROUGH_DIRECT);
		BYTE sk = sense[2] & 0x0F;
		return sk <= 0x01;
	}
	return false;
}

bool ScsiDrive::SendSCSIWithSense(void* cdb, BYTE cdbLength, void* buffer, DWORD bufferSize,
	BYTE* senseKey, BYTE* asc, BYTE* ascq, bool dataIn, DWORD timeoutSec) {
	if (m_handle == INVALID_HANDLE_VALUE) {
		if (senseKey) *senseKey = 0x02;  // Not Ready
		return false;
	}

	constexpr DWORD SENSE_SIZE = 32;
	std::vector<BYTE> sptdBuffer(sizeof(SCSI_PASS_THROUGH_DIRECT) + SENSE_SIZE);
	auto* sptd = reinterpret_cast<SCSI_PASS_THROUGH_DIRECT*>(sptdBuffer.data());
	ZeroMemory(sptd, sptdBuffer.size());

	sptd->Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
	sptd->CdbLength = cdbLength;
	sptd->SenseInfoLength = SENSE_SIZE;

	// FIX: When buffer is null or size is 0, use SCSI_IOCTL_DATA_UNSPECIFIED
	// (no data transfer).  Using DATA_OUT with a null buffer causes some
	// SCSI miniport drivers to silently fail or drop the command entirely.
	if (buffer == nullptr || bufferSize == 0) {
		sptd->DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
		sptd->DataTransferLength = 0;
		sptd->DataBuffer = nullptr;
	}
	else {
		sptd->DataIn = dataIn ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT;
		sptd->DataTransferLength = bufferSize;
		sptd->DataBuffer = buffer;
	}

	sptd->TimeOutValue = timeoutSec;
	sptd->SenseInfoOffset = sizeof(SCSI_PASS_THROUGH_DIRECT);
	memcpy(sptd->Cdb, cdb, cdbLength);

	DWORD bytesReturned;
	BOOL result = DeviceIoControl(m_handle, IOCTL_SCSI_PASS_THROUGH_DIRECT,
		sptd, static_cast<DWORD>(sptdBuffer.size()),
		sptd, static_cast<DWORD>(sptdBuffer.size()),
		&bytesReturned, nullptr);

	BYTE* sense = sptdBuffer.data() + sizeof(SCSI_PASS_THROUGH_DIRECT);
	if (senseKey) *senseKey = sense[2] & 0x0F;
	if (asc) *asc = sense[12];
	if (ascq) *ascq = sense[13];

	if (!result) return false;

	// GOOD status — no error at all
	if (sptd->ScsiStatus == 0) return true;

	// CHECK CONDITION — sense 0x00/0x01 = data buffer is valid
	if (sptd->ScsiStatus == 0x02) {
		BYTE sk = sense[2] & 0x0F;
		return sk <= 0x01;
	}

	return false;
}

void ScsiDrive::SetSpeed(int multiplier, int writeMultiplier) {
	// Preserve legacy behavior: best-effort, no return value.
	(void)TrySetSpeedAndVerify(multiplier, writeMultiplier, nullptr, nullptr);
}

bool ScsiDrive::TrySetSpeedAndVerify(int multiplier, int writeMultiplier,
	WORD* outActualRead, WORD* outActualWrite) {
	m_currentSpeed = multiplier <= 0 ? CD_SPEED_MAX : static_cast<WORD>(multiplier * CD_SPEED_1X);

	// If no explicit write speed given, mirror the read speed
	WORD writeSpeed = CD_SPEED_MAX;
	if (writeMultiplier > 0)
		writeSpeed = static_cast<WORD>(writeMultiplier * CD_SPEED_1X);
	else if (multiplier > 0)
		writeSpeed = m_currentSpeed;

	BYTE cdb[12] = {};
	cdb[0] = SCSI_SET_CD_SPEED;
	cdb[2] = (m_currentSpeed >> 8) & 0xFF;
	cdb[3] = m_currentSpeed & 0xFF;
	cdb[4] = (writeSpeed >> 8) & 0xFF;
	cdb[5] = writeSpeed & 0xFF;

	// Pioneer vendor extension: byte 10 carries a speed-mode qualifier.
	// Bit 7 is always set; bit 6 is the EEP save flag (off so we don't
	// persist to EEPROM on every speed change). Bits 0-5 carry the current
	// session speed-mode value (0 = default/auto, 1 = Performance, 2 = Quiet,
	// 3 = Persistent Quiet) — kept in m_pioneerSpeedMode so an earlier
	// SetCdSpeedPioneer survives plain SetSpeed calls from the rip workflow.
	if (IsPioneer())
		cdb[10] = PIONEER_SPEED_EXT_FLAG | (m_pioneerSpeedMode & 0x3F);

	// Attempt command, then fallback with explicit sense if needed.
	bool sent = SendSCSI(cdb, 12, nullptr, 0, false);
	if (!sent) {
		BYTE sk = 0, asc = 0, ascq = 0;
		sent = SendSCSIWithSense(cdb, 12, nullptr, 0, &sk, &asc, &ascq, false);

		char dbg[160];
		snprintf(dbg, sizeof(dbg),
			"SetSpeed: initial send failed, fallback=%d SK=0x%02X ASC=0x%02X ASCQ=0x%02X\n",
			sent ? 1 : 0, sk, asc, ascq);
		DebugSpeedMessage(dbg);
	}

	if (!sent) {
		return false;
	}

	// For "max" requests, command success is considered success. Only read
	// back the actual speed when a caller wants it (out-pointer supplied), so
	// tight SetSpeed(0) loops don't pay for an extra MODE SENSE each call. When
	// we do read it, log the confirmation line as well since we have the value.
	if (multiplier <= 0) {
		if (outActualRead || outActualWrite) {
			WORD actualRead = 0, actualWrite = 0;
			if (GetActualSpeed(actualRead, actualWrite)) {
				if (outActualRead) *outActualRead = actualRead;
				if (outActualWrite) *outActualWrite = actualWrite;
				LogActualSpeed(multiplier, writeMultiplier, actualRead, actualWrite, true);
			}
		}
		return true;
	}

	const WORD requestedRead = static_cast<WORD>(multiplier * CD_SPEED_1X);
	const WORD requestedWrite = (writeMultiplier > 0)
		? static_cast<WORD>(writeMultiplier * CD_SPEED_1X)
		: requestedRead;

	bool verified = false;
	bool gotAnySpeed = false;
	WORD lastRead = 0, lastWrite = 0;

	for (int attempt = 0; attempt < 3; attempt++) {
		WORD actualRead = 0, actualWrite = 0;
		if (GetActualSpeed(actualRead, actualWrite)) {
			gotAnySpeed = true;
			lastRead = actualRead;
			lastWrite = actualWrite;

			bool readOk = IsSpeedClose(actualRead, requestedRead);
			bool writeOk = IsSpeedClose(actualWrite, requestedWrite);
			if (readOk && writeOk) {
				verified = true;
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(80));
	}

	if (outActualRead) *outActualRead = lastRead;
	if (outActualWrite) *outActualWrite = lastWrite;

	// Log the actual drive-reported speed in both cases: on success to confirm
	// what speed a scan/rip will run at, and on mismatch to show how the drive
	// quantized the request. If every GetActualSpeed call failed, lastRead/
	// lastWrite are still 0 — that's "couldn't read it back," not "drive
	// reported 0," so say so distinctly rather than logging a bogus ~0x.
	if (gotAnySpeed) {
		LogActualSpeed(multiplier, writeMultiplier, lastRead, lastWrite, verified);
	}
	else {
		char dbg[160];
		snprintf(dbg, sizeof(dbg),
			"TrySetSpeedAndVerify: requested %dx (write=%d); command sent but "
			"could not read back actual speed (MODE SENSE 0x2A failed).\n",
			multiplier, writeMultiplier);
		DebugSpeedMessage(dbg);
	}

	return verified;
}

bool ScsiDrive::GetActualSpeed(WORD& readSpeed, WORD& writeSpeed) {
	// Need header (8) + page 2A up to byte 19 (current write speed) + slack
	// for possible block descriptors.
	static constexpr int BUF_SIZE = 64;
	BYTE cdb[10] = { 0x5A, 0, 0x2A, 0, 0, 0, 0, 0, BUF_SIZE, 0 };
	BYTE buffer[BUF_SIZE] = {};

	if (!SendSCSI(cdb, 10, buffer, BUF_SIZE, true))
		return false;

	// MODE SENSE (10) header is 8 bytes; skip any block descriptors.
	int bdLen = (buffer[6] << 8) | buffer[7];
	int pageOff = 8 + bdLen;

	// Sanity: verify page 2A was returned and we have enough data.
	if (pageOff + 20 > BUF_SIZE || (buffer[pageOff] & 0x3F) != 0x2A)
		return false;

	BYTE* page = &buffer[pageOff];
	readSpeed = (page[14] << 8) | page[15];   // Current read speed  (kB/s)
	writeSpeed = (page[18] << 8) | page[19];  // Current write speed (kB/s)

	return true;
}

int ScsiDrive::GetLowestHonoredSpeed(bool apply) {
	// Cached for the open handle — the floor is a fixed drive/media property.
	if (m_lowestHonoredSpeed > 0 && !apply)
		return m_lowestHonoredSpeed;

	// Ask for 1x (the lowest possible) and let the drive clamp up to its real
	// minimum. Reading the speed back tells us exactly what it will honor.
	WORD actualRead = 0, actualWrite = 0;
	int lowest = 1;
	if (TrySetSpeedAndVerify(1, -1, &actualRead, &actualWrite) || actualRead > 0) {
		if (actualRead > 0 && CD_SPEED_1X > 0) {
			// Round to nearest multiplier; clamp to at least 1x.
			lowest = (static_cast<int>(actualRead) + CD_SPEED_1X / 2) / CD_SPEED_1X;
			if (lowest < 1) lowest = 1;
		}
	}
	else {
		// Couldn't read the speed back at all — fall back to requesting 1x.
		lowest = 1;
	}

	m_lowestHonoredSpeed = lowest;

	char dbg[128];
	snprintf(dbg, sizeof(dbg),
		"GetLowestHonoredSpeed: drive floor is %dx (reported %u KB/s).\n",
		lowest, static_cast<unsigned>(actualRead));
	DebugSpeedMessage(dbg);

	// Park the drive at the floor when asked. When apply is false we do NOT
	// restore the prior speed: the probe above already issued SET CD SPEED 1x,
	// so the drive is left requesting 1x (or whatever it clamped to). Callers
	// that care set their own speed immediately after; none currently rely on
	// the pre-probe speed being preserved.
	if (apply)
		SetSpeed(lowest);

	return lowest;
}

// PREVENT ALLOW MEDIUM REMOVAL (0x1E). Reference-counted: the physical lock CDB
// is sent only when the count transitions 0->1, and the unlock CDB only when it
// returns 1->0, so nested DriveDoorLockGuard scopes compose. Prevent=1 sets the
// door-locked bit. A drive that doesn't support it answers CHECK CONDITION,
// which we swallow — locking is best-effort convenience, never a hard gate.
bool ScsiDrive::PreventMediumRemoval(bool prevent) {
	if (m_handle == INVALID_HANDLE_VALUE) return false;

	bool issue;
	BYTE preventBit;
	if (prevent) {
		issue = (m_doorLockCount++ == 0);   // send lock only on the first acquire
		preventBit = 0x01;
	}
	else {
		if (m_doorLockCount == 0) return true;          // already unlocked
		issue = (--m_doorLockCount == 0);   // send unlock only on the last release
		preventBit = 0x00;
	}
	if (!issue) return true;

	BYTE cdb[6] = { 0x1E, 0x00, 0x00, 0x00, preventBit, 0x00 };
	BYTE sk = 0, asc = 0, ascq = 0;
	SendSCSIWithSense(cdb, 6, nullptr, 0, &sk, &asc, &ascq, /*dataIn=*/false);
	return (sk == 0);
}

bool ScsiDrive::Eject() {
	if (m_handle == INVALID_HANDLE_VALUE) return false;
	// Force the tray unlocked first: a scan/rip guard may still hold a lock, and
	// an intentional eject must always win. Reset the ref-count and send the raw
	// unlock so the OS eject below isn't blocked by a door-locked drive.
	if (m_doorLockCount > 0) {
		m_doorLockCount = 0;
		BYTE unlockCdb[6] = { 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00 };
		BYTE sk = 0, asc = 0, ascq = 0;
		SendSCSIWithSense(unlockCdb, 6, nullptr, 0, &sk, &asc, &ascq, /*dataIn=*/false);
	}
	DWORD bytesReturned;
	return DeviceIoControl(m_handle, IOCTL_STORAGE_EJECT_MEDIA,
		nullptr, 0, nullptr, 0, &bytesReturned, nullptr) != 0;
}

// ── Read Error Recovery mode page (0x01) — EXPERIMENTAL ─────────────────────
// Reads mode page 0x01 via MODE SENSE(10) with block descriptors disabled, so the
// page sits at a fixed offset. Copies the whole page (page code..end) into `page`,
// sets `pageBytes` to the total page size, and returns the Read Retry Count (byte
// 3). Returns false if the drive won't return the page or it looks malformed.
bool ScsiDrive::ReadErrorRecoveryPage(BYTE* page, int& pageBytes, BYTE& retryCount) {
	pageBytes = 0;
	retryCount = 0;
	if (m_handle == INVALID_HANDLE_VALUE) return false;

	// MODE SENSE(10): DBD=1 (no block descriptors), PC=0 (current values), page 0x01.
	BYTE cdb[10] = { 0x5A, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 64, 0x00 };
	BYTE buf[64] = {};
	if (!SendSCSI(cdb, 10, buf, sizeof(buf), /*dataIn=*/true)) return false;

	// 8-byte header; block-descriptor length is bytes 6-7 (0 when DBD honored).
	int blockDescLen = (static_cast<int>(buf[6]) << 8) | buf[7];
	int off = 8 + blockDescLen;
	if (off + 2 > static_cast<int>(sizeof(buf))) return false;
	if ((buf[off] & 0x3F) != 0x01) return false;          // not the Read Error Recovery page

	int total = static_cast<int>(buf[off + 1]) + 2;        // page length field + the 2-byte header
	if (total < 4 || total > 32 || off + total > static_cast<int>(sizeof(buf)))
		return false;

	memcpy(page, buf + off, total);
	pageBytes = total;
	retryCount = buf[off + 3];                             // byte 3 = Read Retry Count
	return true;
}

// Writes a modified mode page 0x01 back via MODE SELECT(10). `page` is `pageBytes`
// long (as returned by ReadErrorRecoveryPage). The 8-byte parameter-list header is
// zeroed and the PS/SPF bits are cleared (both must be 0 on MODE SELECT).
bool ScsiDrive::WriteErrorRecoveryPage(const BYTE* page, int pageBytes) {
	if (m_handle == INVALID_HANDLE_VALUE) return false;
	if (pageBytes < 4 || pageBytes > 32) return false;

	BYTE param[8 + 32] = {};
	memcpy(param + 8, page, pageBytes);
	param[8] &= 0x3F;                    // clear PS/SPF — reserved as 0 on MODE SELECT
	int total = 8 + pageBytes;

	BYTE cdb[10] = {};
	cdb[0] = 0x55;                       // MODE SELECT(10)
	cdb[1] = 0x10;                       // PF=1 (page format), SP=0 (do not save to NV)
	cdb[7] = static_cast<BYTE>((total >> 8) & 0xFF);
	cdb[8] = static_cast<BYTE>(total & 0xFF);

	BYTE sk = 0, asc = 0, ascq = 0;
	return SendSCSIWithSense(cdb, 10, param, total, &sk, &asc, &ascq, /*dataIn=*/false)
		&& sk == 0;
}

// ── ReadErrorRecoveryGuard ──────────────────────────────────────────────────
ReadErrorRecoveryGuard::ReadErrorRecoveryGuard(ScsiDrive& drive, int desiredRetry, bool active)
	: m_drive(drive) {
	if (!active || desiredRetry < 0 || desiredRetry > 255) return;

	BYTE page[32] = {};
	int bytes = 0;
	BYTE origRetry = 0;
	if (!m_drive.ReadErrorRecoveryPage(page, bytes, origRetry)) return;  // page unavailable
	m_origRetry = origRetry;
	memcpy(m_savedPage, page, bytes);
	m_savedBytes = bytes;

	if (static_cast<int>(origRetry) == desiredRetry) {
		// Already at the requested value — nothing to change, nothing to restore.
		m_effRetry = origRetry;
		m_honored = true;
		return;
	}

	page[3] = static_cast<BYTE>(desiredRetry);            // byte 3 = Read Retry Count
	if (!m_drive.WriteErrorRecoveryPage(page, bytes)) return;
	m_applied = true;

	// Read back so we can report whether the drive honored (or clamped) the value.
	BYTE verify[32] = {};
	int vbytes = 0;
	BYTE effRetry = 0;
	if (m_drive.ReadErrorRecoveryPage(verify, vbytes, effRetry)) {
		m_effRetry = effRetry;
		m_honored = (static_cast<int>(effRetry) == desiredRetry);
	}
}

ReadErrorRecoveryGuard::~ReadErrorRecoveryGuard() {
	if (m_applied && m_savedBytes > 0) {
		m_drive.WriteErrorRecoveryPage(m_savedPage, m_savedBytes);  // restore original page
	}
}

bool ScsiDrive::TestUnitReady() {
	BYTE cdb[6] = { 0x00 };
	BYTE dummy[4] = {};
	BYTE sk = 0, asc = 0, ascq = 0;
	SendSCSIWithSense(cdb, 6, dummy, 0, &sk, &asc, &ascq, true);
	// sense 0/0/0 = ready. sense 02/3A/xx = Medium Not Present (caller should
	// prompt). 02/04/xx = becoming ready (not ready *yet*, but media is there).
	return (sk == 0);
}

bool ScsiDrive::WaitForDriveReady(int timeoutSeconds) {
	auto start = std::chrono::steady_clock::now();

	while (true) {
		BYTE cdb[6] = { 0x00 };
		BYTE dummy[4] = {};
		BYTE senseKey = 0, asc = 0, ascq = 0;

		SendSCSIWithSense(cdb, 6, dummy, 0, &senseKey, &asc, &ascq, true);

		if (senseKey == 0) return true;

		if (senseKey == 0x06) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		if (senseKey == 0x02 && asc == 0x04) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			continue;
		}

		if (senseKey == 0x02 && asc != 0x3A) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			continue;
		}

		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
		if (elapsed >= timeoutSeconds) return false;

		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
}

bool ScsiDrive::GetMediaStatus(DriveHealthCheck& status) {
	status = DriveHealthCheck{};
	status.driveResponding = IsOpen();
	if (!status.driveResponding) return false;

	BYTE cdb[6] = { 0x00 };
	BYTE dummy[4] = {};
	BYTE senseKey = 0, asc = 0, ascq = 0;

	SendSCSIWithSense(cdb, 6, dummy, 0, &senseKey, &asc, &ascq, true);

	status.mediaReady = (senseKey == 0);
	status.mediaPresent = (senseKey != 0x02 || asc != 0x3A);
	status.trayOpen = (senseKey == 0x02 && asc == 0x3A && ascq == 0x02);
	status.spinningUp = (senseKey == 0x02 && asc == 0x04);

	if (status.mediaPresent && status.mediaReady) {
		BYTE tocCdb[10] = { 0x43, 0x00, 0, 0, 0, 0, 0, 0, 4, 0 };
		std::vector<BYTE> toc(4);
		if (SendSCSI(tocCdb, 10, toc.data(), 4)) {
			status.mediaType = "CD";
		}
	}

	return true;
}

bool ScsiDrive::GetMediaProfile(WORD& profileCode, std::string& profileName) {
	// GET CONFIGURATION (0x46) — request current profile only
	BYTE cdb[10] = { 0x46, 0x01, 0, 0, 0, 0, 0, 0, 16, 0 };
	BYTE buf[16] = {};

	if (!SendSCSI(cdb, 10, buf, 16)) {
		profileCode = 0;
		profileName = "Unknown";
		return false;
	}

	// Current profile is at bytes 6-7 of the feature header
	profileCode = (static_cast<WORD>(buf[6]) << 8) | buf[7];

	switch (profileCode) {
	case 0x0008: profileName = "CD-ROM";  break;   // Factory-pressed CD
	case 0x0009: profileName = "CD-R";    break;   // Recordable CD
	case 0x000A: profileName = "CD-RW";   break;   // Rewritable CD
	case 0x0010: profileName = "DVD-ROM"; break;
	case 0x0011: profileName = "DVD-R";   break;
	case 0x0012: profileName = "DVD-RAM"; break;
	default:     profileName = "Unknown"; break;
	}

	return true;
}

std::string ScsiDrive::GetSenseDescription(BYTE senseKey, BYTE asc, BYTE ascq) {
	switch (senseKey) {
	case 0x00: return "No Sense";
	case 0x01: return "Recovered Error";
	case 0x02:
		if (asc == 0x04) return "Drive Not Ready (spinning up)";
		if (asc == 0x3A) return "Medium Not Present";
		return "Not Ready";
	case 0x03:
		if (asc == 0x02) return "No Seek Complete";
		if (asc == 0x11) return "Unrecovered Read Error";
		if (asc == 0x14) return "Track Following Error";
		return "Medium Error";
	case 0x04: return "Hardware Error";
	case 0x05:
		if (asc == 0x20) return "Illegal Request (invalid command opcode)";
		if (asc == 0x24) return "Illegal Request (invalid field in CDB)";
		if (asc == 0x26) return "Illegal Request (invalid field in parameter list)";
		return "Illegal Request";
	case 0x06: return "Unit Attention (media changed)";
	case 0x0B: return "Aborted Command";
	default: return "Unknown Error (0x" + std::to_string(senseKey) + ")";
	}
}

bool ScsiDrive::RequestSenseProgress(BYTE& senseKey, BYTE& asc, BYTE& ascq, int& progressPercent) {
	progressPercent = -1;
	if (m_handle == INVALID_HANDLE_VALUE) return false;

	// REQUEST SENSE (0x03) — 18 bytes for fixed-format sense with SKS fields
	BYTE cdb[6] = { 0x03, 0x00, 0x00, 0x00, 18, 0x00 };
	BYTE sense[18] = {};

	if (!SendSCSI(cdb, sizeof(cdb), sense, sizeof(sense), true))
		return false;

	// Fixed-format sense: response code 0x70 or 0x71
	if ((sense[0] & 0x7F) != 0x70 && (sense[0] & 0x7F) != 0x71)
		return false;

	senseKey = sense[2] & 0x0F;
	asc = sense[12];
	ascq = sense[13];

	// Sense Key Specific Valid (SKSV) = bit 7 of byte 15
	// Progress indication in bytes 16-17 (big-endian, 0x0000–0xFFFF)
	if (sense[15] & 0x80) {
		WORD progress = (static_cast<WORD>(sense[16]) << 8) | sense[17];
		progressPercent = static_cast<int>(progress * 100UL / 0xFFFF);
	}

	return true;
}

bool ScsiDrive::SpinDown() {
	if (m_handle == INVALID_HANDLE_VALUE) return false;

	// START STOP UNIT (0x1B): Start=0, LoEj=0 → spin down without ejecting
	BYTE cdb[6] = {};
	cdb[0] = 0x1B;   // START STOP UNIT
	// cdb[4] bits: LoEj=0, Start=0 → stop the motor
	BYTE dummy[4] = {};
	return SendSCSI(cdb, 6, dummy, 0, false);
}

bool ScsiDrive::SetCdSpeedPioneer(int multiplier, BYTE speedModeValue,
	bool eepSave, int writeMultiplier) {
	// Remember the chosen mode so plain SetSpeed calls preserve it for the
	// rest of the session (or persist via EEPROM when eepSave is set).
	m_pioneerSpeedMode = speedModeValue & 0x3F;

	m_currentSpeed = multiplier <= 0 ? CD_SPEED_MAX : static_cast<WORD>(multiplier * CD_SPEED_1X);

	WORD writeSpeed = CD_SPEED_MAX;
	if (writeMultiplier > 0)
		writeSpeed = static_cast<WORD>(writeMultiplier * CD_SPEED_1X);
	else if (multiplier > 0)
		writeSpeed = m_currentSpeed;

	BYTE cdb[12] = {};
	cdb[0] = SCSI_SET_CD_SPEED;
	cdb[2] = (m_currentSpeed >> 8) & 0xFF;
	cdb[3] = m_currentSpeed & 0xFF;
	cdb[4] = (writeSpeed >> 8) & 0xFF;
	cdb[5] = writeSpeed & 0xFF;

	// Pioneer vendor byte 10: bit 7 always set, bit 6 = EEP save,
	// bits 0-5 = speed/session mode value (masked to 6 bits).
	cdb[10] = PIONEER_SPEED_EXT_FLAG | (speedModeValue & 0x3F);
	if (eepSave)
		cdb[10] |= PIONEER_SPEED_EEP_SAVE;

	BYTE sk = 0, asc = 0, ascq = 0;
	bool sent = SendSCSIWithSense(cdb, 12, nullptr, 0, &sk, &asc, &ascq, false);

	char dbg[192];
	snprintf(dbg, sizeof(dbg),
		"SetCdSpeedPioneer: %dx mode=0x%02X eep=%d -> sent=%d SK=0x%02X ASC=0x%02X ASCQ=0x%02X\n",
		multiplier, cdb[10], eepSave ? 1 : 0, sent ? 1 : 0, sk, asc, ascq);
	DebugSpeedMessage(dbg);

	return sent;
}
