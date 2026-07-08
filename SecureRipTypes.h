// ============================================================================
// SecureRipTypes.h - Secure ripping configuration and results
// ============================================================================
#pragma once

#include <windows.h>
#include <vector>
#include <string>

// ── Secure rip aggressiveness levels ────────────────────────────────────────
// Determines how many re-reads the ripper performs to guarantee bit-perfect
// output.  Higher modes are slower but more resilient to disc damage.
enum class SecureRipMode {
	Burst = -1,      // No verification – single fastest-possible read
	Disabled = 0,    // Standard single-pass read (default)
	Fast = 1,        // Light verification with minimal re-reads
	Standard = 2,    // Balanced re-read strategy
	Paranoid = 3     // Maximum re-reads for best accuracy (slowest)
};

// ── Secure rip tuning parameters ────────────────────────────────────────────
// Bundles every knob that controls the multi-pass verification engine.
struct SecureRipConfig {
	SecureRipMode mode = SecureRipMode::Disabled;
	int minPasses = 1;
	int maxPasses = 8;
	int requiredMatches = 2;
	bool useC2 = true;
	bool c2Guided = true;
	bool cacheDefeat = true;
	int maxSpeed = 0;

	// EXPERIMENTAL (opt-in via the OPTISCAN_DRIVE_READ_RETRY env var). When >= 0,
	// the secure-rip path overrides the drive's internal Read Error Recovery retry
	// count (mode page 0x01, byte 3) for the duration of the rip. A low value makes
	// the drive surface read errors/C2 to the host quickly instead of grinding
	// through internal re-reads, so OptiScan's own multi-pass/consensus engine
	// controls recovery. -1 leaves the drive's default untouched. Drive-dependent:
	// firmware may clamp or ignore it (the applied value is read back and reported).
	int driveReadRetryOverride = -1;
};

// ── Per-sector log entry from a secure rip ──────────────────────────────────
// One of these is recorded for every sector processed during a secure rip.
struct SecureRipLogEntry {
	DWORD lba = 0;               // Logical Block Address of this sector
	int track = 0;               // Track number the sector belongs to
	int phase = 0;               // Rip phase: 1 = first pass, 2 = sweep, 3 = rescue
	int passesUsed = 0;          // How many read attempts were made
	int matchCount = 0;          // How many passes returned identical data
	int c2Errors = 0;            // C2 error flags encountered across all passes
	double readTimeMs = 0.0;     // Wall-clock time spent reading this sector
	bool verified = false;       // true if the sector met the match threshold
	uint32_t hash = 0;           // CRC / hash of the accepted data
	// Phase-3 byte-consensus rescue figures (0 when no rescue ran for this sector).
	int rescuePasses = 0;        // Re-read passes the consensus rescue performed
	int bytesConfirmed = 0;      // Audio bytes the rescue confirmed at quorum
	int c2DisputedBytes = 0;     // Consensus-kept bytes the drive's C2 flagged bad
};

// ── Per-phase aggregate statistics ──────────────────────────────────────────
// Summarises performance for one phase (first-pass / sweep / rescue).
struct SecureRipPhaseStats {
	int phase = 0;               // Phase number (1, 2, or 3)
	int sectorsProcessed = 0;    // Sectors attempted in this phase
	int sectorsVerified = 0;     // Sectors that passed verification
	int sectorsFailed = 0;       // Sectors still unverified after this phase
	double durationSeconds = 0.0;// Elapsed wall-clock time
	double avgReadTimeMs = 0.0;  // Mean per-sector read time
};

// ── Complete secure rip log ─────────────────────────────────────────────────
// Top-level container that stores every per-sector entry plus per-phase
// summaries and disc-wide totals.
struct SecureRipLog {
	std::string modeName;                         // Human-readable mode name ("Paranoid", etc.)
	int minPasses = 0;
	int maxPasses = 0;
	int requiredMatches = 0;
	bool useC2 = false;
	bool cacheDefeat = false;

	std::vector<SecureRipLogEntry> entries;        // One entry per sector
	std::vector<SecureRipPhaseStats> phaseStats;   // One entry per phase

	int totalSectors = 0;
	int totalVerified = 0;
	int totalUnsecure = 0;           // Sectors that never met the match threshold
	int totalC2Errors = 0;
	double totalDurationSeconds = 0.0;
};

// ── Individual sector outcome after all passes ──────────────────────────────
struct SecureSectorResult {
	DWORD lba = 0;                   // Sector address
	int passesRequired = 0;          // Passes needed before a match was found
	int totalPasses = 0;             // Total passes attempted
	int matchingPasses = 0;          // Passes that agreed with the accepted data
	int c2ErrorPasses = 0;           // Passes that had C2 errors
	bool isSecure = false;           // true = verified, false = gave up
	bool usedCacheDefeat = false;    // Whether cache defeat was needed
	uint32_t finalHash = 0;          // Hash of the accepted data

	// Byte-level consensus rescue outcome (recovery-rip engine).  bytesTotal == 0
	// means no byte-level rescue was attempted for this sector.
	int bytesTotal = 0;              // audio bytes the rescue evaluated (0 = none)
	int bytesConfirmed = 0;          // bytes that reached the consensus quorum
	int c2DisputedBytes = 0;         // consensus-kept bytes the drive's C2 flagged bad
};

// ── Disc-wide secure rip summary ────────────────────────────────────────────
// High-level statistics for the entire rip, plus a list of problem sectors.
struct SecureRipResult {
	int totalSectors = 0;
	int secureSectors = 0;           // Sectors that passed verification
	int unsecureSectors = 0;         // Sectors that failed verification
	int singlePassSectors = 0;       // Verified on first read
	int multiPassSectors = 0;        // Needed extra passes
	int byteRecoveredSectors = 0;    // Fully rebuilt by consensus rescue (counted within secureSectors)
	int partialSectors = 0;          // Rescue ran but some bytes unconfirmed (within unsecureSectors)
	int unconfirmedBytes = 0;        // Total best-vote bytes kept across partial sectors
	long long c2DisputedBytes = 0;   // Consensus-kept bytes the drive's C2 still flagged bad
	int c2DisputedSectors = 0;       // Sectors with >= 1 such disputed byte
	int averagePassesRequired = 0;
	int maxPassesRequired = 0;
	std::vector<SecureSectorResult> problemSectors;  // Only sectors that had trouble
	double securityConfidence = 0.0; // 0.0–100.0 overall quality percentage
	std::string qualityAssessment;   // Human-readable verdict ("Excellent", etc.)
	SecureRipLog log;                // Full detailed log data
};
