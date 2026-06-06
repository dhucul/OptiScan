// ============================================================================
// RecoveryTypes.h - Drive-independent C2 recovery rip configuration & results
//
// The recovery rip is a separate read engine from the secure rip. Instead of
// trusting the drive's C2 pointers, it rebuilds each hard sector from the
// statistical agreement of many re-reads:
//
//   • Per-byte majority voting   – the value a byte position settles on across
//                                  N reads is taken as truth once it reaches a
//                                  quorum, so corruption that is *unstable*
//                                  across reads is voted out.
//   • Jitter / offset alignment  – each re-read is slid against a reference
//                                  frame before voting, so drives that mis-seek
//                                  on re-read (returning sample-shifted audio)
//                                  still contribute aligned votes instead of
//                                  garbage.
//   • Hybrid C2 tie-break        – the drive's own C2 bitmap is used only as a
//                                  per-byte filter, and only after the C2
//                                  validation probe proves the drive's C2 is
//                                  reliable. On a lying drive it is ignored and
//                                  the engine degrades to pure consensus.
//
// What it cannot do: recover a sample that *every* read got wrong in the same
// way. Audio reads never expose the CIRC parity, so a stably-misread byte is
// indistinguishable from a correct one — those bytes are reported, not faked.
// ============================================================================
#pragma once

#include <windows.h>
#include <vector>
#include <string>

// ── Recovery rip tuning parameters ──────────────────────────────────────────
struct RecoveryRipConfig {
	int maxPasses = 16;          // Max re-read passes per problem sector
	int quorum = 3;              // Agreeing reads needed to *confirm* a byte
	int maxJitterSamples = 16;   // ± alignment search window, in stereo samples
	bool useC2TieBreak = true;   // Use drive C2 as a per-byte vote filter…
	bool cacheDefeat = true;     // …seek away between passes so re-reads are real
	int maxSpeed = 4;            // Speed cap during recovery (0 = drive maximum)
};

// ── Per-sector outcome ──────────────────────────────────────────────────────
enum class RecoverySectorStatus {
	CleanFirstPass,   // Read clean on the baseline sweep — no rescue needed
	Recovered,        // Every audio byte reached quorum during rescue
	Partial,          // Some bytes never reached quorum (best-vote value kept)
	Unrecovered       // The sector never produced a single successful read
};

struct RecoverySectorResult {
	DWORD lba = 0;
	int track = 0;
	int passesUsed = 0;
	int confirmedBytes = 0;        // of totalBytes
	int totalBytes = 0;            // AUDIO_SECTOR_SIZE
	int maxJitterSamples = 0;      // largest alignment shift used, in samples
	// Bytes consensus confirmed (reached quorum) that the drive's C2 still
	// flagged bad on a majority of its opinions — i.e. consensus overrode C2.
	// Only ever non-zero when the C2 tie-break was active. A heads-up, not a
	// failure: the value is consensus's best, but the drive disputed it.
	int c2FlaggedBytes = 0;
	RecoverySectorStatus status = RecoverySectorStatus::Unrecovered;
};

// ── Disc-wide recovery summary ──────────────────────────────────────────────
struct RecoveryRipResult {
	int totalSectors = 0;
	int cleanFirstPass = 0;        // sectors that read clean on the baseline sweep
	int recovered = 0;             // fully rebuilt by consensus
	int partial = 0;               // partially rebuilt (some bytes unconfirmed)
	int unrecovered = 0;           // never read successfully
	int problemSectors = 0;        // sectors that entered the rescue phase
	bool c2TieBreakActive = false; // C2 validation passed and C2 was used
	long long c2DisputedBytes = 0; // total consensus-accepted bytes C2 flagged bad
	int c2DisputedSectors = 0;     // sectors with at least one such byte
	double durationSeconds = 0.0;
	double confidence = 0.0;       // % of audio bytes confirmed across the disc
	std::string assessment;        // human-readable verdict
	std::vector<RecoverySectorResult> sectorResults; // non-clean sectors only
};
