// ============================================================================
// ScsiDrive.HideCDR.cpp - Plextor "Hide CDR Media" vendor feature
// ============================================================================
// Toggles the Plextor drive's CDR-detection masking via vendor mode page 0x31
// (the "PX-Plus / persistent settings" page also used by PlexTools and
// QPxTool to control SecuRec, SpeedRead, VariRec, GigaRec).
//
// When Hide CDR Media is ON the drive presents inserted CD-R / CD-RW media
// as pressed CD on disc-info queries.  This is useful for ripping CD-R
// backups of pressings whose source protection alters drive behaviour when
// CD-R media is detected (e.g. older Cactus Data Shield variants).
//
// Implementation does a strict read-modify-write so that other settings on
// the same vendor page (VariRec, GigaRec, etc.) are preserved.
// ============================================================================
#include "ScsiDrive.h"
#include <cstring>
#include <vector>

namespace {
constexpr BYTE  PLEXTOR_PAGE_CODE     = 0x31;
constexpr int   MODE_PARAM_HEADER_LEN = 8;     // MODE SENSE/SELECT 10 header
constexpr int   PAGE_BUFFER_LEN       = 32;    // Header + page (page <= 24)
constexpr int   HIDECDR_PAYLOAD_OFF   = 3;     // Byte offset within page payload
constexpr BYTE  HIDECDR_BIT           = 0x01;
}

bool ScsiDrive::SupportsHideCDRMedia() {
	// Hide CDR Media is a Plextor vendor feature.  The setter probes the
	// page anyway and returns false on rejection, so this is just a fast
	// pre-check for menu-eligibility purposes.
	return IsPlextor();
}

bool ScsiDrive::SetHideCDRMedia(bool enable) {
	if (!IsOpen() || !IsPlextor()) return false;

	std::vector<BYTE> buffer(PAGE_BUFFER_LEN, 0);

	// ── MODE SENSE 10 — read current vendor page 0x31 ──────────────────
	BYTE cdbSense[10] = {};
	cdbSense[0] = 0x5A;                              // MODE SENSE 10
	cdbSense[2] = PLEXTOR_PAGE_CODE;                 // PC=00, page code 0x31
	cdbSense[7] = 0;
	cdbSense[8] = static_cast<BYTE>(PAGE_BUFFER_LEN);

	BYTE sk = 0, asc = 0, ascq = 0;
	if (!SendSCSIWithSense(cdbSense, 10, buffer.data(), PAGE_BUFFER_LEN,
		&sk, &asc, &ascq, /*dataIn=*/true)) {
		return false;
	}

	// Mode Parameter Header (10): bytes 0-1 = mode data length,
	// 2 = medium type, 3 = device-specific, 4-5 reserved,
	// 6-7 = block descriptor length.  Skip header + any block descriptor
	// to locate the page itself.
	int blockDescLen =
		(static_cast<int>(buffer[6]) << 8) | static_cast<int>(buffer[7]);
	int pageOffset = MODE_PARAM_HEADER_LEN + blockDescLen;
	if (pageOffset + 2 > PAGE_BUFFER_LEN) return false;

	BYTE* page = buffer.data() + pageOffset;
	if ((page[0] & 0x3F) != PLEXTOR_PAGE_CODE) {
		// Drive does not expose the Plextor vendor page — feature unsupported.
		return false;
	}

	int pageLen   = page[1];                         // bytes after header
	int payloadAt = pageOffset + 2;
	if (payloadAt + HIDECDR_PAYLOAD_OFF >= PAGE_BUFFER_LEN ||
		HIDECDR_PAYLOAD_OFF >= pageLen) {
		return false;
	}

	// ── Modify only the Hide CDR bit ───────────────────────────────────
	BYTE& target = buffer[payloadAt + HIDECDR_PAYLOAD_OFF];
	if (enable) target |= HIDECDR_BIT;
	else        target &= static_cast<BYTE>(~HIDECDR_BIT);

	// Clear PS bit on page header (must be 0 for MODE SELECT).
	page[0] &= 0x7F;

	// Mode Parameter Header bytes 0-1 (mode data length) are reserved on
	// MODE SELECT — must be zeroed before writing back.
	buffer[0] = 0;
	buffer[1] = 0;

	// Total length to send = header + block desc + page header + page payload.
	int writeLen = pageOffset + 2 + pageLen;
	if (writeLen > PAGE_BUFFER_LEN) writeLen = PAGE_BUFFER_LEN;

	// ── MODE SELECT 10 — write modified page back ──────────────────────
	BYTE cdbSelect[10] = {};
	cdbSelect[0] = 0x55;                             // MODE SELECT 10
	cdbSelect[1] = 0x10;                             // PF=1, SP=0
	cdbSelect[7] = 0;
	cdbSelect[8] = static_cast<BYTE>(writeLen);

	return SendSCSIWithSense(cdbSelect, 10, buffer.data(), writeLen,
		&sk, &asc, &ascq, /*dataIn=*/false);
}
