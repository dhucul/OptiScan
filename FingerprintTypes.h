// ============================================================================
// FingerprintTypes.h - Disc identification and fingerprinting structures
//
// These structs hold the output of three standard disc-identification
// algorithms (CDDB, MusicBrainz, AccurateRip) plus a lightweight audio
// content hash.  Together they enable online metadata lookups and rip
// verification against community databases.
// ============================================================================
#pragma once

#include <windows.h>
#include <vector>
#include <string>

// ── CDDB / FreeDB disc ID ───────────────────────────────────────────────────
// The classic 8-hex-digit identifier computed from the TOC.  Encodes a
// digit-sum checksum (8 bits), total disc length (16 bits), and track
// count (8 bits) into a single 32-bit value.
struct CDDBFingerprint {
	uint32_t discId = 0;           // 32-bit packed ID
	std::string discIdHex;          // 8-character hex string (e.g. "a40b920e")
	int trackCount = 0;            // Number of tracks on the disc
	int totalSeconds = 0;          // Disc length in seconds
	std::vector<int> trackOffsets;  // Per-track frame offsets (LBA + 150)
};

// ── MusicBrainz disc ID ─────────────────────────────────────────────────────
// A Base64-encoded SHA-1 hash of a fixed-format TOC string containing first
// track, last track, lead-out offset, and 99 track-offset slots.
struct MusicBrainzFingerprint {
	std::string discId;             // URL-safe Base64 SHA-1 hash
	int firstTrack = 1;            // First track number (usually 1)
	int lastTrack = 0;             // Last track number
	int leadOutOffset = 0;          // Lead-out position in frames (LBA + 150)
	std::vector<int> trackOffsets;  // Per-track frame offsets (LBA + 150)
};

// ── AccurateRip disc identification ─────────────────────────────────────────
// Two 32-bit IDs derived from track offsets, used to look up per-track
// verification CRCs in the AccurateRip database.
struct AccurateRipFingerprint {
	uint32_t discId1 = 0;           // Simple sum of all track offsets
	uint32_t discId2 = 0;           // Weighted sum: sum(offset × trackNumber)
	uint32_t cddbDiscId = 0;        // CDDB ID (needed for the AccurateRip URL)
	int trackCount = 0;
	std::vector<uint32_t> trackCRCs; // Per-track CRC (populated after rip)
};

// ── Audio content fingerprint ───────────────────────────────────────────────
// A lightweight FNV-1a hash of sampled audio data, useful for duplicate
// detection and content verification independent of TOC metadata.
struct AudioFingerprint {
	uint32_t audioHash = 0;         // FNV-1a hash of sampled PCM data
	uint32_t silenceProfile = 0;    // FNV-1a hash of silence positions
	std::vector<uint32_t> trackHashes; // Per-track FNV-1a hashes
	int sampleCount = 0;            // Number of sectors sampled
};

// ── Combined disc fingerprint ───────────────────────────────────────────────
// Wraps all four identification methods into one object, with metadata
// (timestamp, TOC string) and convenience URL builders for database lookups.
struct DiscFingerprint {
	CDDBFingerprint cddb;                      // CDDB / FreeDB identification
	MusicBrainzFingerprint musicBrainz;        // MusicBrainz identification
	AccurateRipFingerprint accurateRip;        // AccurateRip identification
	AudioFingerprint audio;                    // Audio content hash

	std::string tocString;          // Human-readable TOC summary
	bool isValid = false;           // true if at least one ID was computed
	std::string generationTime;     // ISO 8601 timestamp of generation

	// Build ready-to-use lookup URLs for each database.
	std::string GetCDDBUrl() const;
	std::string GetMusicBrainzUrl() const;
	std::string GetAccurateRipUrl() const;
};