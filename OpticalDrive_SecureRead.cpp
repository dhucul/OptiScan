#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <map>
#include <cstring>
#include <unordered_map>
#include <limits>

// ============================================================================
// Secure Rip Mode
// ============================================================================

bool OpticalDrive::ReadDiscSecure(DiscInfo& disc, const SecureRipConfig& config,
	SecureRipResult& result, std::function<void(int, int)> progress) {

	// Lock the tray for the duration of the secure rip (auto-unlocked on return).
	DriveDoorLockGuard doorLock(m_drive);

	SecureRipConfig effectiveConfig = config;
	effectiveConfig.cacheDefeat = disc.enableCacheDefeat;
	if (!disc.enableC2Detection) effectiveConfig.useC2 = false;

	// Apply the configured speed cap — higher speeds degrade read accuracy
	if (effectiveConfig.maxSpeed > 0) {
		m_drive.SetSpeed(effectiveConfig.maxSpeed);
	}

	// EXPERIMENTAL (opt-in via OPTISCAN_DRIVE_READ_RETRY): cap the drive's internal
	// read-retry count so it surfaces read errors/C2 to the host quickly instead of
	// grinding through firmware re-reads — letting this engine's multi-pass and
	// byte-consensus recovery do the work. Snapshotted and restored on return.
	const bool tuneRetry = (effectiveConfig.driveReadRetryOverride >= 0);
	ReadErrorRecoveryGuard errRecoveryGuard(m_drive, effectiveConfig.driveReadRetryOverride,
		tuneRetry);
	if (tuneRetry) {
		if (errRecoveryGuard.honored()) {
			if (errRecoveryGuard.originalRetry() == effectiveConfig.driveReadRetryOverride)
				std::cout << "  [experimental] Drive read-retry count already "
					<< effectiveConfig.driveReadRetryOverride << ".\n";
			else
				std::cout << "  [experimental] Drive read-retry count set to "
					<< effectiveConfig.driveReadRetryOverride
					<< " (was " << errRecoveryGuard.originalRetry()
					<< "); will be restored after the rip.\n";
		}
		else if (errRecoveryGuard.applied()) {
			std::cout << "  [experimental] Requested drive read-retry "
				<< effectiveConfig.driveReadRetryOverride
				<< ", but the drive reports " << errRecoveryGuard.effectiveRetry()
				<< " (clamped/ignored).\n";
		}
		else {
			std::cout << "  [experimental] Drive did not accept a read-retry override "
				<< "(Read Error Recovery page 0x01 unavailable on this drive).\n";
		}
	}

	uint64_t total64 = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		if (disc.selectedSession > 0 && disc.tracks[i].session != disc.selectedSession) continue;
		DWORD start = (disc.pregapMode == PregapMode::Skip) ? disc.tracks[i].startLBA : disc.tracks[i].pregapLBA;
		if (disc.tracks[i].endLBA < start) {
			std::cerr << "Error: Invalid track read range for track "
				<< disc.tracks[i].trackNumber << "\n";
			return false;
		}
		uint64_t count = static_cast<uint64_t>(disc.tracks[i].endLBA) - start + 1;
		if (count > std::numeric_limits<DWORD>::max() - total64) {
			std::cerr << "Error: Disc read range is too large\n";
			return false;
		}
		total64 += count;
	}
	DWORD total = static_cast<DWORD>(total64);

	result = SecureRipResult{};
	result.totalSectors = static_cast<int>(total);

	try {
		disc.rawSectors.clear();
		disc.rawSectors.reserve(total);
	}
	catch (const std::bad_alloc&) {
		std::cerr << "Error: Not enough memory\n";
		return false;
	}

	disc.errorCount = 0;
	disc.badSectors.clear();

	bool trustC2Clean = effectiveConfig.useC2 && effectiveConfig.c2Guided;

	// Initialize log with configuration
	auto& log = result.log;
	log.modeName = (effectiveConfig.mode == SecureRipMode::Fast) ? "Fast" :
		(effectiveConfig.mode == SecureRipMode::Standard) ? "Standard" :
		(effectiveConfig.mode == SecureRipMode::Paranoid) ? "Paranoid" : "Custom";
	log.minPasses = effectiveConfig.minPasses;
	log.maxPasses = effectiveConfig.maxPasses;
	log.requiredMatches = effectiveConfig.requiredMatches;
	log.useC2 = effectiveConfig.useC2;
	log.cacheDefeat = effectiveConfig.cacheDefeat;
	log.totalSectors = static_cast<int>(total);

	bool logToFile = (disc.loggingOutput == LogOutput::File || disc.loggingOutput == LogOutput::Both);
	if (logToFile) log.entries.reserve(total);

	std::cout << "  Secure rip: " << effectiveConfig.minPasses << "-" << effectiveConfig.maxPasses
		<< " passes, require " << effectiveConfig.requiredMatches << " matches\n";
	std::cout << "  Cache defeat: " << (effectiveConfig.cacheDefeat ? "ENABLED" : "DISABLED") << "\n";
	std::cout << "  C2-guided: " << (trustC2Clean ? "YES (fast path for clean sectors)" : "NO (all sectors verified)") << "\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n" << std::flush;

	if (progress) progress(0, total);

	auto overallStart = std::chrono::steady_clock::now();

	// ========================================================================
	// PHASE 1: Fast first pass — read everything, hash each sector
	// ========================================================================
	struct SectorState {
		size_t index;
		int sectorSize;
		uint32_t hash;
		int matchCount;
		bool isAudio;
		int track;
		bool hadC2Errors;
		bool hasValidHash;
	};

	std::vector<DWORD> rereadLBAs;
	std::map<DWORD, SectorState> sectorStates;

	auto phase1Start = std::chrono::steady_clock::now();
	SecureRipPhaseStats phase1Stats{ 1, 0, 0, 0, 0.0, 0.0 };
	double phase1TotalReadTime = 0.0;

	DWORD cur = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		auto& t = disc.tracks[i];
		if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;
		DWORD start = (disc.pregapMode == PregapMode::Skip) ? t.startLBA : t.pregapLBA;
		int sectorSize = disc.includeSubchannel ? RAW_SECTOR_SIZE : AUDIO_SECTOR_SIZE;

		for (DWORD lba = start;; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				return false;
			}

			std::vector<BYTE> sec(sectorSize, 0);
			int c2Errors = 0;
			bool ok = false;

			auto sectorStart = std::chrono::steady_clock::now();

			if (t.isAudio && effectiveConfig.useC2) {
				ScsiDrive::C2ReadOptions c2Opts;
				c2Opts.countBytes = true;
				BYTE* subPtr = (sectorSize > AUDIO_SECTOR_SIZE) ? sec.data() + AUDIO_SECTOR_SIZE : nullptr;
				ok = m_drive.ReadSectorWithC2Ex(lba, sec.data(), subPtr, c2Errors, nullptr, c2Opts);
			}
			else if (t.isAudio) {
				if (sectorSize > AUDIO_SECTOR_SIZE)
					ok = m_drive.ReadSector(lba, sec.data(), sec.data() + AUDIO_SECTOR_SIZE);
				else
					ok = m_drive.ReadSectorAudioOnly(lba, sec.data());
			}
			else {
				ok = disc.includeSubchannel
					? m_drive.ReadDataSectorWithSubchannel(
						lba, sec.data(), sec.data() + AUDIO_SECTOR_SIZE)
					: m_drive.ReadDataSector(lba, sec.data());
			}

			double readTimeMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - sectorStart).count();
			phase1TotalReadTime += readTimeMs;

			size_t idx = disc.rawSectors.size();
			uint32_t hash = ok ? HashSector(sec.data(), AUDIO_SECTOR_SIZE) : 0;
			disc.rawSectors.push_back(std::move(sec));

			bool phase1Trusted = ok && (c2Errors == 0);
			sectorStates[lba] = { idx, sectorSize, hash, phase1Trusted ? 1 : 0,
				t.isAudio, t.trackNumber, (c2Errors > 0), ok };

			phase1Stats.sectorsProcessed++;

			bool verified = false;
			if (!ok || c2Errors > 0) {
				rereadLBAs.push_back(lba);
				log.totalC2Errors += c2Errors;
			}
			else if (trustC2Clean || !t.isAudio) {
				result.secureSectors++;
				result.singlePassSectors++;
				phase1Stats.sectorsVerified++;
				verified = true;
			}
			else {
				rereadLBAs.push_back(lba);
			}

			if (logToFile) {
				log.entries.push_back({ lba, t.trackNumber, 1, 1, phase1Trusted ? 1 : 0,
					c2Errors, readTimeMs, verified, hash });
			}

			cur++;
			if (progress) progress(cur, total);
			if (lba == t.endLBA) break;
		}
	}

	phase1Stats.durationSeconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - phase1Start).count();
	phase1Stats.avgReadTimeMs = phase1Stats.sectorsProcessed > 0
		? phase1TotalReadTime / phase1Stats.sectorsProcessed : 0.0;
	phase1Stats.sectorsFailed = static_cast<int>(rereadLBAs.size());
	log.phaseStats.push_back(phase1Stats);

	if (rereadLBAs.empty()) {
		log.totalVerified = result.secureSectors;
		log.totalDurationSeconds = phase1Stats.durationSeconds;
		result.securityConfidence = 100.0;
		result.qualityAssessment = "Excellent";
		std::cout << "\n  All sectors verified - no re-reads needed\n";
		std::cout << "\n  Secure: " << result.secureSectors << "/" << result.totalSectors
			<< " (" << std::fixed << std::setprecision(1) << result.securityConfidence << "%)\n";
		return true;
	}

	std::sort(rereadLBAs.begin(), rereadLBAs.end());

	// ========================================================================
	// PHASE 2: Sequential sweep re-reads (no per-sector cache defeat)
	// ========================================================================
	std::vector<DWORD> stillUnverified;
	int maxSweeps = effectiveConfig.maxPasses - 1;
	int totalPhase2Verified = 0;
	auto phase2Start = std::chrono::steady_clock::now();
	double phase2TotalReadTime = 0.0;
	int phase2TotalProcessed = 0;

	// Track consecutive C2 failures per sector to fast-track unrecoverable ones
	std::unordered_map<DWORD, int> consecutiveC2Failures;
	std::vector<DWORD> phase3Pending;  // Fast-tracked sectors waiting for Phase 3

	for (int sweep = 0; sweep < maxSweeps && !rereadLBAs.empty(); sweep++) {
		std::cout << "\n  Phase 2 sweep " << (sweep + 1) << "/" << maxSweeps << ": "
			<< rereadLBAs.size() << " sectors\n";

		ProgressIndicator sweepProgress;
		sweepProgress.SetLabel("  Verify");
		sweepProgress.Start();

		int sweepTotal = static_cast<int>(rereadLBAs.size());
		int sweepCur = 0;
		stillUnverified.clear();

		// Reusable read buffer — avoids per-sector heap allocation
		std::vector<BYTE> buf;

		// Defeat cache at the start of every sweep so re-reads are genuine
		if (!rereadLBAs.empty()) {
			DefeatDriveCache(rereadLBAs.front(), disc.leadOutLBA);
		}

		for (DWORD lba : rereadLBAs) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				sweepProgress.Finish(false, sweepTotal);
				return false;
			}

			auto& state = sectorStates[lba];
			buf.assign(state.sectorSize, 0);

			bool ok = false;
			int c2Errors = 0;

			auto sectorStart = std::chrono::steady_clock::now();

			if (state.isAudio && effectiveConfig.useC2) {
				ScsiDrive::C2ReadOptions c2Opts;
				c2Opts.countBytes = true;
				BYTE* subPtr = (state.sectorSize > AUDIO_SECTOR_SIZE) ? buf.data() + AUDIO_SECTOR_SIZE : nullptr;
				ok = m_drive.ReadSectorWithC2Ex(lba, buf.data(), subPtr, c2Errors, nullptr, c2Opts);
			}
			else if (state.isAudio) {
				if (state.sectorSize > AUDIO_SECTOR_SIZE)
					ok = m_drive.ReadSector(lba, buf.data(), buf.data() + AUDIO_SECTOR_SIZE);
				else
					ok = m_drive.ReadSectorAudioOnly(lba, buf.data());
			}
			else {
				ok = disc.includeSubchannel
					? m_drive.ReadDataSectorWithSubchannel(
						lba, buf.data(), buf.data() + AUDIO_SECTOR_SIZE)
					: m_drive.ReadDataSector(lba, buf.data());
			}

			double readTimeMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - sectorStart).count();
			phase2TotalReadTime += readTimeMs;
			phase2TotalProcessed++;

			// Only C2-clean reads can build a "secure" consensus.  A read that
			// still carries C2 errors is kept only as best-effort fill (when no
			// cleaner data has been captured yet) and never counts toward the
			// match requirement — otherwise a flickering C2 sector could be
			// declared secure on bytes the drive flagged as bad, or a dirty read
			// could overwrite already-clean data.  state.hadC2Errors stays true
			// until the first clean read lands, so it doubles as a "no clean
			// baseline yet" flag.
			bool cleanRead = ok && (c2Errors == 0);

			// Track consecutive C2 failures — skip to Phase 3 early
			if (!ok || c2Errors > 0) {
				consecutiveC2Failures[lba]++;
			}
			else {
				consecutiveC2Failures[lba] = 0;
			}

			if (c2Errors > 0) log.totalC2Errors += c2Errors;

			bool verified = false;
			if (cleanRead) {
				uint32_t sweepHash = HashSector(buf.data(), AUDIO_SECTOR_SIZE);
				if (sweepHash == state.hash && state.hasValidHash && !state.hadC2Errors) {
					state.matchCount++;
				}
				else {
					// First clean read (or a changed value) becomes the baseline.
					state.hash = sweepHash;
					state.hasValidHash = true;
					state.matchCount = 1;
					memcpy(disc.rawSectors[state.index].data(), buf.data(), state.sectorSize);
				}
				state.hadC2Errors = false;

				int totalPasses = sweep + 2;
				if (state.matchCount >= effectiveConfig.requiredMatches &&
					totalPasses >= effectiveConfig.minPasses) {
					result.secureSectors++;
					result.multiPassSectors++;
					totalPhase2Verified++;
					verified = true;
					if (totalPasses > result.maxPassesRequired)
						result.maxPassesRequired = totalPasses;
				}
				else {
					stillUnverified.push_back(lba);
				}
			}
			else {
				// Failed or C2-dirty read.  If no clean data has been captured
				// for this sector yet, keep these bytes as best-effort fill so a
				// later all-fail Phase 3 still has the freshest read; this never
				// counts toward verification and never overwrites clean data.
				if (ok && state.hadC2Errors) {
					memcpy(disc.rawSectors[state.index].data(), buf.data(), state.sectorSize);
					state.hash = HashSector(buf.data(), AUDIO_SECTOR_SIZE);
					state.hasValidHash = true;
				}
				stillUnverified.push_back(lba);
			}

			if (logToFile) {
				log.entries.push_back({ lba, state.track, 2, sweep + 2,
					state.matchCount, c2Errors, readTimeMs, verified, state.hash });
			}

			sweepProgress.Update(++sweepCur, sweepTotal);
		}

		sweepProgress.Finish(true, sweepTotal);
		rereadLBAs = std::move(stillUnverified);

		// Remove sectors that have failed on every sweep so far —
		// they will never verify in Phase 2 and should go straight
		// to Phase 3 per-sector rescue instead of wasting more sweeps.
		constexpr int MAX_CONSECUTIVE_SWEEP_FAILURES = 3;
		if (sweep >= MAX_CONSECUTIVE_SWEEP_FAILURES - 1) {
			std::vector<DWORD> worthRetrying;
			for (DWORD lba : rereadLBAs) {
				if (consecutiveC2Failures[lba] >= MAX_CONSECUTIVE_SWEEP_FAILURES) {
					phase3Pending.push_back(lba);
				}
				else {
					worthRetrying.push_back(lba);
				}
			}
			if (!phase3Pending.empty() && worthRetrying.size() < rereadLBAs.size()) {
				int skipped = static_cast<int>(rereadLBAs.size() - worthRetrying.size());
				std::cout << "  " << skipped << " sector(s) fast-tracked to Phase 3 (persistent C2 errors)\n";
			}
			rereadLBAs = std::move(worthRetrying);
			// If nothing is worth retrying, stop sweeping
			if (rereadLBAs.empty()) break;
		}
	}

	// Merge fast-tracked sectors back for Phase 3
	rereadLBAs.insert(rereadLBAs.end(), phase3Pending.begin(), phase3Pending.end());
	std::sort(rereadLBAs.begin(), rereadLBAs.end());

	// ========================================================================
	// PHASE 3: Per-sector rescue for truly stubborn sectors
	// ========================================================================
	if (!rereadLBAs.empty()) {
		std::cout << "\n  Phase 3: " << rereadLBAs.size()
			<< " stubborn sectors - per-sector verification\n";

		// ── Byte-level consensus rescue setup ───────────────────────────────
		// The residue that whole-sector hash consensus could not secure is
		// handed to the recovery-rip engine (RescueSectorConsensus): aligned
		// per-byte majority voting with the drive's C2 bitmap as a tie-break.
		// This is the only path that performs true byte-level C2-guided repair.
		RecoveryRipConfig recCfg;
		recCfg.maxPasses        = std::max(effectiveConfig.maxPasses, 8);
		recCfg.quorum           = std::max(2, effectiveConfig.requiredMatches);
		recCfg.maxJitterSamples = 16;
		recCfg.useC2TieBreak    = effectiveConfig.useC2;
		// Always defeat the cache during the rescue, even on Accurate Stream
		// drives where the secure path skips it.  These are damaged sectors: the
		// point is to force genuine physical re-reads, otherwise the drive can
		// return the same cached erroneous bytes every pass and per-byte
		// consensus would "confirm" that bad data.
		recCfg.cacheDefeat      = true;
		recCfg.maxSpeed         = effectiveConfig.maxSpeed;

		// Skip the (expensive) windowed rescue when the stubborn set is so large
		// that the cause is systemic — a rejected read form or a digitally-silent
		// disc — rather than localized media damage, where re-reads cannot help.
		bool escalateToConsensus = (rereadLBAs.size() * 2 < total);
		if (!escalateToConsensus) {
			std::cout << "  (Byte-level rescue skipped - too many stubborn sectors; "
				"likely a read-form/silence issue, not media damage)\n";
		}

		// Hybrid C2 gate: trust the drive's C2 as a per-byte tie-break only if a
		// short probe shows it is reliable on this disc; otherwise pure consensus.
		bool c2TieBreak = escalateToConsensus && recCfg.useC2TieBreak;
		if (c2TieBreak) {
			std::vector<DWORD> probes;
			for (const auto& t : disc.tracks) {
				if (!t.isAudio) continue;
				if (t.endLBA > t.startLBA)
					probes.push_back(t.startLBA + (t.endLBA - t.startLBA) / 2);
				if (probes.size() >= 3) break;
			}
			int passed = 0;
			for (DWORD probe : probes)
				if (m_drive.ValidateC2Accuracy(probe)) passed++;
			c2TieBreak = (!probes.empty() && passed == static_cast<int>(probes.size()));
			std::cout << "  C2 tie-break: "
				<< (c2TieBreak ? "reliable\n" : "unreliable - pure consensus\n");
		}

		ProgressIndicator phase3Progress;
		phase3Progress.SetLabel("  Rescue");
		phase3Progress.Start();

		auto phase3Start = std::chrono::steady_clock::now();
		SecureRipPhaseStats phase3Stats{ 3, 0, 0, 0, 0.0, 0.0 };
		double phase3TotalReadTime = 0.0;

		int phase3Total = static_cast<int>(rereadLBAs.size());
		int phase3Cur = 0;

		for (DWORD lba : rereadLBAs) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				phase3Progress.Finish(false, phase3Total);
				return false;
			}

			auto& state = sectorStates[lba];

			auto sectorStart = std::chrono::steady_clock::now();

			SecureSectorResult secResult;
			bool ok = ReadSectorSecure(lba, disc.rawSectors[state.index].data(),
				state.sectorSize, state.isAudio, effectiveConfig, secResult,
				disc.leadOutLBA);
			phase3Stats.sectorsProcessed++;

			// Escalation: if whole-sector consensus could not secure an audio
			// sector, rebuild its 2352-byte payload by aligned per-byte voting.
			// Only the audio bytes are touched; any subchannel bytes are left as
			// the best-effort data already in the buffer.
			bool rescuedFull = false, rescuedPartial = false;
			int rescuePasses = 0;
			if (escalateToConsensus && state.isAudio && !secResult.isSecure) {
				BYTE* audioPtr = disc.rawSectors[state.index].data();
				std::vector<BYTE> preRescue(audioPtr, audioPtr + AUDIO_SECTOR_SIZE);

				RecoverySectorResult rr;
				rr.lba = lba;
				rr.track = state.track;
				rr.totalBytes = AUDIO_SECTOR_SIZE;
				RescueSectorConsensus(lba, audioPtr, recCfg, disc.leadOutLBA,
					c2TieBreak, rr);

				rescuePasses = rr.passesUsed;
				secResult.bytesTotal = rr.totalBytes;
				secResult.bytesConfirmed = rr.confirmedBytes;
				secResult.c2DisputedBytes = rr.c2FlaggedBytes;
				if (rr.c2FlaggedBytes > 0) {
					result.c2DisputedBytes += rr.c2FlaggedBytes;
					result.c2DisputedSectors++;
				}
				if (rr.passesUsed > result.maxPassesRequired)
					result.maxPassesRequired = rr.passesUsed;

				if (rr.status == RecoverySectorStatus::Recovered) {
					// Every audio byte reached quorum — a stronger guarantee than
					// a whole-sector hash match, so this counts as secure.
					secResult.isSecure = true;
					ok = true;
					rescuedFull = true;
				}
				else if (rr.status == RecoverySectorStatus::Partial) {
					// Improved but not fully confirmed: keep the best-vote bytes
					// (statistically better than a single best-effort read).
					ok = true;
					rescuedPartial = true;
					result.unconfirmedBytes += (rr.totalBytes - rr.confirmedBytes);
				}
				else {
					// Unrecovered: the rescue read nothing and zeroed the buffer —
					// restore the secure pass's best-effort bytes.
					memcpy(audioPtr, preRescue.data(), AUDIO_SECTOR_SIZE);
				}
			}

			double readTimeMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - sectorStart).count();
			phase3TotalReadTime += readTimeMs;

			if (secResult.isSecure) {
				result.secureSectors++;
				result.multiPassSectors++;
				phase3Stats.sectorsVerified++;
				if (rescuedFull) result.byteRecoveredSectors++;
			}
			else {
				result.unsecureSectors++;
				result.problemSectors.push_back(secResult);
				phase3Stats.sectorsFailed++;
				if (rescuedPartial) result.partialSectors++;
			}

			if (!ok) {
				disc.errorCount++;
				disc.badSectors.push_back(lba);
			}

			if (secResult.passesRequired > result.maxPassesRequired)
				result.maxPassesRequired = secResult.passesRequired;

			if (logToFile) {
				SecureRipLogEntry entry{ lba, state.track, 3, secResult.totalPasses,
					secResult.matchingPasses, secResult.c2ErrorPasses,
					readTimeMs, secResult.isSecure, secResult.finalHash };
				entry.rescuePasses = rescuePasses;
				entry.bytesConfirmed = secResult.bytesConfirmed;
				entry.c2DisputedBytes = secResult.c2DisputedBytes;
				log.entries.push_back(entry);
			}

			phase3Progress.Update(++phase3Cur, phase3Total);
		}

		phase3Stats.durationSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - phase3Start).count();
		phase3Stats.avgReadTimeMs = phase3Stats.sectorsProcessed > 0
			? phase3TotalReadTime / phase3Stats.sectorsProcessed : 0.0;
		log.phaseStats.push_back(phase3Stats);

		phase3Progress.Finish(true, phase3Total);
	}

	log.totalVerified = result.secureSectors;
	log.totalUnsecure = result.unsecureSectors;
	log.totalDurationSeconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - overallStart).count();

	result.securityConfidence = total > 0 ? static_cast<double>(result.secureSectors) / total * 100.0 : 0;

	// Calculate average passes from the phase stats
	int totalPasses = 0;
	int totalProcessed = 0;
	for (const auto& ps : log.phaseStats) {
		totalPasses += ps.sectorsProcessed;
		totalProcessed += ps.sectorsVerified;
	}
	result.averagePassesRequired = totalProcessed > 0
		? static_cast<int>(std::round(static_cast<double>(totalPasses) / totalProcessed))
		: 1;

	result.qualityAssessment = result.securityConfidence >= 99.9 ? "Excellent" :
		result.securityConfidence >= 99.0 ? "Good" :
		result.securityConfidence >= 95.0 ? "Acceptable" : "Poor";

	std::cout << "\n  Secure: " << result.secureSectors << "/" << result.totalSectors
		<< " (" << std::fixed << std::setprecision(1) << result.securityConfidence << "%)\n";

	if (result.byteRecoveredSectors > 0 || result.partialSectors > 0) {
		std::cout << "  Byte-level rescue: " << result.byteRecoveredSectors
			<< " recovered, " << result.partialSectors << " partial";
		if (result.c2DisputedBytes > 0)
			std::cout << " (" << result.c2DisputedBytes << " C2-disputed byte(s) kept)";
		std::cout << "\n";
	}

	return true;
}
