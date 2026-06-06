// ============================================================================
// OpticalDrive_Fingerprinting.cpp - Disc identification algorithms
//
// Computes CDDB, MusicBrainz, and AccurateRip disc IDs from the TOC, plus a
// lightweight FNV-1a audio content hash.  Includes self-contained SHA-1 and
// Base64 implementations so the module has no external crypto dependencies.
// ============================================================================
#define NOMINMAX
#include "OpticalDrive.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <fstream>
#include <iomanip>
// ... other includes as needed

// ============================================================================
// GenerateDiscFingerprint
//
// Orchestrator: computes all disc IDs (CDDB, MusicBrainz, AccurateRip) and an
// optional audio content hash, packages them into a DiscFingerprint, records a
// TOC summary string and timestamp, then prints the report if at least one ID
// was successfully generated.
// ============================================================================
bool OpticalDrive::GenerateDiscFingerprint(const DiscInfo& disc, DiscFingerprint& fingerprint) {
	if (disc.tracks.empty()) {
		std::cerr << "Error: No tracks to fingerprint\n";
		return false;
	}

	std::cout << "\n=== Generating Disc Fingerprint ===\n";
	fingerprint = DiscFingerprint{};  // Reset to defaults

	// Compute each ID independently — any single failure is non-fatal.
	bool cddbOk = CalculateCDDBId(disc, fingerprint.cddb);
	bool mbOk = CalculateMusicBrainzId(disc, fingerprint.musicBrainz);
	bool arOk = CalculateAccurateRipId(disc, fingerprint.accurateRip);

	// If raw audio sectors have already been cached in memory, also generate
	// a content-based hash for duplicate detection.
	if (!disc.rawSectors.empty()) {
		CalculateAudioFingerprint(disc, fingerprint.audio);
	}

	// Build a human-readable TOC summary string, e.g.:
	//   "Tracks: 12 | 0 18225 36840 ... | Lead-out: 234567"
	std::ostringstream toc;
	toc << "Tracks: " << disc.tracks.size() << " | ";
	for (const auto& t : disc.tracks) {
		toc << t.startLBA << " ";
	}
	toc << "| Lead-out: " << disc.leadOutLBA;
	fingerprint.tocString = toc.str();

	// Record the ISO 8601 timestamp of when this fingerprint was generated.
	time_t now = time(nullptr);
	char timeBuf[64];
	struct tm timeinfo;
	localtime_s(&timeinfo, &now);
	strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
	fingerprint.generationTime = timeBuf;

	// The fingerprint is valid if at least one ID was successfully computed.
	fingerprint.isValid = cddbOk || mbOk || arOk;

	if (fingerprint.isValid) {
		PrintDiscFingerprint(fingerprint);
	}

	return fingerprint.isValid;
}

// ============================================================================
// CalculateCDDBId
//
// Implements the CDDB / FreeDB disc-ID algorithm.  The 32-bit ID packs three
// fields:
//   Bits 31–24: digit-sum checksum mod 255
//   Bits 23–8:  total disc length in seconds
//   Bits  7–0:  track count
//
// Track offsets are expressed in CD frames (LBA + 150).  There are 75 frames
// per second on a Red Book audio CD.
// ============================================================================
bool OpticalDrive::CalculateCDDBId(const DiscInfo& disc, CDDBFingerprint& cddb) {
	if (disc.tracks.empty()) return false;

	cddb.trackCount = static_cast<int>(disc.tracks.size());
	cddb.trackOffsets.clear();

	int checksumTotal = 0;

	for (const auto& track : disc.tracks) {
		// Convert LBA to absolute CD frames (Red Book adds a 2-second / 150-frame offset).
		int frames = static_cast<int>(track.startLBA) + 150;
		int seconds = frames / 75;  // 75 frames per second

		cddb.trackOffsets.push_back(frames);
		checksumTotal += CDDBSum(seconds);  // Sum the decimal digits of `seconds`
	}

	// Total disc length in seconds = (lead-out − first track) / 75.
	int leadOutFrames = static_cast<int>(disc.leadOutLBA) + 150;
	int firstTrackFrames = static_cast<int>(disc.tracks[0].startLBA) + 150;
	cddb.totalSeconds = (leadOutFrames - firstTrackFrames) / 75;

	// Pack the three fields into one 32-bit disc ID.
	uint32_t checksum = static_cast<uint32_t>(checksumTotal % 255);
	cddb.discId = (checksum << 24) |
		(static_cast<uint32_t>(cddb.totalSeconds) << 8) |
		static_cast<uint32_t>(cddb.trackCount);

	// Format as an 8-character lower-case hex string.
	char hexBuf[16];
	snprintf(hexBuf, sizeof(hexBuf), "%08x", cddb.discId);
	cddb.discIdHex = hexBuf;

	std::cout << "  CDDB ID: " << cddb.discIdHex << "\n";
	return true;
}

// ── CDDBSum ─────────────────────────────────────────────────────────────────
// Sums the decimal digits of an integer, e.g. 123 → 1 + 2 + 3 = 6.
// Used by the CDDB checksum algorithm.
int OpticalDrive::CDDBSum(int n) {
	int sum = 0;
	while (n > 0) {
		sum += n % 10;
		n /= 10;
	}
	return sum;
}

// ============================================================================
// CalculateMusicBrainzId
//
// Builds the MusicBrainz disc ID by:
//   1. Constructing a fixed-length hex TOC string: 2-char first track,
//      2-char last track, 8-char lead-out, then 99 × 8-char track-offset
//      slots (unused slots padded with zeroes).
//   2. Hashing the TOC string with SHA-1.
//   3. Base64-encoding the 20-byte SHA-1 digest.
//   4. Applying MusicBrainz URL-safe character substitutions:
//      '+' → '.', '/' → '_', strip trailing '=', append '-'.
// ============================================================================
bool OpticalDrive::CalculateMusicBrainzId(const DiscInfo& disc, MusicBrainzFingerprint& mb) {
	if (disc.tracks.empty()) return false;

	mb.firstTrack = disc.tracks[0].trackNumber;
	mb.lastTrack = disc.tracks.back().trackNumber;
	mb.leadOutOffset = static_cast<int>(disc.leadOutLBA) + 150;

	mb.trackOffsets.clear();
	for (const auto& track : disc.tracks) {
		mb.trackOffsets.push_back(static_cast<int>(track.startLBA) + 150);
	}

	// Build the fixed-format uppercase hex TOC string.
	std::ostringstream tocStream;
	tocStream << std::hex << std::uppercase << std::setfill('0');

	tocStream << std::setw(2) << mb.firstTrack;
	tocStream << std::setw(2) << mb.lastTrack;
	tocStream << std::setw(8) << mb.leadOutOffset;

	// Always emit exactly 99 track slots (pad unused with 00000000).
	for (int i = 0; i < 99; i++) {
		if (i < static_cast<int>(mb.trackOffsets.size())) {
			tocStream << std::setw(8) << mb.trackOffsets[i];
		}
		else {
			tocStream << std::setw(8) << 0;
		}
	}

	std::string tocString = tocStream.str();

	// SHA-1 hash the TOC string.
	BYTE sha1Result[20];
	SHA1Hash(reinterpret_cast<const BYTE*>(tocString.c_str()),
		tocString.length(), sha1Result);

	// Base64-encode the 20-byte digest, then apply MusicBrainz substitutions.
	mb.discId = Base64Encode(sha1Result, 20);

	for (char& c : mb.discId) {
		if (c == '+') c = '.';       // URL-safe: '+' → '.'
		else if (c == '/') c = '_';  // URL-safe: '/' → '_'
	}
	while (!mb.discId.empty() && mb.discId.back() == '=') {
		mb.discId.pop_back();        // Strip Base64 padding
	}
	mb.discId += '-';                // MusicBrainz convention: trailing dash

	std::cout << "  MusicBrainz ID: " << mb.discId << "\n";
	return true;
}

// ============================================================================
// CalculateAccurateRipId
//
// Computes two 32-bit disc IDs used for AccurateRip database lookups:
//   discId1 = sum of all (trackOffset)          — simple checksum
//   discId2 = sum of all (trackOffset × trackN) — weighted checksum
// The lead-out contributes to discId2 with track number = (lastTrack + 1).
// Also stores the CDDB ID, which is required in the AccurateRip lookup URL.
// ============================================================================
bool OpticalDrive::CalculateAccurateRipId(const DiscInfo& disc, AccurateRipFingerprint& ar) {
	if (disc.tracks.empty()) return false;

	ar.trackCount = static_cast<int>(disc.tracks.size());
	ar.discId1 = 0;
	ar.discId2 = 0;

	int trackNum = 1;
	for (const auto& track : disc.tracks) {
		int offset = static_cast<int>(track.startLBA) + 150;
		ar.discId1 += static_cast<uint32_t>(offset);              // Simple sum
		ar.discId2 += static_cast<uint32_t>(offset) * trackNum;   // Weighted sum
		trackNum++;
	}

	// Lead-out contributes to discId2 with the next track number.
	ar.discId2 += static_cast<uint32_t>(disc.leadOutLBA + 150) * (trackNum);

	// Compute the CDDB ID for cross-reference (needed in the AccurateRip URL).
	CDDBFingerprint cddbTemp;
	CalculateCDDBId(disc, cddbTemp);
	ar.cddbDiscId = cddbTemp.discId;

	std::cout << "  AccurateRip ID1: " << std::hex << std::setfill('0')
		<< std::setw(8) << ar.discId1 << std::dec << "\n";
	std::cout << "  AccurateRip ID2: " << std::hex << std::setfill('0')
		<< std::setw(8) << ar.discId2 << std::dec << std::setfill(' ') << "\n";

	return true;
}

// ============================================================================
// CalculateAudioFingerprint
//
// Generates a lightweight audio content hash by sampling approximately 1% of
// sectors from each audio track.  Uses the FNV-1a algorithm (offset basis
// 2166136261, prime 16777619) for fast, well-distributed hashing.
//
// Produces three outputs:
//   - audioHash:     overall FNV-1a hash across all sampled sectors
//   - silenceProfile: separate FNV-1a hash tracking where silence occurs
//   - trackHashes[]: per-track FNV-1a hashes
// ============================================================================
bool OpticalDrive::CalculateAudioFingerprint(const DiscInfo& disc, AudioFingerprint& audio) {
	if (disc.rawSectors.empty()) return false;

	audio = AudioFingerprint{};
	audio.trackHashes.resize(disc.tracks.size(), 0);

	// FNV-1a initial offset basis values.
	uint32_t audioHash = 2166136261u;
	uint32_t silenceHash = 2166136261u;
	int sampleCount = 0;

	size_t sectorIdx = 0;
	for (size_t trackIdx = 0; trackIdx < disc.tracks.size(); trackIdx++) {
		const auto& track = disc.tracks[trackIdx];
		if (!track.isAudio) continue;  // Skip data tracks

		uint32_t trackHash = 2166136261u;
		DWORD trackSectors = track.endLBA - track.pregapLBA + 1;

		// Sample roughly 100 evenly-spaced sectors per track (~1%).
		int sampleInterval = std::max(1, static_cast<int>(trackSectors / 100));

		for (DWORD i = 0; i < trackSectors && sectorIdx < disc.rawSectors.size(); i++, sectorIdx++) {
			if (i % sampleInterval != 0) continue;  // Skip non-sampled sectors

			const auto& sector = disc.rawSectors[sectorIdx];
			if (sector.size() < AUDIO_SECTOR_SIZE) continue;

			// Read 4-byte words at 16-byte intervals through the 2352-byte sector.
			for (int j = 0; j < AUDIO_SECTOR_SIZE; j += 16) {
				uint32_t sample = *reinterpret_cast<const uint32_t*>(sector.data() + j);

				// Fold the sample into the running FNV-1a hash.
				audioHash ^= sample;
				audioHash *= 16777619u;

				// Track zero-valued samples separately for silence profiling.
				if (sample == 0) {
					silenceHash ^= static_cast<uint32_t>(sectorIdx & 0xFFFFFFFF);
					silenceHash *= 16777619u;
				}

				// Also accumulate a per-track hash.
				trackHash ^= sample;
				trackHash *= 16777619u;
			}
			sampleCount++;
		}

		audio.trackHashes[trackIdx] = trackHash;
	}

	audio.audioHash = audioHash;
	audio.silenceProfile = silenceHash;
	audio.sampleCount = sampleCount;

	std::cout << "  Audio Hash: " << std::hex << std::setfill('0')
		<< std::setw(8) << audio.audioHash << std::dec << std::setfill(' ') << "\n";
	std::cout << "  Samples analyzed: " << audio.sampleCount << "\n";

	return true;
}

// ============================================================================
// PrintDiscFingerprint
//
// Formats and prints a full fingerprint report to stdout, covering CDDB,
// MusicBrainz, AccurateRip IDs (with lookup URLs), audio content hash, and
// a TOC summary with generation timestamp.
// ============================================================================
void OpticalDrive::PrintDiscFingerprint(const DiscFingerprint& fingerprint) {
	std::cout << "\n" << std::string(50, '=') << "\n";
	std::cout << "         DISC FINGERPRINT REPORT\n";
	std::cout << std::string(50, '=') << "\n\n";

	// ── CDDB / FreeDB section ───────────────────────────────────────────
	std::cout << "=== CDDB/FreeDB ===\n";
	std::cout << "  Disc ID:      " << fingerprint.cddb.discIdHex << "\n";
	std::cout << "  Track Count:  " << fingerprint.cddb.trackCount << "\n";
	std::cout << "  Total Length: " << fingerprint.cddb.totalSeconds / 60 << ":"
		<< std::setfill('0') << std::setw(2) << fingerprint.cddb.totalSeconds % 60 << std::setfill(' ') << "\n";
	std::cout << "  Lookup URL:   " << fingerprint.GetCDDBUrl() << "\n";

	// ── MusicBrainz section ─────────────────────────────────────────────
	std::cout << "\n=== MusicBrainz ===\n";
	std::cout << "  Disc ID:      " << fingerprint.musicBrainz.discId << "\n";
	std::cout << "  Tracks:       " << fingerprint.musicBrainz.firstTrack << "-"
		<< fingerprint.musicBrainz.lastTrack << "\n";
	std::cout << "  Lead-out:     " << fingerprint.musicBrainz.leadOutOffset << " frames\n";
	std::cout << "  Lookup URL:   " << fingerprint.GetMusicBrainzUrl() << "\n";

	// ── AccurateRip section ─────────────────────────────────────────────
	std::cout << "\n=== AccurateRip ===\n";
	std::cout << "  Disc ID 1:    " << std::hex << std::setfill('0')
		<< std::setw(8) << fingerprint.accurateRip.discId1 << std::dec << "\n";
	std::cout << "  Disc ID 2:    " << std::hex << std::setfill('0')
		<< std::setw(8) << fingerprint.accurateRip.discId2 << std::dec << std::setfill(' ') << "\n";
	std::cout << "  Database URL: " << fingerprint.GetAccurateRipUrl() << "\n";

	// ── Audio content section (only if samples were analysed) ───────────
	if (fingerprint.audio.sampleCount > 0) {
		std::cout << "\n=== Audio Content ===\n";
		std::cout << "  Audio Hash:   " << std::hex << std::setfill('0')
			<< std::setw(8) << fingerprint.audio.audioHash << std::dec << std::setfill(' ') << "\n";
		std::cout << "  Samples:      " << fingerprint.audio.sampleCount << "\n";
	}

	// ── TOC summary and timestamp ───────────────────────────────────────
	std::cout << "\n=== TOC Summary ===\n";
	std::cout << "  " << fingerprint.tocString << "\n";
	std::cout << "  Generated:    " << fingerprint.generationTime << "\n";
	std::cout << std::string(50, '=') << "\n";
}

// ============================================================================
// SaveDiscFingerprint
//
// Writes the fingerprint data to an INI-style text file with [CDDB],
// [MusicBrainz], and [AccurateRip] sections.
// ============================================================================
bool OpticalDrive::SaveDiscFingerprint(const DiscFingerprint& fingerprint, const std::wstring& filename) {
	std::ofstream file(filename);
	if (!file) return false;

	file << "# Disc Fingerprint\n";
	file << "# Generated: " << fingerprint.generationTime << "\n\n";
	file << "[CDDB]\nDiscID=" << fingerprint.cddb.discIdHex << "\n";
	file << "TrackCount=" << fingerprint.cddb.trackCount << "\n";
	file << "TotalSeconds=" << fingerprint.cddb.totalSeconds << "\n\n";
	file << "[MusicBrainz]\nDiscID=" << fingerprint.musicBrainz.discId << "\n\n";
	file << "[AccurateRip]\nDiscID1=" << std::hex << fingerprint.accurateRip.discId1 << "\n";
	file << "DiscID2=" << fingerprint.accurateRip.discId2 << std::dec << "\n";
	file.close();
	return true;
}

// ============================================================================
// Base64Encode
//
// Standard RFC 4648 Base64 encoder.  Processes input 3 bytes at a time,
// emitting 4 characters each iteration.  Pads with '=' when the input length
// is not a multiple of 3.
// ============================================================================
std::string OpticalDrive::Base64Encode(const BYTE* data, size_t length) {
	static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string result;
	result.reserve(((length + 2) / 3) * 4);  // Pre-allocate exact output size
	for (size_t i = 0; i < length; i += 3) {
		// Pack up to 3 input bytes into a 24-bit value.
		uint32_t n = static_cast<uint32_t>(data[i]) << 16;
		if (i + 1 < length) n |= static_cast<uint32_t>(data[i + 1]) << 8;
		if (i + 2 < length) n |= static_cast<uint32_t>(data[i + 2]);

		// Extract four 6-bit groups, using '=' for missing input bytes.
		result += chars[(n >> 18) & 0x3F];
		result += chars[(n >> 12) & 0x3F];
		result += (i + 1 < length) ? chars[(n >> 6) & 0x3F] : '=';
		result += (i + 2 < length) ? chars[n & 0x3F] : '=';
	}
	return result;
}

// ============================================================================
// SHA1Hash
//
// Self-contained SHA-1 implementation per FIPS 180-4.
//   1. Pads the input message to a multiple of 512 bits (64 bytes):
//      append 0x80, then zeroes, then the 64-bit big-endian bit length.
//   2. Processes each 64-byte chunk through 80 rounds of mixing using four
//      phase-specific operations (Ch, Parity, Maj, Parity) with distinct
//      round constants.
//   3. Writes the final 20-byte (160-bit) digest to `output` in big-endian.
// ============================================================================
void OpticalDrive::SHA1Hash(const BYTE* data, size_t length, BYTE* output) {
	// Initial hash values per FIPS 180-4 §5.3.1.
	uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

	// ── Step 1: Pad the message ─────────────────────────────────────────
	// Append a single 0x80 byte, then enough zero bytes so that the total
	// length ≡ 56 (mod 64), then the 64-bit big-endian original bit length.
	size_t newLen = length + 1;
	while (newLen % 64 != 56) newLen++;
	std::vector<BYTE> msg(newLen + 8, 0);
	memcpy(msg.data(), data, length);
	msg[length] = 0x80;  // Append the "1" bit
	uint64_t bitLen = static_cast<uint64_t>(length) * 8;
	for (int i = 0; i < 8; i++) msg[newLen + 7 - i] = static_cast<BYTE>(bitLen >> (i * 8));

	// Bit-rotation helper.
	auto leftRotate = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };

	// ── Step 2: Process each 512-bit (64-byte) chunk ────────────────────
	for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
		uint32_t w[80];

		// Load 16 big-endian 32-bit words from the chunk.
		for (int i = 0; i < 16; i++)
			w[i] = (static_cast<uint32_t>(msg[chunk + i * 4]) << 24) | (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
			(static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8) | static_cast<uint32_t>(msg[chunk + i * 4 + 3]);

		// Expand the 16 words into 80 via XOR and left-rotate-1.
		for (int i = 16; i < 80; i++) w[i] = leftRotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

		// Initialise working variables from current hash state.
		uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

		// ── 80-round compression function ───────────────────────────────
		// Rounds  0–19: Ch(b,c,d)        k = 0x5A827999
		// Rounds 20–39: Parity(b,c,d)    k = 0x6ED9EBA1
		// Rounds 40–59: Maj(b,c,d)       k = 0x8F1BBCDC
		// Rounds 60–79: Parity(b,c,d)    k = 0xCA62C1D6
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
			else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
			else { f = b ^ c ^ d; k = 0xCA62C1D6; }
			uint32_t temp = leftRotate(a, 5) + f + e + k + w[i];
			e = d; d = c; c = leftRotate(b, 30); b = a; a = temp;
		}

		// Add this chunk's contribution to the running hash.
		h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
	}

	// ── Step 3: Write the final 20-byte digest in big-endian ────────────
	uint32_t hashes[] = { h0, h1, h2, h3, h4 };
	for (int i = 0; i < 5; i++) {
		output[i * 4] = static_cast<BYTE>(hashes[i] >> 24);
		output[i * 4 + 1] = static_cast<BYTE>(hashes[i] >> 16);
		output[i * 4 + 2] = static_cast<BYTE>(hashes[i] >> 8);
		output[i * 4 + 3] = static_cast<BYTE>(hashes[i]);
	}
}

// ============================================================================
// URL builders
//
// Each returns a ready-to-use lookup URL for the respective online database.
// ============================================================================

// CDDB / GnuDB lookup — simply appends the hex disc ID to the base URL.
std::string DiscFingerprint::GetCDDBUrl() const {
	return "https://gnudb.org/cd/" + cddb.discIdHex;
}

// MusicBrainz lookup — appends the URL-safe Base64 disc ID.
std::string DiscFingerprint::GetMusicBrainzUrl() const {
	return "https://musicbrainz.org/cdtoc/" + musicBrainz.discId;
}

// AccurateRip lookup — the URL path encodes the low nibbles of discId1 as
// directory components, followed by a filename that contains track count,
// both disc IDs, and the CDDB disc ID.
std::string DiscFingerprint::GetAccurateRipUrl() const {
	std::ostringstream url;
	url << "http://www.accuraterip.com/accuraterip/"
		<< std::hex << std::setfill('0')
		<< std::setw(1) << (accurateRip.discId1 & 0xF) << "/"           // Nibble 0
		<< std::setw(1) << ((accurateRip.discId1 >> 4) & 0xF) << "/"    // Nibble 1
		<< std::setw(1) << ((accurateRip.discId1 >> 8) & 0xF) << "/"    // Nibble 2
		<< "dBAR-" << std::setw(3) << std::dec << accurateRip.trackCount // Track count
		<< "-" << std::hex << std::setw(8) << accurateRip.discId1        // Full discId1
		<< "-" << std::setw(8) << accurateRip.discId2                    // Full discId2
		<< "-" << std::setw(8) << accurateRip.cddbDiscId << ".bin";      // CDDB ID
	return url.str();
}