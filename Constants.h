// ============================================================================
// Constants.h - CD sector geometry and SCSI command constants
// ============================================================================
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>

// ── CD sector byte-layout constants ─────────────────────────────────────────
// A Red Book audio CD stores 2352 bytes of PCM audio per sector (16-bit
// stereo at 44100 Hz).  Subchannel data (P–W) adds 96 bytes.  The drive can
// optionally return C2 error pointers (296 bytes) alongside the audio.
constexpr int AUDIO_SECTOR_SIZE = 2352;       // Raw 16-bit stereo PCM per sector
constexpr int SUBCHANNEL_SIZE = 96;           // P–W subchannel block
constexpr int RAW_SECTOR_SIZE = 2448;         // Audio (2352) + subchannel (96)
constexpr int C2_ERROR_SIZE = 296;            // C2 error pointer / flag block
constexpr int C2_POINTER_BYTES = 294;         // Actual error pointers (excludes 2-byte block error stats)
constexpr int SECTOR_WITH_C2_SIZE = 2648;     // Audio (2352) + C2 (296)
constexpr int FULL_SECTOR_WITH_C2 = 2744;     // Audio (2352) + subchannel (96) + C2 (296)

// ── Drive speed multipliers ─────────────────────────────────────────────────
// CD 1× = 176 KB/s data throughput.  0xFFFF is the MMC "maximum available" sentinel.
constexpr WORD CD_SPEED_1X = 176;             // 1× CD read speed in KB/s
constexpr WORD CD_SPEED_MAX = 0xFFFF;         // Ask drive for maximum speed

// ── SCSI MMC command opcodes ────────────────────────────────────────────────
// Only the two opcodes needed for raw audio extraction are declared here.
constexpr BYTE SCSI_READ_CD = 0xBE;           // READ CD – raw sector read
constexpr BYTE SCSI_SET_CD_SPEED = 0xBB;      // SET CD SPEED – spindle control

// ── Pioneer vendor extension for SET CD SPEED (byte 10) ─────────────────────
// Pioneer drives use byte 10 of the 12-byte SET CD SPEED CDB for an
// extended speed/session mode.  Bit 7 must always be set.  Bit 6 is the
// EEP (EEPROM) save flag — when set the speed setting persists across
// power cycles.  Bits 0–5 carry the speed or session mode value.
constexpr BYTE PIONEER_SPEED_EXT_FLAG   = 0x80; // Bit 7: always set on Pioneer
constexpr BYTE PIONEER_SPEED_EEP_SAVE   = 0x40; // Bit 6: persist to EEPROM

// ── Logging destination bitmask ─────────────────────────────────────────────
// Controls where diagnostic / progress messages are routed.
enum class LogOutput {
	None = 0,      // Suppress all output
	Console = 1,   // Write to stdout only
	File = 2,      // Write to a log file only
	Both = 3       // Write to stdout AND log file
};

// ── Sector sizes ────────────────────────────────────────────────────────
// Red Book audio: 2352 bytes PCM (588 stereo samples × 4 bytes/sample).
// Subchannel:     96 bytes (P–W channels, 1 byte per bit × 96 bits).
// C2 error block: 296 bytes (294 error pointers + 2 block-error stats).
// Combined sizes are used to size SCSI transfer buffers depending on
// which data the caller requested.

// ── Speed constants ─────────────────────────────────────────────────────
// CD_SPEED_1X (176 KB/s) is the base multiplier for SET CD SPEED.
// CD_SPEED_MAX (0xFFFF) tells the drive to use its highest speed.

// ── BcdToBin / SubchannelCRC16 ──────────────────────────────────────────
// Inline utilities shared across ScsiDrive and OpticalDrive translation
// units.  BCD is the encoding used by Q subchannel for track/index/MSF.
// CRC-16-CCITT (poly 0x1021) validates Q subchannel integrity.

// ── Shared subchannel utility functions ─────────────────────────────────────
// Used by both ScsiDrive (low-level reads) and OpticalDrive (verification).
// Defined inline to avoid duplicate symbols across translation units.

// BCD-to-binary conversion.  CD subchannel data stores track, index, and MSF
// values in Binary-Coded Decimal.
inline BYTE BcdToBin(BYTE bcd) {
	return ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F);
}

// CRC-16-CCITT for Q subchannel (polynomial 0x1021).  Validates Q-channel
// integrity: bytes 0-9 are checked against the CRC stored in bytes 10-11.
inline uint16_t SubchannelCRC16(const BYTE* data, int len) {
	uint16_t crc = 0;
	for (int i = 0; i < len; i++) {
		crc ^= static_cast<uint16_t>(data[i]) << 8;
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
		}
	}
	return crc;
}

// ── Subchannel synthesis utilities ──────────────────────────────────────────
// Functions for generating Q-channel data and interleaving packed subchannel
// into the raw P-W format that drives expect for raw subchannel writes.

// Binary-to-BCD conversion (inverse of BcdToBin).  CD subchannel writes
// require track numbers and MSF values in Binary-Coded Decimal.
inline BYTE BinToBcd(BYTE bin) {
	return static_cast<BYTE>(((bin / 10) << 4) | (bin % 10));
}

// Fill CRC-16 bytes in a 12-byte Q subchannel record.  Computes CRC over
// bytes 0-9 and stores the inverted result in bytes 10-11 (big-endian).
// Per Red Book, CRC bits are inverted when stored on disc.
inline void SubchannelFillCRC(BYTE* q12) {
	uint16_t crc = SubchannelCRC16(q12, 10);
	crc ^= 0xFFFF;
	q12[10] = static_cast<BYTE>((crc >> 8) & 0xFF);
	q12[11] = static_cast<BYTE>(crc & 0xFF);
}

// Build a Mode-1 (position) Q subchannel record.
// control: 0x00 = standard audio, 0x01 = pre-emphasis, 0x02 = copy permitted.
// All time and track/index values are automatically BCD-encoded.
inline void BuildPositionQ(BYTE* q12, BYTE control, BYTE track, BYTE index,
	BYTE relM, BYTE relS, BYTE relF,
	BYTE absM, BYTE absS, BYTE absF) {
	q12[0] = static_cast<BYTE>((control << 4) | 0x01);  // CTL | ADR=1
	q12[1] = BinToBcd(track);
	q12[2] = BinToBcd(index);
	q12[3] = BinToBcd(relM);
	q12[4] = BinToBcd(relS);
	q12[5] = BinToBcd(relF);
	q12[6] = 0x00;
	q12[7] = BinToBcd(absM);
	q12[8] = BinToBcd(absS);
	q12[9] = BinToBcd(absF);
	SubchannelFillCRC(q12);
}

// Encode a 13-digit Media Catalog Number (MCN / EAN-13) into a Mode-2 Q
// record.  The 13 BCD digits are packed as pairs into bytes 1-7 (the 14th
// nibble is zero-padded).  Byte 9 carries the absolute frame (BCD).
// Red Book specifies MCN appears approximately once every 100 sectors.
inline void EncodeMCN(BYTE* q12, const char* mcn, BYTE absFrame) {
	q12[0] = 0x02;  // CTL=0 (audio), ADR=2 (MCN)
	for (int i = 1; i <= 7; i++) {
		BYTE hi = 0, lo = 0;
		if (*mcn >= '0' && *mcn <= '9') hi = static_cast<BYTE>(*mcn - '0');
		if (*mcn) mcn++;
		if (*mcn >= '0' && *mcn <= '9') lo = static_cast<BYTE>(*mcn - '0');
		if (*mcn) mcn++;
		q12[i] = static_cast<BYTE>((hi << 4) | lo);
	}
	q12[8] = 0x00;
	q12[9] = BinToBcd(absFrame);
	SubchannelFillCRC(q12);
}

// ISRC character-to-Q encoding per Red Book Table 22.
// '0'-'9' → 0x00-0x09, 'A'-'Z' → 0x11-0x2A.
inline BYTE ISRCCharToQ(char c) {
	if (c >= '0' && c <= '9') return static_cast<BYTE>(c - '0');
	if (c >= 'A' && c <= 'Z') return static_cast<BYTE>(0x11 + c - 'A');
	if (c >= 'a' && c <= 'z') return static_cast<BYTE>(0x11 + c - 'a');
	return 0;
}

// Encode a 12-character ISRC code into a Mode-3 Q record.
// Format: CC-OOO-YY-NNNNN (dashes optional, stripped automatically).
// First 5 characters are 6-bit packed, last 7 are BCD digit pairs.
// Red Book specifies ISRC appears approximately once every 100 sectors.
inline void EncodeISRC(BYTE* q12, const char* isrc, BYTE absFrame) {
	BYTE tmp[13] = {};
	int ti = 0;
	for (int i = 0; ti < 12 && isrc[i] != '\0'; i++) {
		if (isrc[i] == '-') continue;
		tmp[ti++] = ISRCCharToQ(isrc[i]);
	}

	q12[0] = 0x03;  // CTL=0 (audio), ADR=3 (ISRC)

	// Pack first 5 chars (6 bits each) into bytes 1-4
	q12[1] = static_cast<BYTE>((tmp[0] << 2) | ((tmp[1] >> 4) & 0x03));
	q12[2] = static_cast<BYTE>(((tmp[1] << 4) & 0xF0) | ((tmp[2] >> 2) & 0x0F));
	q12[3] = static_cast<BYTE>(((tmp[2] << 6) & 0xC0) | (tmp[3] & 0x3F));
	q12[4] = static_cast<BYTE>(tmp[4] << 2);

	// Pack last 7 digits as BCD pairs into bytes 5-8
	q12[5] = static_cast<BYTE>((tmp[5] << 4) | tmp[6]);
	q12[6] = static_cast<BYTE>((tmp[7] << 4) | tmp[8]);
	q12[7] = static_cast<BYTE>((tmp[9] << 4) | tmp[10]);
	q12[8] = static_cast<BYTE>(tmp[11] << 4);

	q12[9] = BinToBcd(absFrame);
	SubchannelFillCRC(q12);
}

// Assemble a complete 96-byte packed subchannel block from a 12-byte Q record.
// P channel (bytes 0-11) is all-ones during pause/pregap, all-zeros otherwise.
// R-W channels (bytes 24-95) are left as zeros.
inline void BuildPackedSubchannel(BYTE* packed96, const BYTE* q12, bool isPause) {
	memset(packed96, 0, 96);
	if (isPause) memset(packed96, 0xFF, 12);
	memcpy(packed96 + 12, q12, 12);
}

// Interleave packed subchannel (8 channels × 12 bytes = 96 bytes) into the
// raw interleaved format (96 bytes, each byte carries one bit per channel).
// This is the inverse of DeinterleaveSubchannel.
//
//   Bit mapping:  packed[ch*12 + i/8] bit (7 - i%8)  <->  raw[i] bit (7 - ch)
//   Channel order: bit 7 = P, bit 6 = Q, ... bit 0 = W.
inline void InterleaveSubchannel(const BYTE* packed, BYTE* raw) {
	memset(raw, 0, 96);
	for (int i = 0; i < 96; i++) {
		for (int ch = 0; ch < 8; ch++) {
			int srcByte = ch * 12 + (i / 8);
			int srcBit = 7 - (i % 8);
			if (packed[srcByte] & (1 << srcBit)) {
				raw[i] |= static_cast<BYTE>(0x80 >> ch);
			}
		}
	}
}