// ============================================================================
// ScsiDrive.Read.cpp - SCSI sector reading and C2 handling
// ============================================================================
#include "ScsiDrive.h"
#include <climits>
#include <cstdio>
#include <vector>

namespace {
bool IsAllZeroAudio(const BYTE* buffer, DWORD bufferSize) {
	if (!buffer || bufferSize < AUDIO_SECTOR_SIZE) return false;
	for (DWORD i = 0; i < AUDIO_SECTOR_SIZE; i++) {
		if (buffer[i] != 0) return false;
	}
	return true;
}
}

// C2_POINTER_BYTES is defined in Constants.h (included via ScsiDrive.h).
// In ErrorPointers mode the drive returns 296 bytes, but bytes 294-295 are
// C1/C2 *block error statistics* from the hardware ECC decoder — NOT error
// pointers.  C1 block errors are routine even on perfect discs and must
// never be counted as C2 errors.

// ── ParseRawSubchannel ──────────────────────────────────────────────────
// De-interleaves the 96-byte raw P–W subchannel block into the 12-byte
// Q channel, validates CRC-16-CCITT, and extracts BCD-encoded track
// number and index from ADR=1 (position) frames.

bool ScsiDrive::ParseRawSubchannel(const BYTE* sub, int& qTrack, int& qIndex) {
	BYTE qchannel[12] = {};
	for (int i = 0; i < 96; i++) {
		int byteIdx = i / 8;
		int bitIdx = 7 - (i % 8);
		if (sub[i] & 0x40) {
			qchannel[byteIdx] |= (1 << bitIdx);
		}
	}

	// Validate CRC-16 (bytes 0-9 checked against bytes 10-11)
	uint16_t calcCrc = SubchannelCRC16(qchannel, 10);
	uint16_t storedCrc = (static_cast<uint16_t>(qchannel[10]) << 8) | qchannel[11];
	if (calcCrc != storedCrc) {
		return false;  // CRC mismatch — data is unreliable
	}

	// Only ADR=1 frames carry track/index position data
	BYTE adr = qchannel[0] & 0x0F;
	if (adr != 1) {
		return false;  // Not a position frame (MCN or ISRC)
	}

	// Q subchannel stores track/index in BCD
	qTrack = BcdToBin(qchannel[1]);
	qIndex = BcdToBin(qchannel[2]);
	return true;
}

// ── ReadCdAudio ───────────────────────────────────────────────────────────
// Issues SCSI READ CD (0xBE) for `count` CD-DA sectors at `lba`, requesting the
// caller's subchannel-selection byte (CDB[10]).  The expected sector type
// (CDB[1]) and main-channel byte (CDB[9]) start at the cached CD-DA form
// (0x04 / 0xF8).  On a hard rejection — while the accepted form is still
// unknown — a fallback ladder probes the alternates and caches whatever works
// for the rest of the session:
//   • CDB[9]: 0xF8 (sync+headers+user+EDC/ECC) → 0x10 (user-data-only). Strict
//     firmware (e.g. Hitachi-LG HL-DT-ST) rejects 0xF8 for a CD-DA-typed read.
//   • CDB[1]: 0x04 (CD-DA) → 0x00 (any). Modern Pioneer BD burners (e.g.
//     BDR-S13U) reject a CD-DA-typed READ CD and only accept expected-type 0.
// For an audio track the returned 2352 bytes are identical across these forms.

bool ScsiDrive::ReadCdAudio(DWORD lba, DWORD count, BYTE subSelect,
	BYTE* buffer, DWORD bufferSize) {
	BYTE cdb[12] = {};
	cdb[0] = SCSI_READ_CD;
	cdb[1] = m_cddaSectorType;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[6] = (count >> 16) & 0xFF;
	cdb[7] = (count >> 8) & 0xFF;
	cdb[8] = count & 0xFF;
	cdb[9] = m_cddaMainChannelFlags;
	cdb[10] = subSelect;

	// SendSCSIWithSense is equivalent to SendSCSI on the success path (GOOD or
	// sense key <= 0x01 both return true); it additionally surfaces the sense
	// bytes so a hard rejection can be reported and steered, not just guessed.
	BYTE sk = 0, asc = 0, ascq = 0;
	if (SendSCSIWithSense(cdb, 12, buffer, bufferSize, &sk, &asc, &ascq)) {
		// GOOD status confirms this CDB form (sector type + main channel) is
		// accepted — enough for the subchannel/pregap/C2 paths to adopt it.
		m_cddaFormDiscovered = true;
		if (m_cddaReadFormProbed || m_cddaMainChannelFlags != 0xF8 ||
			!IsAllZeroAudio(buffer, bufferSize)) {
			m_cddaReadFormProbed = true;  // The current form works on this drive
			return true;
		}

		// Some HL-DT-ST firmware reports GOOD for 0xF8 but returns an untouched
		// zeroed data buffer. Probe the canonical CD-DA form before trusting it.
		cdb[9] = 0x10;
		std::vector<BYTE> alt(bufferSize, 0);
		if (SendSCSIWithSense(cdb, 12, alt.data(), bufferSize, &sk, &asc, &ascq)) {
			memcpy(buffer, alt.data(), bufferSize);
			if (!IsAllZeroAudio(buffer, bufferSize)) {
				m_cddaMainChannelFlags = 0x10;
				m_cddaReadFormProbed = true;
				OutputDebugStringA("ReadCdAudio: 0xF8 returned zero audio; "
					"switched to 0x10 (user-data-only) for this session.\n");
			}
			return true;
		}
		return true;
	}

	// Hard failure (CHECK CONDITION with sense key >= 0x02). Record the reason
	// so the workflow can report it instead of guessing.
	m_lastReadSenseKey = sk; m_lastReadASC = asc; m_lastReadASCQ = ascq;

	// Walk a fallback ladder while the accepted CD-DA read form is still
	// unknown. Two CDB dimensions vary:
	//   • CDB[9] main-channel selection: 0xF8 (sync+headers+user+EDC/ECC) vs.
	//     0x10 (user-data-only, the canonical CD-DA form). HL-DT-ST firmware
	//     rejects 0xF8 for a CD-DA-typed read.
	//   • CDB[1] expected sector type: 0x04 (CD-DA) vs. 0x00 (any). Modern
	//     Pioneer BD burners (e.g. BDR-S13U) return CHECK CONDITION for a
	//     CD-DA-typed READ CD and only accept expected-sector-type 0.
	// Once a form is locked in (m_cddaReadFormProbed) a later failure is a
	// genuine medium error, so the ladder only runs until then. Cache the
	// first combination that works for the rest of the session.
	if (!m_cddaReadFormProbed) {
		struct ReadForm { BYTE sectorType; BYTE mainChannel; };
		const ReadForm ladder[] = {
			{ 0x04, 0x10 },   // HL-DT-ST: CD-DA type, user-data-only
			{ 0x00, 0xF8 },   // Pioneer: any type, full selection
			{ 0x00, 0x10 },   // Pioneer: any type, user-data-only
		};
		for (const ReadForm& f : ladder) {
			if (f.sectorType == m_cddaSectorType && f.mainChannel == m_cddaMainChannelFlags) {
				continue;  // identical to the primary attempt already made
			}
			cdb[1] = f.sectorType;
			cdb[9] = f.mainChannel;
			if (SendSCSIWithSense(cdb, 12, buffer, bufferSize, &sk, &asc, &ascq)) {
				m_cddaSectorType = f.sectorType;
				m_cddaMainChannelFlags = f.mainChannel;
				m_cddaReadFormProbed = true;
				m_cddaFormDiscovered = true;
				char dbg[160];
				snprintf(dbg, sizeof(dbg),
					"ReadCdAudio: drive rejected CD-DA read form; switched to "
					"sectorType=0x%02X mainChannel=0x%02X for this session.\n",
					f.sectorType, f.mainChannel);
				OutputDebugStringA(dbg);
				return true;
			}
			m_lastReadSenseKey = sk; m_lastReadASC = asc; m_lastReadASCQ = ascq;
		}
	}
	return false;
}

// ── EnsureCddaReadForm ────────────────────────────────────────────────────
// Lazily discovers the READ CD form (expected sector type CDB[1] + main-channel
// selection CDB[9]) the drive accepts for CD-DA, via a throwaway 1-sector read
// at `lba`, and caches it in m_cddaSectorType / m_cddaMainChannelFlags. This
// lets the subchannel, pregap, and C2 read paths use the same form ReadCdAudio
// would (e.g. expected sector type 0x00 on Pioneer BD burners that reject the
// CD-DA-typed 0x04 form) instead of hard-coding 0x04/0xF8.
//
// Gated by m_cddaFormDiscovered (set on the first GOOD read here OR in
// ReadCdAudio), so it runs at most until one form succeeds — a no-op after.
// Unlike ReadCdAudio it does not resolve the HL-DT-ST GOOD-but-zero ambiguity;
// any GOOD status means the CDB form is accepted, which is all these callers
// need. Failure leaves the defaults unchanged for the next caller to retry.
void ScsiDrive::EnsureCddaReadForm(DWORD lba) {
	if (m_cddaFormDiscovered) return;

	struct ReadForm { BYTE sectorType; BYTE mainChannel; };
	const ReadForm forms[] = {
		{ m_cddaSectorType, m_cddaMainChannelFlags },  // current best guess first
		{ 0x04, 0xF8 }, { 0x04, 0x10 },                // CD-DA type (standard drives)
		{ 0x00, 0xF8 }, { 0x00, 0x10 },                // any type (Pioneer BD burners)
	};

	BYTE tmp[AUDIO_SECTOR_SIZE];
	BYTE sk = 0, asc = 0, ascq = 0;
	for (const ReadForm& f : forms) {
		BYTE cdb[12] = {};
		cdb[0] = SCSI_READ_CD;
		cdb[1] = f.sectorType;
		cdb[2] = (lba >> 24) & 0xFF;
		cdb[3] = (lba >> 16) & 0xFF;
		cdb[4] = (lba >> 8) & 0xFF;
		cdb[5] = lba & 0xFF;
		cdb[8] = 1;
		cdb[9] = f.mainChannel;
		cdb[10] = 0x00;  // main channel only — no subchannel needed to probe form
		if (SendSCSIWithSense(cdb, 12, tmp, AUDIO_SECTOR_SIZE, &sk, &asc, &ascq)) {
			m_cddaSectorType = f.sectorType;
			m_cddaMainChannelFlags = f.mainChannel;
			m_cddaFormDiscovered = true;
			return;
		}
		m_lastReadSenseKey = sk; m_lastReadASC = asc; m_lastReadASCQ = ascq;
	}
}

// ── ReadSector ──────────────────────────────────────────────────────────
// Reads one CD-DA sector plus its raw P–W subchannel (CDB[10] = 0x01) and
// copies 2352 bytes of audio and 96 bytes of subchannel into the caller's
// buffers.

bool ScsiDrive::ReadSector(DWORD lba, BYTE* audio, BYTE* subchannel) {
	std::vector<BYTE> buffer(RAW_SECTOR_SIZE);

	if (!ReadCdAudio(lba, 1, 0x01, buffer.data(), RAW_SECTOR_SIZE)) return false;

	memcpy(audio, buffer.data(), AUDIO_SECTOR_SIZE);
	memcpy(subchannel, buffer.data() + AUDIO_SECTOR_SIZE, SUBCHANNEL_SIZE);
	return true;
}

// ── ReadSectorWithC2 ────────────────────────────────────────────────────
// Thin wrapper that delegates to ReadSectorWithC2Ex so the current
// m_c2Mode (ErrorPointers vs. ErrorFlags vs. Plextor D8) is respected
// everywhere, fixing several scan functions that previously hard-coded
// the C2 layout.

// FIX A: Delegate to ReadSectorWithC2Ex so m_c2Mode is respected everywhere.
// This fixes RunDiscRotScan, RunSpeedComparisonTest, CheckLeadAreas,
// GenerateSurfaceMap, and ScanDiscForC2Errors dual-speed validation.
bool ScsiDrive::ReadSectorWithC2(DWORD lba, BYTE* audio, BYTE* subchannel, int& c2Errors) {
	C2ReadOptions opts;
	return ReadSectorWithC2Ex(lba, audio, subchannel, c2Errors, nullptr, opts);
}

bool ScsiDrive::ReadSectorWithC2Ex(DWORD lba, BYTE* audio, BYTE* subchannel,
	int& c2Errors, BYTE* c2Raw, const C2ReadOptions& options,
	BYTE* outSenseKey, BYTE* outASC, BYTE* outASCQ,
	int* outC1BlockErrors, int* outC2BlockErrors) {

	c2Errors = 0;
	if (c2Raw) memset(c2Raw, 0, C2_ERROR_SIZE);
	if (outC1BlockErrors) *outC1BlockErrors = 0;
	if (outC2BlockErrors) *outC2BlockErrors = 0;

	if (m_c2Mode == C2Mode::NotSupported || !m_c2Functional) {
		if (outSenseKey) *outSenseKey = 0;
		if (outASC) *outASC = 0;
		if (outASCQ) *outASCQ = 0;
		return false;
	}

	// Adopt the drive's accepted CD-DA READ CD form (sector type + main
	// channel) before issuing the C2 read, so C2-based scans work on drives
	// that reject the CD-DA-typed 0x04 form (e.g. Pioneer BD burners).
	// Skipped for Plextor D8 mode, which uses a different opcode entirely.
	if (m_c2Mode != C2Mode::PlextorD8) {
		EnsureCddaReadForm(lba);
	}

	if (options.multiPass && options.passCount > 1) {
		return ReadSectorWithC2ExMultiPass(lba, audio, subchannel, c2Errors, c2Raw, options,
			outSenseKey, outASC, outASCQ);
	}

	if (m_c2Mode == C2Mode::PlextorD8) {
		return PlextorReadC2(lba, audio, c2Errors, c2Raw, options.countBytes,
			outSenseKey, outASC, outASCQ,
			outC1BlockErrors, outC2BlockErrors);
	}

	BYTE cdb[12] = {};
	int bufferSize = subchannel ? FULL_SECTOR_WITH_C2 : SECTOR_WITH_C2_SIZE;
	std::vector<BYTE> buffer(bufferSize);

	cdb[0] = SCSI_READ_CD;
	cdb[1] = m_cddaSectorType;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;

	bool useErrorBlock = false;
	BYTE senseKey = 0, asc = 0, ascq = 0;

	if (m_c2Mode == C2Mode::ErrorBlock) {
		BYTE c2Bits = 0x02;
		cdb[9] = m_cddaMainChannelFlags | c2Bits;
		cdb[10] = subchannel ? 0x01 : 0x00;
		useErrorBlock = true;
		bool ok = SendSCSIWithSense(cdb, 12, buffer.data(), bufferSize, &senseKey, &asc, &ascq);
		if (ok && !m_cddaReadFormProbed && m_cddaMainChannelFlags == 0xF8 &&
			IsAllZeroAudio(buffer.data(), bufferSize)) {
			cdb[9] = 0x10 | c2Bits;
			std::vector<BYTE> alt(bufferSize, 0);
			BYTE altSk = 0, altAsc = 0, altAscq = 0;
			if (SendSCSIWithSense(cdb, 12, alt.data(), bufferSize, &altSk, &altAsc, &altAscq)) {
				buffer.swap(alt);
				senseKey = altSk; asc = altAsc; ascq = altAscq;
				if (!IsAllZeroAudio(buffer.data(), bufferSize)) {
					m_cddaMainChannelFlags = 0x10;
					m_cddaReadFormProbed = true;
					OutputDebugStringA("ReadSectorWithC2Ex: 0xF8+C2 returned zero audio; "
						"switched to 0x10+C2 for this session.\n");
				}
			}
		}
		if (!ok && senseKey != 0x01) {
			if (!m_cddaReadFormProbed && m_cddaMainChannelFlags == 0xF8) {
				cdb[9] = 0x10 | c2Bits;
				ok = SendSCSIWithSense(cdb, 12, buffer.data(), bufferSize, &senseKey, &asc, &ascq);
				if (ok || senseKey == 0x01) {
					m_cddaMainChannelFlags = 0x10;
					m_cddaReadFormProbed = true;
					OutputDebugStringA("ReadSectorWithC2Ex: drive rejected 0xF8+C2; "
						"switched to 0x10+C2 for this session.\n");
				}
			}
			if (!ok && senseKey != 0x01) {
				if (outSenseKey) *outSenseKey = senseKey;
				if (outASC) *outASC = asc;
				if (outASCQ) *outASCQ = ascq;
				return false;
			}
		}
		else if (ok && !IsAllZeroAudio(buffer.data(), bufferSize)) {
			m_cddaReadFormProbed = true;
		}
	}
	else {
		BYTE c2Bits = 0x04;
		cdb[9] = m_cddaMainChannelFlags | c2Bits;
		cdb[10] = subchannel ? 0x01 : 0x00;

		bool ok = SendSCSIWithSense(cdb, 12, buffer.data(), bufferSize, &senseKey, &asc, &ascq);
		if (ok && !m_cddaReadFormProbed && m_cddaMainChannelFlags == 0xF8 &&
			IsAllZeroAudio(buffer.data(), bufferSize)) {
			cdb[9] = 0x10 | c2Bits;
			std::vector<BYTE> alt(bufferSize, 0);
			BYTE altSk = 0, altAsc = 0, altAscq = 0;
			if (SendSCSIWithSense(cdb, 12, alt.data(), bufferSize, &altSk, &altAsc, &altAscq)) {
				buffer.swap(alt);
				senseKey = altSk; asc = altAsc; ascq = altAscq;
				if (!IsAllZeroAudio(buffer.data(), bufferSize)) {
					m_cddaMainChannelFlags = 0x10;
					m_cddaReadFormProbed = true;
					OutputDebugStringA("ReadSectorWithC2Ex: 0xF8+C2 returned zero audio; "
						"switched to 0x10+C2 for this session.\n");
				}
			}
		}
		if (!ok && senseKey != 0x01) {
			if (!m_cddaReadFormProbed && m_cddaMainChannelFlags == 0xF8) {
				cdb[9] = 0x10 | c2Bits;
				ok = SendSCSIWithSense(cdb, 12, buffer.data(), bufferSize, &senseKey, &asc, &ascq);
				if (ok || senseKey == 0x01) {
					m_cddaMainChannelFlags = 0x10;
					m_cddaReadFormProbed = true;
					OutputDebugStringA("ReadSectorWithC2Ex: drive rejected 0xF8+C2; "
						"switched to 0x10+C2 for this session.\n");
				}
			}
		}
		if (!ok && senseKey != 0x01) {
			// First mode failed — try fallback
			c2Bits = 0x02;
			cdb[9] = m_cddaMainChannelFlags | c2Bits;
			cdb[10] = subchannel ? 0x01 : 0x00;

			ok = SendSCSIWithSense(cdb, 12, buffer.data(), bufferSize, &senseKey, &asc, &ascq);
			if (!ok && senseKey != 0x01 && !m_cddaReadFormProbed && m_cddaMainChannelFlags == 0xF8) {
				cdb[9] = 0x10 | c2Bits;
				ok = SendSCSIWithSense(cdb, 12, buffer.data(), bufferSize, &senseKey, &asc, &ascq);
				if (ok || senseKey == 0x01) {
					m_cddaMainChannelFlags = 0x10;
					m_cddaReadFormProbed = true;
					OutputDebugStringA("ReadSectorWithC2Ex: drive rejected 0xF8+C2 fallback; "
						"switched to 0x10+C2 for this session.\n");
				}
			}
			if (!ok && senseKey != 0x01) {
				if (outSenseKey) *outSenseKey = senseKey;
				if (outASC) *outASC = asc;
				if (outASCQ) *outASCQ = ascq;
				return false;
			}
			useErrorBlock = true;
		}
		else if (ok && !IsAllZeroAudio(buffer.data(), bufferSize)) {
			m_cddaReadFormProbed = true;
		}
	}

	memcpy(audio, buffer.data(), AUDIO_SECTOR_SIZE);

	const BYTE* c2Data = buffer.data() + AUDIO_SECTOR_SIZE;

	// Only count the 294 actual C2 error pointer bytes.  Bytes 294-295
	// in ErrorPointers mode are C1/C2 block error statistics — C1 counts are
	// routinely non-zero on perfect discs and were causing false positives.
	// In countBytes (PlexTools-style) mode, 0xFF means "no error sample
	// pointer" and must also be excluded.
	for (int i = 0; i < C2_POINTER_BYTES; i++) {
		if (options.countBytes) {
			if (c2Data[i] != 0 && c2Data[i] != 0xFF) c2Errors++;
		}
		else {
			BYTE b = c2Data[i];
			while (b) { c2Errors += b & 1; b >>= 1; }
		}
	}

	if (outSenseKey) *outSenseKey = senseKey;
	if (outASC) *outASC = asc;
	if (outASCQ) *outASCQ = ascq;

	if (c2Raw) {
		memset(c2Raw, 0, C2_ERROR_SIZE);
		memcpy(c2Raw, c2Data, C2_POINTER_BYTES);
	}

	// Extract C1/C2 block error statistics from bytes 294-295.
	// Only valid in ErrorPointers mode — ErrorBlock has a different layout.
	if (!useErrorBlock) {
		if (outC1BlockErrors) *outC1BlockErrors = static_cast<int>(c2Data[294]);
		if (outC2BlockErrors) *outC2BlockErrors = static_cast<int>(c2Data[295]);

		// Cross-check: the drive's CIRC decoder reports its own C2 block error
		// count in byte 295.  If it says zero but the pointer bitmap produced a
		// non-zero count, the bitmap contains drive-specific padding/status bytes
		// rather than real error pointers.  Trust the hardware counter.
		// Only apply when the drive is known to populate block error fields;
		// many drives (especially MediaTek-based) leave bytes 294-295 as zero
		// regardless of actual errors, which would suppress all real C2 counts.
		if (m_c1BlockErrorsAvailable &&
			c2Errors > 0 && c2Data[295] == 0 && senseKey == 0x00) {
			c2Errors = 0;
			if (c2Raw) {
				memset(c2Raw, 0, C2_POINTER_BYTES);
			}
		}
	}
	else {
		if (outC1BlockErrors) *outC1BlockErrors = 0;
		if (outC2BlockErrors) *outC2BlockErrors = 0;
	}

	if (subchannel) {
		memcpy(subchannel, buffer.data() + AUDIO_SECTOR_SIZE + C2_ERROR_SIZE, SUBCHANNEL_SIZE);
	}

	return true;
}

// FIX B: Cache defeat between multi-pass reads
bool ScsiDrive::ReadSectorWithC2ExMultiPass(DWORD lba, BYTE* audio, BYTE* subchannel,
	int& c2Errors, BYTE* c2Raw, const C2ReadOptions& options,
	BYTE* outSenseKey, BYTE* outASC, BYTE* outASCQ) {

	std::vector<BYTE> bestAudio(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> aggregatedC2(C2_ERROR_SIZE, 0);
	int minPassErrors = INT_MAX;
	BYTE worstSenseKey = 0x00;
	BYTE worstASC = 0x00;
	BYTE worstASCQ = 0x00;

	for (int pass = 0; pass < options.passCount; pass++) {
		// Cache defeat: seek to a distant sector between passes
		if (options.defeatCache && pass > 0) {
			BYTE dummy[AUDIO_SECTOR_SIZE];
			DWORD cacheBustLBA = (lba > 10000) ? lba - 10000 : lba + 10000;
			ReadSectorAudioOnly(cacheBustLBA, dummy);
		}

		BYTE passAudio[AUDIO_SECTOR_SIZE];
		BYTE passC2[C2_ERROR_SIZE];
		int passErrors = 0;
		BYTE passSenseKey = 0, passASC = 0, passASCQ = 0;

		C2ReadOptions singleOpts;
		singleOpts.multiPass = false;
		singleOpts.passCount = 1;
		singleOpts.defeatCache = false;
		singleOpts.countBytes = options.countBytes;

		if (!ReadSectorWithC2Ex(lba, passAudio, subchannel, passErrors, passC2, singleOpts,
			&passSenseKey, &passASC, &passASCQ)) {
			if (outSenseKey) *outSenseKey = passSenseKey;
			if (outASC) *outASC = passASC;
			if (outASCQ) *outASCQ = passASCQ;
			return false;
		}

		// Keep the worst sense key across passes (0x03 > 0x01 > 0x00)
		if (passSenseKey > worstSenseKey) {
			worstSenseKey = passSenseKey;
			worstASC = passASC;
			worstASCQ = passASCQ;
		}

		// Only merge the actual C2 pointer bytes, not the block error stats
		for (int i = 0; i < C2_POINTER_BYTES; i++) {
			aggregatedC2[i] |= passC2[i];
		}

		if (passErrors < minPassErrors) {
			memcpy(bestAudio.data(), passAudio, AUDIO_SECTOR_SIZE);
			minPassErrors = passErrors;
		}
	}

	// Recount over 294 pointer bytes only — excludes block error stats.
	// In countBytes (PlexTools-style) mode, 0xFF means "no error sample
	// pointer" — only non-0xFF, non-zero bytes indicate actual errors.
	c2Errors = 0;
	for (int i = 0; i < C2_POINTER_BYTES; i++) {
		if (options.countBytes) {
			if (aggregatedC2[i] != 0 && aggregatedC2[i] != 0xFF) c2Errors++;
		}
		else {
			BYTE b = aggregatedC2[i];
			while (b) { c2Errors += b & 1; b >>= 1; }
		}
	}

	if (outSenseKey) *outSenseKey = worstSenseKey;
	if (outASC) *outASC = worstASC;
	if (outASCQ) *outASCQ = worstASCQ;

	memcpy(audio, bestAudio.data(), AUDIO_SECTOR_SIZE);
	if (c2Raw) memcpy(c2Raw, aggregatedC2.data(), C2_POINTER_BYTES);

	return true;
}

bool ScsiDrive::PlextorReadC2(DWORD lba, BYTE* audio, int& c2Errors, BYTE* c2Raw, bool countBytes,
	BYTE* outSenseKey, BYTE* outASC, BYTE* outASCQ,
	int* outC1BlockErrors, int* outC2BlockErrors) {
	BYTE cdb[12] = {};
	std::vector<BYTE> buffer(SECTOR_WITH_C2_SIZE);

	cdb[0] = 0xD8;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0x02;

	BYTE senseKey = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, buffer.data(), SECTOR_WITH_C2_SIZE,
		&senseKey, &asc, &ascq);

	if (outSenseKey) *outSenseKey = senseKey;
	if (outASC) *outASC = asc;
	if (outASCQ) *outASCQ = ascq;

	// Accept No Sense (0x00) and Recovered Error (0x01) — many Plextor
	// drives return CHECK CONDITION with sense key 0x00 on vendor D8 reads.
	// The data buffer is valid in both cases.
	if (!ok && senseKey > 0x01) {
		return false;
	}

	memcpy(audio, buffer.data(), AUDIO_SECTOR_SIZE);

	// Count only 294 pointer bytes — Plextor D8 also returns block
	// error stats in the trailing bytes that must not inflate the count.
	// In countBytes (PlexTools-style) mode, 0xFF means "no error sample
	// pointer" and must be excluded — consistent with ReadSectorWithC2Ex
	// and ReadSectorWithC2ExMultiPass.
	c2Errors = 0;
	const BYTE* c2Data = buffer.data() + AUDIO_SECTOR_SIZE;
	for (int i = 0; i < C2_POINTER_BYTES; i++) {
		if (countBytes) {
			if (c2Data[i] != 0 && c2Data[i] != 0xFF) c2Errors++;
		}
		else {
			BYTE b = c2Data[i];
			while (b) { c2Errors += b & 1; b >>= 1; }
		}
	}

	if (c2Raw) {
		memcpy(c2Raw, c2Data, C2_POINTER_BYTES);
	}

	// Plextor D8 returns C1/C2 block error statistics in bytes 294-295
	// of the 296-byte C2 region.  These are hardware ECC decoder counts
	// per sector — the true BLER values that PlexTools reports.
	if (outC1BlockErrors) *outC1BlockErrors = static_cast<int>(c2Data[294]);
	if (outC2BlockErrors) *outC2BlockErrors = static_cast<int>(c2Data[295]);

	return true;
}

// FIX C & D: Restore missing functions (unresolved externals)

bool ScsiDrive::ValidateC2Accuracy(DWORD testLBA) {
	constexpr int NUM_READS = 4;
	constexpr int SPEEDS[] = { 4, 8, 16, 0 }; // 0 = max
	constexpr DWORD CACHE_DEFEAT_DISTANCE = 5000;

	std::vector<BYTE> audio(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> c2First(C2_ERROR_SIZE);
	std::vector<BYTE> c2Current(C2_ERROR_SIZE);

	C2ReadOptions opts;
	opts.countBytes = false;

	// PHASE 1: Verify this is a clean sector (no C2 errors at baseline speed)
	SetSpeed(8); // Use medium speed for initial scan
	int preTestErrors = 0;
	if (!ReadSectorWithC2Ex(testLBA, audio.data(), nullptr, preTestErrors, nullptr, opts)) {
		SetSpeed(0);
		return false; // Read failure
	}

	// If sector has C2 errors, we can't validate C2 accuracy here
	// (errors might be real, so variation is expected)
	if (preTestErrors > 0) {
		SetSpeed(0);
		return true; // PASS - can't disprove C2 accuracy on error-containing sectors
	}

	// PHASE 2: Now verify the sector stays clean at all speeds
	for (int i = 0; i < NUM_READS; i++) {
		SetSpeed(SPEEDS[i]);

		// Cache defeat between reads
		DWORD farLBA = (testLBA > CACHE_DEFEAT_DISTANCE)
			? testLBA - CACHE_DEFEAT_DISTANCE
			: testLBA + CACHE_DEFEAT_DISTANCE;
		ReadSectorAudioOnly(farLBA, audio.data());
		Sleep(10);

		int c2Errors = 0;
		if (!ReadSectorWithC2Ex(testLBA, audio.data(), nullptr, c2Errors, nullptr, opts)) {
			SetSpeed(0);
			return false; // Read failure
		}

		// If a previously-clean sector now shows C2 errors, the reporting is unreliable
		if (c2Errors > 0) {
			SetSpeed(0);
			return false; // FAIL - phantom errors appeared
		}
	}

	SetSpeed(0);
	return true; // PASS - sector stayed clean at all speeds
}

bool ScsiDrive::ReadSectorAudioOnly(DWORD lba, BYTE* audio) {
	return ReadCdAudio(lba, 1, 0x00, audio, AUDIO_SECTOR_SIZE);
}

bool ScsiDrive::ReadDataSector(DWORD lba, BYTE* data) {
	BYTE cdb[12] = {};
	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x00;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0xF8;
	cdb[10] = 0x00;
	return SendSCSI(cdb, 12, data, AUDIO_SECTOR_SIZE);
}

bool ScsiDrive::ReadSectorQRaw(DWORD lba, int& qTrack, int& qIndex) {
	// Adopt the READ CD form the drive accepts for CD-DA (sector type +
	// main channel) so subchannel reads work on drives that reject the
	// CD-DA-typed 0x04 form (e.g. Pioneer BD burners).
	EnsureCddaReadForm(lba);

	BYTE cdb[12] = {};
	BYTE buffer[RAW_SECTOR_SIZE];

	cdb[0] = SCSI_READ_CD;
	cdb[1] = m_cddaSectorType;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = m_cddaMainChannelFlags;
	cdb[10] = 0x01;  // Raw subchannel

	// Retry once on CRC failure — transient subchannel errors are common
	for (int attempt = 0; attempt < 2; attempt++) {
		// In ReadSectorQRaw — subchannel reads should complete in <5 seconds
		if (!SendSCSI(cdb, 12, buffer, RAW_SECTOR_SIZE, true, 5)) return false;
		if (ParseRawSubchannel(buffer + AUDIO_SECTOR_SIZE, qTrack, qIndex)) return true;
	}
	return false;
}

// Reads the Q-subchannel CONTROL nibble from a raw P-W subchannel read at `lba`.
// Mirrors ReadSectorQRaw's read + de-interleave + CRC check, but keeps the high
// nibble of Q byte 0 (the control field) instead of the ADR/position. Returns
// false if the read fails, the CRC is bad on both attempts, or the frame is not
// an ADR=1 position frame (control only accompanies position frames).
bool ScsiDrive::ReadSectorQControl(DWORD lba, int& control) {
	control = 0;
	BYTE buffer[RAW_SECTOR_SIZE];

	BYTE cdb[12] = {};
	cdb[0] = SCSI_READ_CD;
	cdb[1] = m_cddaSectorType;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = m_cddaMainChannelFlags;
	cdb[10] = 0x01;  // Raw subchannel

	// Retry once on CRC failure — transient subchannel errors are common.
	for (int attempt = 0; attempt < 2; attempt++) {
		if (!SendSCSI(cdb, 12, buffer, RAW_SECTOR_SIZE, true, 5)) return false;

		// De-interleave the 96-byte raw P-W block into the 12-byte Q channel
		// (same bit extraction ParseRawSubchannel uses).
		const BYTE* sub = buffer + AUDIO_SECTOR_SIZE;
		BYTE qchannel[12] = {};
		for (int i = 0; i < 96; i++) {
			int byteIdx = i / 8;
			int bitIdx = 7 - (i % 8);
			if (sub[i] & 0x40) qchannel[byteIdx] |= (1 << bitIdx);
		}

		uint16_t calcCrc = SubchannelCRC16(qchannel, 10);
		uint16_t storedCrc = (static_cast<uint16_t>(qchannel[10]) << 8) | qchannel[11];
		if (calcCrc != storedCrc) continue;   // unreliable — retry once

		if ((qchannel[0] & 0x0F) != 1) return false;  // not a position frame
		control = (qchannel[0] >> 4) & 0x0F;           // high nibble = CONTROL
		return true;
	}
	return false;
}

// Single-read Q subchannel helper (raw with formatted fallback, no voting)
bool ScsiDrive::ReadSectorQSingle(DWORD lba, int& qTrack, int& qIndex) {
	if (ReadSectorQRaw(lba, qTrack, qIndex)) {
		return true;
	}

	// Fallback: formatted Q subchannel. The accepted CD-DA form was discovered
	// by the ReadSectorQRaw call above, so honor it here too.
	BYTE cdb[12] = {};
	BYTE buffer[AUDIO_SECTOR_SIZE + 16];

	cdb[0] = SCSI_READ_CD;
	cdb[1] = m_cddaSectorType;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = m_cddaMainChannelFlags;
	cdb[10] = 0x02;  // Formatted Q subchannel

	if (!SendSCSI(cdb, 12, buffer, AUDIO_SECTOR_SIZE + 16, true, 5)) {
		// Both raw and formatted failed — seek away to reset drive
		// head position, then retry raw once more
		DWORD resetLBA = (lba > 150) ? lba - 150 : lba + 150;
		BYTE seekCdb[12] = {};
		seekCdb[0] = SCSI_READ_CD;
		seekCdb[1] = m_cddaSectorType;
		seekCdb[2] = (resetLBA >> 24) & 0xFF;
		seekCdb[3] = (resetLBA >> 16) & 0xFF;
		seekCdb[4] = (resetLBA >> 8) & 0xFF;
		seekCdb[5] = resetLBA & 0xFF;
		seekCdb[8] = 1;
		seekCdb[9] = m_cddaMainChannelFlags;
		seekCdb[10] = 0x01;
		BYTE dummy[RAW_SECTOR_SIZE];
		SendSCSI(seekCdb, 12, dummy, RAW_SECTOR_SIZE, true, 5);

		// Retry the original sector
		return ReadSectorQRaw(lba, qTrack, qIndex);
	}

	const BYTE* qData = buffer + AUDIO_SECTOR_SIZE;

	// Validate ADR mode — only mode 1 carries position data
	BYTE adr = qData[0] & 0x0F;
	if (adr != 1) return false;

	// Validate BCD ranges before conversion
	if ((qData[1] & 0xF0) > 0x90 || (qData[1] & 0x0F) > 0x09) return false;
	if ((qData[2] & 0xF0) > 0x90 || (qData[2] & 0x0F) > 0x09) return false;

	qTrack = BcdToBin(qData[1]);
	qIndex = BcdToBin(qData[2]);
	return true;
}

// Majority-voting Q subchannel read — performs 3 single reads and returns
// the (track, index) pair that at least 2 of 3 reads agree on.  This defeats
// stale subchannel data that many drives return at index transition boundaries.
bool ScsiDrive::ReadSectorQ(DWORD lba, int& qTrack, int& qIndex) {
	constexpr int ROUNDS = 3;
	constexpr int MAJORITY = 2;

	struct QResult { int track; int index; };
	QResult results[ROUNDS];
	int validCount = 0;

	for (int round = 0; round < ROUNDS; round++) {
		int t = 0, idx = -1;
		if (ReadSectorQSingle(lba, t, idx)) {
			results[validCount++] = { t, idx };
		}
	}

	if (validCount == 0) return false;

	// Find a (track, index) pair that appears >= MAJORITY times
	for (int i = 0; i < validCount; i++) {
		int count = 0;
		for (int j = 0; j < validCount; j++) {
			if (results[j].track == results[i].track &&
				results[j].index == results[i].index) {
				count++;
			}
		}
		if (count >= MAJORITY) {
			qTrack = results[i].track;
			qIndex = results[i].index;
			return true;
		}
	}

	// No majority — return first valid result as best-effort
	qTrack = results[0].track;
	qIndex = results[0].index;
	return true;
}

// Adaptive Q subchannel read — uses a single read for sectors deep within a
// track and falls back to majority voting only near index transition points
// (pregap/start boundaries) where drives commonly return stale data.
bool ScsiDrive::ReadSectorQAdaptive(DWORD lba, int& qTrack, int& qIndex,
	DWORD pregapLBA, DWORD startLBA) {
	constexpr DWORD TRANSITION_MARGIN = 75;  // +/- 1 second (75 sectors) around boundaries

	bool nearTransition = false;
	if (lba >= pregapLBA && lba < pregapLBA + TRANSITION_MARGIN) nearTransition = true;
	if (lba >= startLBA && lba < startLBA + TRANSITION_MARGIN)  nearTransition = true;
	if (pregapLBA > TRANSITION_MARGIN && lba >= pregapLBA - TRANSITION_MARGIN && lba < pregapLBA)
		nearTransition = true;

	if (nearTransition) {
		return ReadSectorQ(lba, qTrack, qIndex);  // Full 3-read majority voting
	}

	// Single read for mid-track sectors; fall back to voting on failure
	if (ReadSectorQSingle(lba, qTrack, qIndex)) {
		return true;
	}
	return ReadSectorQ(lba, qTrack, qIndex);
}

// A C2 error is likely recoverable if:
// 1. Re-read at a different speed produces c2Errors == 0 for the same LBA
// 2. Multiple reads return identical audio bytes (matching hash)
// 3. SCSI sense key == 0x01 (Recovered Error)
//
// A C2 error is likely unrecoverable if:
// 1. Every re-read still shows C2 errors
// 2. Audio data differs across reads (hash mismatch)
// 3. SCSI sense key == 0x03 (Medium Error)

// Read multiple consecutive sectors at once
bool ScsiDrive::ReadSectorsAudioOnly(DWORD startLBA, DWORD count, BYTE* audio) {
	if (count == 0 || count > 32) return false;
	return ReadCdAudio(startLBA, count, 0x00, audio, AUDIO_SECTOR_SIZE * count);
}

bool ScsiDrive::SeekToLBA(DWORD lba) {
	BYTE cdb[10] = {};
	cdb[0] = 0x2B; // SEEK(10)
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;

	// Capture sense rather than collapsing every failure to a bare false. A
	// seek can fail because the tray is empty (ASC 0x3A), because the disc is
	// still spinning up (ASC 0x04) or because the drive rejected the address —
	// callers report "could not seek", which named none of them. The Pioneer
	// scan path in particular relies on this seek as its ONLY media check,
	// since the vendor scan CDBs return GOOD with no disc loaded.
	m_lastSeekSenseKey = 0;
	m_lastSeekASC = 0;
	m_lastSeekASCQ = 0;
	return SendSCSIWithSense(cdb, 10, nullptr, 0,
		&m_lastSeekSenseKey, &m_lastSeekASC, &m_lastSeekASCQ, false);
}

// Q subchannel read with Expected Sector Type = 0 (any).  Requests only the
// 16-byte formatted Q subchannel — no user data — so it works on both audio
// and data sectors.  Used by the TOC-less scanner to see through data tracks
// that reject CD-DA-typed reads.
bool ScsiDrive::ReadSectorQAnyType(DWORD lba, int& qTrack, int& qIndex) {
	BYTE cdb[12] = {};
	BYTE buffer[16] = {};

	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x00;                  // Expected Sector Type = any
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0x00;                  // No user data, no header, no EDC
	cdb[10] = 0x02;                 // Formatted Q subchannel only

	if (!SendSCSI(cdb, 12, buffer, 16)) return false;

	BYTE adr = buffer[0] & 0x0F;
	if (adr != 1) return false;

	if ((buffer[1] & 0xF0) > 0x90 || (buffer[1] & 0x0F) > 0x09) return false;
	if ((buffer[2] & 0xF0) > 0x90 || (buffer[2] & 0x0F) > 0x09) return false;

	qTrack = BcdToBin(buffer[1]);
	qIndex = BcdToBin(buffer[2]);
	return true;
}

// ── MMC structure commands ──────────────────────────────────────────────────
// These query the drive's firmware cache, not the disc surface.  They complete
// in milliseconds and work even on discs with damaged/illegal TOCs — the drive
// has already parsed the lead-in during spin-up and cached the results.

bool ScsiDrive::ReadDiscCapacity(DWORD& lastLBA, int& sessions, int& lastTrack) {
	BYTE cdb[10] = {};
	BYTE buffer[34] = {};

	cdb[0] = 0x51;
	cdb[1] = 0x00;
	cdb[7] = 0x00;
	cdb[8] = sizeof(buffer);

	if (!SendSCSI(cdb, 10, buffer, sizeof(buffer))) return false;

	// Last Track Number in Last Session (MSB:LSB)
	lastTrack = (buffer[11] << 8) | buffer[6];

	// Number of Sessions (MSB:LSB)
	sessions = (buffer[9] << 8) | buffer[4];
	if (sessions < 1) sessions = 1;

	lastLBA = (static_cast<DWORD>(buffer[16]) << 24) |
		(static_cast<DWORD>(buffer[17]) << 16) |
		(static_cast<DWORD>(buffer[18]) << 8) |
		static_cast<DWORD>(buffer[19]);

	if (lastLBA == 0) {
		lastLBA = (static_cast<DWORD>(buffer[20]) << 24) |
			(static_cast<DWORD>(buffer[21]) << 16) |
			(static_cast<DWORD>(buffer[22]) << 8) |
			static_cast<DWORD>(buffer[23]);
	}

	// Reject obviously invalid values — a CD cannot exceed ~90 minutes
	// (405,000 sectors).  Copy-protected TOCs often return 0xFFFFFFFF or
	// other absurd LBAs.  Also reject 0 (field not populated).
	if (lastLBA == 0 || lastLBA > 405000) {
		lastLBA = 0;
	}
	if (lastTrack > 99 || lastTrack < 1) {
		lastTrack = 0;
	}
	if (sessions > 10) {
		sessions = 1;  // No real CD has >10 sessions
	}

	return lastLBA > 0 || lastTrack > 0;
}

bool ScsiDrive::ReadTrackInfo(int trackNumber, DWORD& startLBA, DWORD& trackLength,
	bool& isAudio, int& session, int& mode) {
	// READ TRACK INFORMATION (0x52) — per-track metadata from drive firmware
	BYTE cdb[10] = {};
	BYTE buffer[36] = {};

	cdb[0] = 0x52;                   // READ TRACK INFORMATION
	cdb[1] = 0x01;                   // Address/Number Type = track number
	cdb[4] = static_cast<BYTE>(trackNumber);
	cdb[7] = 0x00;
	cdb[8] = sizeof(buffer);         // Allocation length

	if (!SendSCSI(cdb, 10, buffer, sizeof(buffer))) return false;

	// Byte 2: session number this track belongs to
	session = buffer[2];

	// Byte 5, bits 3-0: track mode
	//   0x00 or 0x02 = audio (2 ch without/with pre-emphasis)
	//   0x01 or 0x03 = audio (4 ch)
	//   0x04 = data, uninterrupted
	//   0x05 = data, incremental
	mode = buffer[5] & 0x0F;
	isAudio = (mode <= 0x03);

	// Bytes 8-11: track start address (LBA, MSB first)
	startLBA = (static_cast<DWORD>(buffer[8]) << 24) |
		(static_cast<DWORD>(buffer[9]) << 16) |
		(static_cast<DWORD>(buffer[10]) << 8) |
		static_cast<DWORD>(buffer[11]);

	// Bytes 24-27: track size in sectors (MSB first)
	trackLength = (static_cast<DWORD>(buffer[24]) << 24) |
		(static_cast<DWORD>(buffer[25]) << 16) |
		(static_cast<DWORD>(buffer[26]) << 8) |
		static_cast<DWORD>(buffer[27]);

	return true;
}
