// ============================================================================
// OpticalDrive_TOCScan.cpp - TOC-less disc scanning via raw Q subchannel
//
// When the Table of Contents is damaged, illegal, or missing entirely, this
// routine probes the disc from LBA 0 forward.  It reads the Q subchannel at
// each sector to discover track numbers, index transitions, and the actual
// lead-out position — then reconstructs a DiscInfo as if a valid TOC existed.
//
// Strategy (borrowed from CloneCD / IsoBuster):
//   Phase 0 — Ask drive firmware via READ DISC INFORMATION + READ TRACK
//             INFORMATION.  Completes in milliseconds, no disc I/O.
//   Phase 1 — Coarse Q-subchannel scan from LBA 0 with gap-hopping to
//             survive data tracks and inter-session gaps.
//   Phase 2 — Group coarse samples by track number.
//   Phase 3 — Binary-search refinement of audio↔audio boundaries only.
//   Phase 4 — Build DiscInfo with validated fields.
// ============================================================================
#include "OpticalDrive.h"
#include "ConsoleColors.h"
#include "InterruptHandler.h"
#include "Progress.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <map>
#include <set>
#include <chrono>
#include <cstring>

// ── Tuning constants ────────────────────────────────────────────────────────

// Maximum LBA to probe (~90 minutes at 75 sectors/sec).
static constexpr DWORD MAX_SCAN_LBA = 405000;

// Step size for coarse scan (sectors).
static constexpr DWORD COARSE_STEP = 75;  // 1 second of disc time

// Consecutive coarse-step failures before attempting a gap hop.
static constexpr int GAP_TRIGGER = 5;

// Gap-hop distances to try (LBA).  Each is probed in order; if any yields a
// readable sector we resume the coarse scan from there.
static constexpr DWORD GAP_HOPS[] = { 3750, 7500, 15000, 30000, 60000 };
static constexpr int GAP_HOP_COUNT = 5;

// LBA gap between coarse samples that indicates an unreadable region.
// Boundaries touching such gaps skip binary-search refinement entirely.
static constexpr DWORD GAP_THRESHOLD = COARSE_STEP * 3;  // 225 sectors = 3s

// Maximum iterations for binary-search refinement.  log2(405000) ≈ 19,
// so 20 is generous and prevents infinite grinding on pathological discs.
static constexpr int MAX_REFINE_ITER = 20;

// ── Slowdown / C2 probe constants ───────────────────────────────────────────
// A read taking longer than SLOWDOWN_MULTIPLIER × baseline is flagged.
static constexpr double SLOWDOWN_MULTIPLIER = 3.0;
// Minimum baseline samples before slowdown detection activates.
static constexpr int    SLOWDOWN_BASELINE_MIN = 10;
// Don't flag reads shorter than this absolute floor (ms) — avoids
// false positives when the baseline is artificially low.
static constexpr double SLOWDOWN_FLOOR_MS = 50.0;
// Maximum slowdown events to C2-probe (limits time spent on dying discs).
static constexpr int    MAX_SLOWDOWN_PROBES = 50;

// ── Slowdown event record ───────────────────────────────────────────────────
struct SlowdownEvent {
	DWORD  lba;
	double readTimeMs;       // Actual read time
	double baselineMs;       // Baseline average at the time
	int    c2Errors;         // -1 = C2 probe failed / not supported, 0 = clean, >0 = errors
	bool   readFailed;       // True if the SCSI read itself failed
	BYTE   senseKey;
	BYTE   asc;
	BYTE   ascq;
};

// ═════════════════════════════════════════════════════════════════════════════
// Phase 0 — MMC firmware query (CloneCD-style, no disc I/O)
// ═════════════════════════════════════════════════════════════════════════════
static bool TryMMCStructureScan(ScsiDrive& drive, DiscInfo& disc,
	DWORD& discCapacity)
{
	Console::Info("Querying drive firmware for disc structure...\n");

	DWORD lastLBA = 0;
	int sessions = 0, lastTrack = 0;
	bool hasDiscInfo = drive.ReadDiscCapacity(lastLBA, sessions, lastTrack);

	if (hasDiscInfo) {
		discCapacity = lastLBA;
		if (lastLBA > 0) {
			std::cout << "  Disc capacity: LBA " << lastLBA
				<< " (" << (lastLBA / 75 / 60) << ":"
				<< std::setfill('0') << std::setw(2) << ((lastLBA / 75) % 60)
				<< std::setfill(' ') << " mm:ss), "
				<< sessions << " session(s)";
			if (lastTrack > 0)
				std::cout << ", up to " << lastTrack << " tracks";
			std::cout << ".\n";
		}
	}
	else {
		Console::Warning("  READ DISC INFORMATION not supported - probing tracks directly.\n");
	}

	// If firmware gave a track count, use it.  Otherwise probe tracks 1-99
	// and stop after 3 consecutive READ TRACK INFORMATION failures.
	// Some drives (e.g. Plextor/LG rebrands) don't populate the track count
	// in READ DISC INFORMATION for pressed CDs but still support READ TRACK
	// INFORMATION per-track.
	int maxTrack = (lastTrack > 0) ? std::min(lastTrack, 99) : 99;
	int consecutiveFails = 0;

	// Enumerate tracks via READ TRACK INFORMATION
	int discovered = 0;
	for (int t = 1; t <= maxTrack; t++) {
		DWORD startLBA = 0, length = 0;
		bool isAudio = true;
		int session = 1, mode = 0;

		if (!drive.ReadTrackInfo(t, startLBA, length, isAudio, session, mode)) {
			// In probing mode (no track count), stop after consecutive failures
			if (lastTrack == 0 && ++consecutiveFails >= 3)
				break;
			continue;
		}
		consecutiveFails = 0;

		if (length == 0)
			continue;

		// Reject obviously invalid values from copy-protected TOCs
		if (startLBA > MAX_SCAN_LBA || length > MAX_SCAN_LBA)
			continue;

		TrackInfo ti;
		ti.trackNumber = t;
		ti.startLBA = startLBA;
		ti.endLBA = startLBA + length - 1;
		ti.pregapLBA = startLBA;
		ti.index01LBA = startLBA;
		ti.isAudio = isAudio;
		ti.session = session;
		ti.mode = mode;

		disc.tracks.push_back(ti);
		discovered++;

		printf("  Track %2d [S%d]: LBA %6u - %6u  (%u sectors)%s\n",
			t, session, ti.startLBA, ti.endLBA, length,
			isAudio ? "" : "  [DATA]");
	}

	if (discovered == 0) {
		Console::Warning("  READ TRACK INFORMATION returned no usable tracks.\n");
		disc.tracks.clear();
		return false;
	}

	// If firmware didn't provide disc capacity, derive from last track
	if (discCapacity == 0 && !disc.tracks.empty()) {
		discCapacity = disc.tracks.back().endLBA + 1;
	}

	disc.leadOutLBA = discCapacity > 0 ? discCapacity : disc.tracks.back().endLBA + 1;
	disc.audioLeadOutLBA = disc.leadOutLBA;
	disc.sessionCount = std::max(sessions, 1);
	disc.tocRepaired = true;

	// Snapshot reconstructed LBAs as "raw" entries for the firmware-query path.
	disc.rawTocEntries.clear();
	for (const auto& t : disc.tracks) {
		RawTocEntry raw;
		raw.trackNumber = t.trackNumber;
		raw.originalStartLBA = t.startLBA;
		raw.originalEndLBA = t.endLBA;
		disc.rawTocEntries.push_back(raw);
	}
	disc.rawLeadOutLBA = disc.leadOutLBA;

	std::cout << "  " << discovered
		<< " track(s) discovered via firmware query.\n\n";
	return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Pregap helper — detects INDEX 00 (pregap) and INDEX 01 (track start).
//
// refineBoundary finds the first sector where Q track == trackNumber, which
// may be INDEX 00 (pregap) or INDEX 01 (track start).  This function:
//   1. Checks if startLBA is in INDEX 00 — if so, scans forward to find
//      INDEX 01 and sets pregapLBA = startLBA, startLBA = INDEX 01 position.
//   2. If startLBA is already INDEX 01, scans backward to find the EARLIEST
//      INDEX 00 sector (the true pregap start).
// ═════════════════════════════════════════════════════════════════════════════
static void ProbePregap(ScsiDrive& drive, TrackInfo& ti)
{
	if (ti.startLBA == 0) return;

	int qt = 0, qi = 0;

	// ── Step 1: Check if startLBA is in the pregap (INDEX 00) ───────────
	// This single classification picks the branch direction for the entire
	// function (scan forward for INDEX 01 vs scan backward for INDEX 00), so
	// use the voted read first to suppress drive flakiness, then fall through
	// to the cheaper single-reads if voted-read isn't applicable.
	if (drive.ReadSectorQ(ti.startLBA, qt, qi)
		|| drive.ReadSectorQAnyType(ti.startLBA, qt, qi)
		|| drive.ReadSectorQSingle(ti.startLBA, qt, qi))
	{
		if (qt == ti.trackNumber && qi == 0) {
			// startLBA IS in the pregap — scan forward to find INDEX 01.
			// pregapLBA stays at the refineBoundary result, which is the
			// first sector of this track number (= pregap start).
			ti.pregapLBA = ti.startLBA;

			for (DWORD probe = ti.startLBA + 1;
				probe < ti.startLBA + 600; probe++)
			{
				int ft = 0, fi = 0;
				if (drive.ReadSectorQAnyType(probe, ft, fi)
					|| drive.ReadSectorQSingle(probe, ft, fi))
				{
					if (ft == ti.trackNumber && fi >= 1) {
						ti.index01LBA = probe;
						ti.startLBA = probe;
						return;
					}
					// Different track before INDEX 01 — shouldn't happen,
					// but stop to avoid scanning into the next track.
					if (ft != ti.trackNumber) return;
				}
			}
			return;
		}
	}

	// ── Step 2: startLBA is INDEX 01 — scan backward for INDEX 00 ───────
	// Keep scanning to find the EARLIEST INDEX 00 (the true pregap start),
	// not just the first one nearest to startLBA.
	DWORD probeLimit = (ti.startLBA > 225) ? (ti.startLBA - 225) : 0;

	for (DWORD probe = ti.startLBA - 1; probe >= probeLimit; probe--) {
		qt = 0; qi = 0;
		if (drive.ReadSectorQAnyType(probe, qt, qi)
			|| drive.ReadSectorQSingle(probe, qt, qi))
		{
			if (qt == ti.trackNumber && qi == 0) {
				// Keep going — update pregapLBA to track the earliest INDEX 00
				ti.pregapLBA = probe;
			}
			else {
				// Hit a different track or INDEX 01 of same track — stop.
				// pregapLBA is already set to the earliest INDEX 00 found.
				return;
			}
		}
		// Read failed — if we've already found INDEX 00, keep scanning
		// through the failure to find the true start. If not, keep going.

		// DWORD wraparound guard: probe-- would underflow if probe == 0
		if (probe == 0) break;
	}
}

// ═════════════════════════════════════════════════════════════════════════════
// Slowdown / C2 diagnostic report
// ═════════════════════════════════════════════════════════════════════════════
static void PrintSlowdownReport(const std::vector<SlowdownEvent>& slowdowns,
	bool c2Supported)
{
	if (slowdowns.empty()) return;

	int withC2 = 0;
	int withFail = 0;
	int clean = 0;
	int notProbed = 0;
	for (const auto& ev : slowdowns) {
		if (ev.readFailed)        withFail++;
		else if (ev.c2Errors > 0) withC2++;
		else if (ev.c2Errors == 0) clean++;
		else                       notProbed++;   // c2Errors == -1
	}

	std::cout << "\n";
	Console::Warning("  Slowdown analysis: ");
	std::cout << slowdowns.size() << " sector(s) exceeded "
		<< SLOWDOWN_MULTIPLIER << "x baseline read time.\n";

	if (c2Supported) {
		int probed = withC2 + withFail + clean;
		if (probed > 0) {
			if (withC2 > 0 || withFail > 0) {
				Console::SetColor(Console::Color::Red);
				std::cout << "  C2 correlation: " << withC2
					<< " sector(s) with C2 errors, "
					<< withFail << " read failure(s)";
				if (clean > 0)
					std::cout << ", " << clean << " clean";
				std::cout << ".\n";
				Console::Reset();
				Console::Warning(
					"  Slowdowns are likely caused by disc surface damage.\n"
					"  Recommendation: run a full C2 scan for detailed diagnostics.\n");
			}
			else {
				Console::SetColor(Console::Color::Green);
				std::cout << "  C2 probe: all " << clean
					<< " slow sector(s) returned 0 C2 errors.\n";
				Console::Reset();
				std::cout << "  Slowdown is likely mechanical (seek latency, spin-up,"
					" vibration) rather than data corruption.\n";
			}
		}

		if (notProbed > 0) {
			std::cout << "  " << notProbed
				<< " slowdown(s) on data sectors (C2 probe not applicable).\n";
		}
	}
	else {
		std::cout << "  C2 error reporting not supported by this drive -"
			" cannot determine if slowdowns are data-related.\n";
	}

	// Detailed table (top 20)
	int showCount = std::min(static_cast<int>(slowdowns.size()), 20);
	std::cout << "\n  LBA       Time(ms)  Baseline  Ratio   C2 Errors  Status\n";
	std::cout << "  " << std::string(68, '-') << "\n";

	// Sort by read time descending to show worst first
	auto sorted = slowdowns;
	std::sort(sorted.begin(), sorted.end(),
		[](const SlowdownEvent& a, const SlowdownEvent& b) {
			return a.readTimeMs > b.readTimeMs;
		});

	for (int i = 0; i < showCount; i++) {
		const auto& ev = sorted[i];
		double ratio = ev.baselineMs > 0 ? ev.readTimeMs / ev.baselineMs : 0;

		std::cout << "  " << std::setw(8) << ev.lba << "  "
			<< std::fixed << std::setprecision(1)
			<< std::setw(8) << ev.readTimeMs << "  "
			<< std::setw(8) << ev.baselineMs << "  "
			<< std::setw(5) << ratio << "x  ";

		if (ev.readFailed) {
			Console::SetColor(Console::Color::Red);
			std::cout << "   FAILED  ";
			Console::Reset();
			std::cout << "SK=0x" << std::hex << std::uppercase
				<< std::setfill('0') << std::setw(2)
				<< static_cast<int>(ev.senseKey) << std::dec << std::nouppercase
				<< std::setfill(' ');
		}
		else if (ev.c2Errors > 0) {
			Console::SetColor(Console::Color::Red);
			std::cout << std::setw(9) << ev.c2Errors << "  ";
			Console::Reset();
			std::cout << "DAMAGED";
		}
		else if (ev.c2Errors == 0) {
			Console::SetColor(Console::Color::Green);
			std::cout << "        0  ";
			Console::Reset();
			std::cout << "CLEAN";
		}
		else {
			std::cout << "      N/A  DATA";
		}
		std::cout << "\n";
	}

	if (static_cast<int>(slowdowns.size()) > showCount) {
		std::cout << "  ... and " << (slowdowns.size() - showCount)
			<< " more slowdown(s).\n";
	}

	// Restore default float formatting
	std::cout << std::defaultfloat;
}

// ═════════════════════════════════════════════════════════════════════════════
// Main entry point
// ═════════════════════════════════════════════════════════════════════════════
bool OpticalDrive::ScanDiscWithoutTOC(DiscInfo& disc, int scanSpeed)
{
	Console::Heading("\n=== TOC-less Disc Scan ===\n\n");

	// ── Phase 0: instant firmware query ─────────────────────────────────
	DWORD discCapacity = 0;
	if (TryMMCStructureScan(m_drive, disc, discCapacity)) {

		// Refine pregaps for audio tracks via Q subchannel
		Console::Info("Refining audio track pregaps via Q subchannel...\n");
		m_drive.SetSpeed(scanSpeed);

		int audioCount = 0;
		for (const auto& t : disc.tracks)
			if (t.isAudio) audioCount++;

		ProgressIndicator pregapProgress(40);
		pregapProgress.SetLabel("  Pregaps");
		pregapProgress.Start();

		int current = 0;
		for (auto& t : disc.tracks) {
			if (!t.isAudio) continue;
			ProbePregap(m_drive, t);
			current++;
			pregapProgress.Update(current, audioCount > 0 ? audioCount : 1);
		}
		pregapProgress.Finish(true);

		Console::Success("\nDisc scan complete (firmware query).\n");
		m_drive.SetSpeed(0);
		return true;
	}

	// ── Phase 1: coarse Q-subchannel scan ───────────────────────────────
	Console::Info("Scanning disc from LBA 0 via Q subchannel...\n");
	Console::Info("This may take several minutes.\n\n");

	m_drive.SetSpeed(scanSpeed);

	struct QSample {
		DWORD lba;
		int   track;
		int   index;
		bool  isAudio;
	};

	std::vector<QSample> samples;
	int   consecutiveFailures = 0;
	DWORD lastReadableLBA = 0;
	int   lastSuccessStep = 0;
	bool  inGap = false;

	// Use firmware capacity for progress if sane, else 90-minute max.
	DWORD scanLimit = MAX_SCAN_LBA;
	if (discCapacity > 0 && discCapacity <= MAX_SCAN_LBA)
		scanLimit = discCapacity;
	int totalSteps = static_cast<int>(scanLimit / COARSE_STEP);

	ProgressIndicator coarseProgress(40);
	coarseProgress.SetLabel("  Coarse scan");
	coarseProgress.Start();

	// ── Slowdown / C2 diagnostics state ─────────────────────────────────
	bool c2Supported = m_drive.CheckC2Support();
	double baselineSum = 0.0;
	int    baselineCount = 0;
	std::vector<SlowdownEvent> slowdowns;

	// Pre-allocate buffers for C2 probes (reused across iterations)
	std::vector<BYTE> c2Audio(AUDIO_SECTOR_SIZE, 0);
	std::vector<BYTE> c2Raw(C2_ERROR_SIZE, 0);
	ScsiDrive::C2ReadOptions c2Opts;
	c2Opts.multiPass = false;
	c2Opts.countBytes = true;
	c2Opts.defeatCache = true;   // Force a fresh read — Q-only reads do not
	// cache the full 2352-byte audio sector.

	int  step = 0;
	DWORD lba = 0;

	while (lba < MAX_SCAN_LBA) {
		if (g_interrupt.IsInterrupted()) {
			Console::Warning("\n*** Scan interrupted ***\n");
			return false;
		}

		int  qTrack = 0, qIndex = 0;
		bool audio = true;
		bool gotQ = false;

		auto readStart = std::chrono::steady_clock::now();

		if (inGap) {
			// Lightweight: single 16-byte SCSI call, any sector type
			if (m_drive.ReadSectorQAnyType(lba, qTrack, qIndex)) {
				gotQ = true;
				audio = false;
				// Reclassify: check if sector is actually audio
				int at = 0, ai = 0;
				if (m_drive.ReadSectorQSingle(lba, at, ai)) {
					audio = true;
					qTrack = at;
					qIndex = ai;
				}
			}
		}
		else {
			// Normal: try audio-typed first, then any-type fallback
			if (m_drive.ReadSectorQSingle(lba, qTrack, qIndex)) {
				gotQ = true;
				audio = true;
			}
			else if (m_drive.ReadSectorQAnyType(lba, qTrack, qIndex)) {
				gotQ = true;
				audio = false;
			}
		}

		double readMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - readStart).count();

		if (gotQ) {
			samples.push_back({ lba, qTrack, qIndex, audio });
			consecutiveFailures = 0;
			lastReadableLBA = lba;
			lastSuccessStep = step;
			inGap = false;

			// ── Slowdown detection ──────────────────────────────────
			// Compute baseline from PREVIOUS reads only (excludes current).
			double baselineAvg = baselineCount > 0
				? baselineSum / baselineCount : readMs;
			double threshold = std::max(baselineAvg * SLOWDOWN_MULTIPLIER,
				SLOWDOWN_FLOOR_MS);

			bool isSlowdown = baselineCount >= SLOWDOWN_BASELINE_MIN
				&& readMs > threshold
				&& static_cast<int>(slowdowns.size()) < MAX_SLOWDOWN_PROBES;

			if (!isSlowdown) {
				// Normal read — include in baseline
				baselineSum += readMs;
				baselineCount++;
			}
			else {
				// Slowdown detected — do NOT add to baseline (keeps it clean).
				SlowdownEvent ev{};
				ev.lba = lba;
				ev.readTimeMs = readMs;
				ev.baselineMs = baselineAvg;
				ev.c2Errors = -1;   // default: not probed / not applicable
				ev.readFailed = false;
				ev.senseKey = 0;
				ev.asc = 0;
				ev.ascq = 0;

				// C2 probe — only valid on audio sectors; data sectors would
				// fail with sense 0x05 / ASC 0x64 (illegal mode for this track).
				if (c2Supported && audio) {
					int c2Errors = 0;
					std::memset(c2Raw.data(), 0, C2_ERROR_SIZE);
					BYTE sk = 0, a = 0, aq = 0;
					bool ok = m_drive.ReadSectorWithC2Ex(
						lba, c2Audio.data(), nullptr, c2Errors,
						c2Raw.data(), c2Opts, &sk, &a, &aq);
					if (ok) {
						ev.c2Errors = c2Errors;
						ev.senseKey = sk;
						ev.asc = a;
						ev.ascq = aq;
					}
					else {
						ev.readFailed = true;
						ev.senseKey = sk;
						ev.asc = a;
						ev.ascq = aq;
					}
				}

				slowdowns.push_back(ev);
			}
		}
		else {
			consecutiveFailures++;
			if (!inGap && lastSuccessStep > 0)
				inGap = true;

			// ── Failed-read slowdown detection ──────────────────────
			// A timed-out or very slow failed read is itself strong
			// evidence of disc damage — record it even without C2 data.
			if (baselineCount >= SLOWDOWN_BASELINE_MIN
				&& readMs > SLOWDOWN_FLOOR_MS
				&& static_cast<int>(slowdowns.size()) < MAX_SLOWDOWN_PROBES)
			{
				double baselineAvg = baselineSum / baselineCount;
				if (readMs > baselineAvg * SLOWDOWN_MULTIPLIER) {
					SlowdownEvent ev{};
					ev.lba = lba;
					ev.readTimeMs = readMs;
					ev.baselineMs = baselineAvg;
					ev.c2Errors = -1;       // No C2 probe — Q read already failed
					ev.readFailed = true;
					ev.senseKey = 0;
					ev.asc = 0;
					ev.ascq = 0;
					slowdowns.push_back(ev);
				}
			}

			// ── Gap-hop logic ───────────────────────────────────────
			if (consecutiveFailures == GAP_TRIGGER) {
				bool hopped = false;
				for (int h = 0; h < GAP_HOP_COUNT; h++) {
					DWORD hopLBA = lba + GAP_HOPS[h];
					if (hopLBA >= MAX_SCAN_LBA) break;

					int ht = 0, hi = 0;
					bool ha = false;
					if (m_drive.ReadSectorQAnyType(hopLBA, ht, hi)) {
						// Classify audio vs data at hop destination
						int at = 0, ai = 0;
						if (m_drive.ReadSectorQSingle(hopLBA, at, ai)) {
							ha = true;
							ht = at;
							hi = ai;
						}
						samples.push_back({ hopLBA, ht, hi, ha });
						lba = hopLBA;
						step = static_cast<int>(lba / COARSE_STEP);
						consecutiveFailures = 0;
						lastReadableLBA = hopLBA;
						lastSuccessStep = step;
						inGap = false;
						hopped = true;
						break;
					}
				}
				if (!hopped) break;   // All hops failed → real lead-out

				lba += COARSE_STEP;
				step++;
				coarseProgress.Update(step, totalSteps);
				continue;
			}
		}

		lba += COARSE_STEP;
		step++;
		coarseProgress.Update(step, totalSteps);
	}

	coarseProgress.Finish(true, step);
	std::cout << "  " << samples.size() << " samples collected.\n";

	if (samples.empty()) {
		Console::Error("No readable sectors found on disc.\n");
		return false;
	}

	// ── Phase 2: group samples by track number ──────────────────────────
	struct TrackBounds {
		DWORD firstLBA = UINT_MAX;
		DWORD lastLBA = 0;
		int   audioHits = 0;
		int   dataHits = 0;
	};
	std::map<int, TrackBounds> trackMap;

	for (const auto& s : samples) {
		if (s.track < 1 || s.track > 99) continue;
		auto& tb = trackMap[s.track];
		if (s.lba < tb.firstLBA) tb.firstLBA = s.lba;
		if (s.lba > tb.lastLBA)  tb.lastLBA = s.lba;
		if (s.isAudio) tb.audioHits++;
		else           tb.dataHits++;
	}

	if (trackMap.empty()) {
		Console::Error("No valid track numbers found in Q subchannel data.\n");
		return false;
	}

	// Classify audio vs data
	std::set<int> dataTrackNums;
	int audioTracks = 0, dataTracks = 0;
	for (const auto& kv : trackMap) {
		if (kv.second.dataHits > kv.second.audioHits) {
			dataTrackNums.insert(kv.first);
			dataTracks++;
		}
		else {
			audioTracks++;
		}
	}

	std::cout << "  Found " << trackMap.size() << " track(s) via Q subchannel";
	if (dataTracks > 0)
		std::cout << " (" << audioTracks << " audio, " << dataTracks << " data)";
	std::cout << ".\n\n";

	// ── Phase 3: refine boundaries ──────────────────────────────────────
	Console::Info("Refining track boundaries...\n");

	std::vector<int> trackNumbers;
	trackNumbers.reserve(trackMap.size());
	for (const auto& kv : trackMap)
		trackNumbers.push_back(kv.first);
	std::sort(trackNumbers.begin(), trackNumbers.end());

	int refineTotal = static_cast<int>(trackNumbers.size()) * 2;
	int refineCurrent = 0;

	ProgressIndicator refineProgress(40);
	refineProgress.SetLabel("  Refining");
	refineProgress.Start();

	// Binary-search with iteration cap and lightweight probes first.
	auto refineBoundary = [&](DWORD lo, DWORD hi,
		int expectedTrack) -> DWORD
		{
			int iter = MAX_REFINE_ITER;
			while (lo < hi && iter-- > 0) {
				DWORD mid = lo + (hi - lo) / 2;
				int t = 0, idx = 0;
				bool found = m_drive.ReadSectorQAnyType(mid, t, idx)
					|| m_drive.ReadSectorQSingle(mid, t, idx);
				if (found && t == expectedTrack)
					hi = mid;
				else
					lo = mid + 1;
			}
			return lo;
		};

	// ── Phase 4: build DiscInfo ─────────────────────────────────────────
	disc.tracks.clear();
	disc.rawTocEntries.clear();
	disc.tocRepaired = true;

	for (size_t i = 0; i < trackNumbers.size(); i++) {
		int  tNum = trackNumbers[i];
		const auto& tb = trackMap[tNum];
		bool isData = dataTrackNums.count(tNum) > 0;

		TrackInfo ti;
		ti.trackNumber = tNum;
		ti.isAudio = !isData;
		ti.session = 1;

		// Detect unreadable gaps and adjacent data tracks
		bool prevIsData = (i > 0)
			&& dataTrackNums.count(trackNumbers[i - 1]) > 0;
		bool nextIsData = (i + 1 < trackNumbers.size())
			&& dataTrackNums.count(trackNumbers[i + 1]) > 0;

		bool gapBefore = false, gapAfter = false;
		if (i > 0) {
			DWORD prevEnd = trackMap[trackNumbers[i - 1]].lastLBA;
			gapBefore = (tb.firstLBA > prevEnd + GAP_THRESHOLD);
		}
		if (i + 1 < trackNumbers.size()) {
			DWORD nextStart = trackMap[trackNumbers[i + 1]].firstLBA;
			gapAfter = (nextStart > tb.lastLBA + GAP_THRESHOLD);
		}

		bool skipStart = isData || prevIsData || gapBefore;
		bool skipEnd = isData || nextIsData || gapAfter;

		// ── Start boundary ──────────────────────────────────────
		if (skipStart) {
			ti.startLBA = tb.firstLBA;
		}
		else if (i == 0) {
			ti.startLBA = refineBoundary(0, tb.firstLBA, tNum);
		}
		else {
			int prevTrack = trackNumbers[i - 1];
			ti.startLBA = refineBoundary(
				trackMap[prevTrack].lastLBA, tb.firstLBA, tNum);
		}

		// ── End boundary ────────────────────────────────────────
		if (skipEnd) {
			ti.endLBA = tb.lastLBA;
		}
		else if (i + 1 < trackNumbers.size()) {
			int nextTrack = trackNumbers[i + 1];
			DWORD nextStart = refineBoundary(
				tb.lastLBA, trackMap[nextTrack].firstLBA, nextTrack);
			ti.endLBA = (nextStart > 0) ? nextStart - 1 : tb.lastLBA;
		}
		else {
			ti.endLBA = lastReadableLBA;
		}

		// Sanity: clamp to prevent underflow / impossible ranges
		if (ti.endLBA < ti.startLBA)
			ti.endLBA = ti.startLBA;

		refineCurrent++;
		refineProgress.Update(refineCurrent, refineTotal);

		// ── Pregap ──────────────────────────────────────────────
		ti.pregapLBA = ti.startLBA;
		ti.index01LBA = ti.startLBA;

		if (!isData && !gapBefore)
			ProbePregap(m_drive, ti);

		refineCurrent++;
		refineProgress.Update(refineCurrent, refineTotal);

		disc.tracks.push_back(ti);

		DWORD sectorCount = ti.endLBA - ti.startLBA + 1;
		printf("  Track %2d: LBA %6u - %6u  (%6u sectors)%s%s\n",
			tNum, ti.startLBA, ti.endLBA, sectorCount,
			isData ? "  [DATA]" : "",
			(gapBefore || gapAfter) ? "  [GAP]" : "");
	}

	refineProgress.Finish(true);

	// ── Finalise DiscInfo ────────────────────────────────────────────────
	disc.leadOutLBA = lastReadableLBA + 1;
	disc.audioLeadOutLBA = disc.leadOutLBA;

	// For multi-session discs, set audioLeadOutLBA to end of last audio
	// track in session 1 so ripping doesn't extend into data sessions.
	for (auto it = disc.tracks.rbegin(); it != disc.tracks.rend(); ++it) {
		if (it->isAudio && it->session == 1) {
			disc.audioLeadOutLBA = it->endLBA + 1;
			break;
		}
	}

	// Snapshot reconstructed LBAs as "raw" entries — the original TOC was
	// unreadable, so these Q-subchannel-derived positions are the best we have.
	for (const auto& t : disc.tracks) {
		RawTocEntry raw;
		raw.trackNumber = t.trackNumber;
		raw.originalStartLBA = t.startLBA;
		raw.originalEndLBA = t.endLBA;
		disc.rawTocEntries.push_back(raw);
	}
	disc.rawLeadOutLBA = disc.leadOutLBA;

	Console::Success("\nDisc scan complete.\n");
	std::cout << "  " << disc.tracks.size() << " track(s) reconstructed";
	if (dataTracks > 0)
		std::cout << " (" << audioTracks << " audio, " << dataTracks << " data)";
	std::cout << ".  Lead-out at LBA " << disc.leadOutLBA << "\n";

	// ── Slowdown / C2 diagnostic report ─────────────────────────────────
	PrintSlowdownReport(slowdowns, c2Supported);

	m_drive.SetSpeed(0);
	return !disc.tracks.empty();
}
