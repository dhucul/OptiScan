// ============================================================================
// CDStructures.h - Core CD disc and track structures
// ============================================================================
#pragma once

#include "Constants.h"
#include <windows.h>
#include <vector>
#include <string>
#include <tuple>

// ── Raw TOC LBA record (pre-clamping) ───────────────────────────────────
// When the drive returns corrupt TOC LBAs (e.g. 0xFFFFFFE2), the values
// are clamped and recovered.  This struct preserves the originals so the
// protection checker can evaluate the raw TOC, not the sanitised one.
struct RawTocEntry {
	int trackNumber = 0;
	DWORD originalStartLBA = 0;
	DWORD originalEndLBA = 0;
};

// ── Track descriptor ────────────────────────────────────────────────────────
// Describes a single track from the disc's Table of Contents (TOC).
struct TrackInfo {
	int trackNumber = 0;             // 1-based track number
	DWORD startLBA = 0;             // First sector of the track (INDEX 01) — from TOC, do not modify
	DWORD endLBA = 0;               // Last sector of the track
	DWORD pregapLBA = 0;            // First sector of the pre-gap (INDEX 00)
	DWORD index01LBA = 0;           // Detected INDEX 01 position (from subchannel scan)
	bool isAudio = true;            // true = audio track, false = data track
	int mode = 0;                    // Track mode (0 = audio, 1 = Mode 1, 2 = Mode 2)
	int session = 1;                 // Session number (multi-session discs)
	std::string isrc;                // International Standard Recording Code (per-track)
	bool hasPreemphasis = false;     // CD pre-emphasis flag (bit 0 of TOC control byte)
};

// ── CD-TEXT metadata ────────────────────────────────────────────────────────
// Optional album / track metadata embedded in the disc's lead-in sub-code.
struct CDText {
	std::string albumTitle;                    // Disc-level title
	std::string albumArtist;                   // Disc-level performer
	std::vector<std::string> trackTitles;      // Per-track titles
	std::vector<std::string> trackArtists;     // Per-track performers
};

// ── Pre-gap extraction mode ─────────────────────────────────────────────────
// Pre-gaps are the (often silent) audio between INDEX 00 and INDEX 01 of each
// track.  This enum controls how that audio is handled during a rip.
enum class PregapMode {
	Include = 0,        // Merge pre-gap audio into the track file (default)
	Skip = 1,           // Discard pre-gaps – rip starts at INDEX 01
	Separate = 2        // Write each pre-gap as its own file
};

// ── Master disc state ───────────────────────────────────────────────────────
// The single authoritative object that tracks everything about the disc
// currently being processed: TOC, raw sector data, error lists, rip settings,
// AccurateRip CRC, CD-TEXT, and more.
struct DiscInfo {
	std::vector<TrackInfo> tracks;                      // TOC track list
	std::vector<std::vector<BYTE>> rawSectors;          // Cached raw audio sectors
	bool includeSubchannel = true;                      // Request subchannel data during reads
	int sessionCount = 1;                               // Number of sessions on the disc
	int selectedSession = 0;                            // Session to rip (0 = all)
	int errorCount = 0;                                 // Total read errors encountered
	std::vector<DWORD> badSectors;                      // LBAs that failed to read
	std::vector<DWORD> c2ErrorSectors;                  // LBAs with C2 error flags
	bool enableC2Detection = false;                     // Whether C2 reporting is active
	bool driveSupportsAccurateStream = false;            // Drive guarantees jitter-free reads
	int totalC2Errors = 0;                              // Cumulative C2 error count
	LogOutput loggingOutput = LogOutput::Console;       // Where to send log messages
	std::vector<std::tuple<DWORD, int, double>> readLog;// Per-sector read log: (LBA, errors, timeMs)
	uint32_t accurateRipCRC = 0;                        // AccurateRip CRC for verification
	int driveOffset = 0;                                // Sample-level read offset correction
	PregapMode pregapMode = PregapMode::Include;        // How to handle pre-gaps
	bool extractHiddenTrack = false;                    // Extract hidden track one audio (HTOA)
	bool hasHiddenTrack = false;                        // HTOA detected before Track 1 INDEX 01
	CDText cdText;                                      // Embedded CD-TEXT metadata
	std::string mcn;                                    // Media Catalog Number (UPC/EAN), if present
	DWORD leadOutLBA = 0;                               // LBA of the lead-out area
	DWORD audioLeadOutLBA = 0;                          // Session-1 lead-out (for enhanced/multisession CDs)
	bool enableCacheDefeat = false;                     // Defeat drive read cache between reads
	bool tocRepaired = false;                           // TOC had out-of-range LBAs that were clamped
	bool tocSignCorrected = false;                      // TOC had firmware sign-bug LBAs that were auto-fixed
	bool tocLBAsRecovered = false;                      // Clamped LBAs were recovered from Full TOC data
	std::vector<RawTocEntry> rawTocEntries;             // Original LBAs before clamping/recovery
	DWORD rawLeadOutLBA = 0;                            // Original lead-out LBA before clamping
};

// ── TrackInfo ───────────────────────────────────────────────────────────
// One per TOC entry.  Carries the LBA range (pregap through end),
// audio/data flag, session number, ISRC, and pre-emphasis flag.
// startLBA is the INDEX 01 position from the TOC and must not be
// modified after reading; index01LBA is the subchannel-detected value.

// ── DiscInfo ────────────────────────────────────────────────────────────
// Master state object threaded through the entire application.  Holds:
//   • tracks[]         – TOC track list
//   • rawSectors[]     – cached raw 2352-byte audio sectors
//   • badSectors[]     – LBAs that failed to read
//   • c2ErrorSectors[] – LBAs with C2 error flags
//   • readLog[]        – per-sector (LBA, errors, timeMs) tuples
//   • cdText           – embedded CD-TEXT metadata
//   • accurateRipCRC   – verification CRC from the AR database
//   • driveOffset      – sample-level read offset correction
//   • pregapMode       – Include / Skip / Separate pre-gap audio
//   • tocRepaired      – whether corrupt TOC LBAs were clamped
//   • rawTocEntries[]  – original LBAs before clamping (for protection check)