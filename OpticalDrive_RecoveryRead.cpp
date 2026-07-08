// ============================================================================
// OpticalDrive_RecoveryRead.cpp - Drive-independent C2 recovery rip
//
// A separate read engine from the secure rip. It does not trust the drive's
// C2 pointers as ground truth; instead it rebuilds each hard sector from the
// statistical agreement of many re-reads:
//
//   Phase 0  Hybrid C2 gate     — probe whether the drive's C2 is reliable
//                                 (ValidateC2Accuracy). Only if it passes is
//                                 C2 used as a per-byte vote filter; otherwise
//                                 the engine runs pure consensus.
//   Phase 1  Baseline sweep      — read every sector once. C2-clean reads are
//                                 trusted; everything else goes to rescue.
//   Phase 2  Consensus rescue     — per problem sector: read an overlapping
//                                 window many times, align each window against
//                                 a reference frame to defeat re-read jitter,
//                                 then majority-vote every byte position.
//
// See RecoveryTypes.h for the rationale and the hard limit (a sample that is
// stably misread on every pass cannot be recovered from audio reads alone).
// ============================================================================
#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <map>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kFrameBytes = 4;   // one stereo 16-bit sample pair

// Sum-of-absolute-differences alignment. Returns the byte delta (a multiple of
// kFrameBytes) within [-shiftNeg, +shiftPos] that best lines up the candidate
// window slice (window + centerOffset + delta, A bytes) with `reference`.
// A lower SAD means a better match; the early-out keeps the search cheap.
//
// Shifts are tried center-outward (0, ±4, ±8, …) and ties are kept by the
// strict "<" below, so the SMALLEST shift wins when several score equally. That
// matters for silent/periodic audio (pregaps, fades) where every shift matches
// perfectly — without it the search would lock onto the largest offset and both
// inflate the jitter stat and misalign the sparse real samples.
int BestAlignDelta(const BYTE* window, int centerOffset, const BYTE* reference,
	int A, int shiftNeg, int shiftPos) {
	long bestScore = LONG_MAX;
	int bestDelta = 0;
	const int maxMag = std::max(shiftNeg, shiftPos);

	auto eval = [&](int d) {
		if (d < -shiftNeg || d > shiftPos) return;
		const BYTE* p = window + centerOffset + d;
		long score = 0;
		for (int k = 0; k < A; k++) {
			int diff = static_cast<int>(p[k]) - static_cast<int>(reference[k]);
			score += (diff < 0) ? -diff : diff;
			if (score >= bestScore) return;   // cannot win — abandon this shift
		}
		if (score < bestScore) {
			bestScore = score;
			bestDelta = d;
		}
	};

	eval(0);
	for (int mag = kFrameBytes; mag <= maxMag; mag += kFrameBytes) {
		eval(-mag);
		eval(mag);
	}
	return bestDelta;
}

// True if audio byte `i` is flagged erroneous in a C2 error-pointer bitmap.
// In ErrorPointers mode (countBytes = false) the 294 pointer bytes form a
// 2352-bit map, one bit per audio byte, MSB first.
inline bool C2ByteFlagged(const BYTE* c2Raw, int i) {
	return (c2Raw[i >> 3] >> (7 - (i & 7))) & 1;
}

}  // namespace

// ── Per-sector consensus rescue ─────────────────────────────────────────────
bool OpticalDrive::RescueSectorConsensus(DWORD lba, BYTE* audioOut,
	const RecoveryRipConfig& cfg, DWORD maxLBA, bool c2TieBreak,
	RecoverySectorResult& sr) {

	const int A = AUDIO_SECTOR_SIZE;
	const int maxShift = cfg.maxJitterSamples * kFrameBytes;

	// Desired overlapping window: up to two sectors of margin on each side so a
	// jitter-shifted center can still be extracted with in-bounds indices. The
	// extents are clamped to the disc start and to the last readable sector
	// (maxLBA is the lead-out, an exclusive upper bound); the per-pass read then
	// shrinks the window further if the drive rejects it (e.g. the window would
	// span into the lead-out or an adjacent data track).
	const int wantBefore = 2, wantAfter = 2;
	int desiredBefore = (lba >= static_cast<DWORD>(wantBefore)) ? wantBefore : static_cast<int>(lba);
	int desiredAfter = wantAfter;
	if (maxLBA > 0 && lba + static_cast<DWORD>(desiredAfter) >= maxLBA)
		desiredAfter = (maxLBA > lba + 1) ? static_cast<int>(maxLBA - 1 - lba) : 0;

	std::vector<BYTE> window(static_cast<size_t>(desiredBefore + 1 + desiredAfter) * A);

	// Each accepted pass contributes an aligned 2352-byte view plus an optional
	// per-byte "C2 clean" mask. c2Opinion records whether this pass actually
	// carries a C2 verdict (so c2Clean can be read as good/bad rather than
	// "no opinion") — needed to annotate bytes where consensus overrode C2.
	struct Pass {
		std::vector<BYTE> audio;
		std::vector<bool> c2Clean;   // meaningful only when c2Opinion is true
		bool c2Opinion = false;      // this pass fetched a usable C2 bitmap
	};
	std::vector<Pass> passes;
	std::vector<BYTE> reference;     // frame anchor = first successful center

	int passesUsed = 0;
	int maxJit = 0;
	bool anySuccess = false;

	for (int pass = 0; pass < cfg.maxPasses; pass++) {
		if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey())
			break;
		if (cfg.cacheDefeat && pass > 0)
			DefeatDriveCache(lba, maxLBA);

		// Read the widest window the drive will accept, shrinking on failure so
		// boundary sectors (near lead-out, across track edges, or with an
		// unreadable neighbour) still rescue — down to a single-sector read.
		int useBefore = desiredBefore, useAfter = desiredAfter;
		bool readOk = false;
		while (true) {
			DWORD useStart = lba - static_cast<DWORD>(useBefore);
			DWORD useCount = static_cast<DWORD>(useBefore + 1 + useAfter);
			if (m_drive.ReadSectorsAudioOnly(useStart, useCount, window.data())) {
				readOk = true;
				break;
			}
			if (useAfter > 0) { useAfter--; continue; }
			if (useBefore > 0) { useBefore--; continue; }
			break;   // even a lone read of `lba` failed
		}
		if (!readOk) continue;   // this pass produced nothing
		passesUsed++;
		anySuccess = true;

		// Geometry for THIS pass (it may have shrunk from the desired window).
		int centerOffset = useBefore * A;
		int shiftNeg = std::min(maxShift, useBefore * A);
		int shiftPos = std::min(maxShift, useAfter * A);

		int delta = 0;
		if (reference.empty()) {
			// First good read defines the frame; the rest of the disc was read
			// against this same drive position, so it is a consistent anchor.
			reference.assign(window.begin() + centerOffset,
				window.begin() + centerOffset + A);
		} else {
			delta = BestAlignDelta(window.data(), centerOffset, reference.data(),
				A, shiftNeg, shiftPos);
			maxJit = std::max(maxJit, std::abs(delta));
		}

		Pass p;
		p.audio.assign(window.begin() + centerOffset + delta,
			window.begin() + centerOffset + delta + A);
		p.c2Clean.assign(A, true);

		// Hybrid C2 tie-break input: only when the drive's C2 validated, and
		// only on jitter-free passes (delta == 0) where the independent single-
		// sector C2 read lines up with the window center. Flagged bytes are
		// recorded so the vote can prefer C2-clean values when counts tie — C2
		// never overrides a clear consensus.
		if (c2TieBreak && delta == 0) {
			std::vector<BYTE> tmp(A);
			BYTE c2Raw[C2_ERROR_SIZE] = {};
			int c2Errors = 0;
			ScsiDrive::C2ReadOptions o;
			o.countBytes = false;   // need the per-byte pointer bitmap
			if (m_drive.ReadSectorWithC2Ex(lba, tmp.data(), nullptr, c2Errors,
				c2Raw, o)) {
				p.c2Opinion = true;   // c2Errors == 0 is a verdict too ("all clean")
				if (c2Errors > 0)
					for (int i = 0; i < A; i++)
						if (C2ByteFlagged(c2Raw, i)) p.c2Clean[i] = false;
			}
		}

		passes.push_back(std::move(p));

		// Vote each byte. Consensus is PRIMARY: the value with the most agreeing
		// reads across ALL passes wins. C2 only breaks a tie between values that
		// are equally agreed — among those it prefers the one more reads
		// considered clean. A byte is confirmed once its winning value reaches
		// the quorum of agreeing reads. Stop early once every byte is confirmed.
		int confirmed = 0;
		int c2Flagged = 0;
		for (int i = 0; i < A; i++) {
			std::map<BYTE, int> tally;       // all passes
			std::map<BYTE, int> cleanTally;  // C2-clean passes only (tie-break)
			int c2Opinions = 0, c2Bad = 0;   // C2 verdicts for this byte
			for (const auto& pp : passes) {
				tally[pp.audio[i]]++;
				if (pp.c2Clean[i]) cleanTally[pp.audio[i]]++;
				if (pp.c2Opinion) {
					c2Opinions++;
					if (!pp.c2Clean[i]) c2Bad++;
				}
			}

			int topCount = 0;
			for (const auto& kv : tally)
				topCount = std::max(topCount, kv.second);

			// Among values tied at topCount, prefer the highest C2-clean count
			// (ties beyond that fall to the lowest byte value, deterministically).
			BYTE topVal = 0;
			int bestClean = -1;
			for (const auto& kv : tally) {
				if (kv.second != topCount) continue;
				auto it = cleanTally.find(kv.first);
				int cleanCnt = (it != cleanTally.end()) ? it->second : 0;
				if (cleanCnt > bestClean) { bestClean = cleanCnt; topVal = kv.first; }
			}

			audioOut[i] = topVal;
			bool isConfirmed = (topCount >= cfg.quorum);
			if (isConfirmed) confirmed++;
			// Annotate: consensus accepted this byte, but C2 flagged it bad on a
			// majority of its verdicts. The drive disputes a value we kept.
			if (isConfirmed && c2Opinions > 0 && c2Bad * 2 > c2Opinions)
				c2Flagged++;
		}

		sr.confirmedBytes = confirmed;
		sr.c2FlaggedBytes = c2Flagged;
		if (confirmed == A) break;   // fully recovered — no need for more passes
	}

	sr.lba = lba;
	sr.passesUsed = passesUsed;
	sr.totalBytes = A;
	sr.maxJitterSamples = maxJit / kFrameBytes;

	if (!anySuccess) {
		sr.confirmedBytes = 0;
		sr.status = RecoverySectorStatus::Unrecovered;
		std::memset(audioOut, 0, A);   // nothing readable — leave silence
		return false;
	}
	sr.status = (sr.confirmedBytes == A)
		? RecoverySectorStatus::Recovered
		: RecoverySectorStatus::Partial;
	return true;
}

// ── Whole-disc recovery rip ─────────────────────────────────────────────────
bool OpticalDrive::ReadDiscRecovery(DiscInfo& disc, const RecoveryRipConfig& config,
	RecoveryRipResult& result, std::function<void(int, int)> progress) {

	// Lock the tray for the duration of the recovery rip (auto-unlocked on return).
	DriveDoorLockGuard doorLock(m_drive);

	if (config.maxSpeed > 0) m_drive.SetSpeed(config.maxSpeed);

	DWORD total = 0;
	for (const auto& t : disc.tracks) {
		if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;
		DWORD start = (disc.pregapMode == PregapMode::Skip) ? t.startLBA : t.pregapLBA;
		if (t.endLBA >= start) total += t.endLBA - start + 1;
	}

	result = RecoveryRipResult{};
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

	auto overallStart = std::chrono::steady_clock::now();

	// ── Phase 0: hybrid C2 gate ─────────────────────────────────────────────
	bool c2TieBreak = config.useC2TieBreak && disc.enableC2Detection;
	if (c2TieBreak) {
		std::cout << "  Validating drive C2 reliability (hybrid tie-break)... " << std::flush;
		std::vector<DWORD> probes;
		for (const auto& t : disc.tracks) {
			if (!t.isAudio) continue;
			if (t.endLBA > t.startLBA)
				probes.push_back(t.startLBA + (t.endLBA - t.startLBA) / 2);
			if (probes.size() >= 3) break;
		}
		int passed = 0;
		for (DWORD lba : probes)
			if (m_drive.ValidateC2Accuracy(lba)) passed++;
		c2TieBreak = (!probes.empty() && passed == static_cast<int>(probes.size()));
		std::cout << (c2TieBreak ? "reliable — C2 used as tie-break\n"
			: "unreliable — pure consensus\n");
	}
	else {
		std::cout << "  C2 tie-break disabled — pure consensus mode\n";
	}
	result.c2TieBreakActive = c2TieBreak;

	std::cout << "  Recovery: up to " << config.maxPasses << " passes, quorum "
		<< config.quorum << ", jitter window +/-" << config.maxJitterSamples
		<< " samples\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n" << std::flush;

	// ── Phase 1: baseline sweep ─────────────────────────────────────────────
	// One pass over the disc. Clean audio reads are trusted; failed or
	// C2-flagged audio sectors are queued for the consensus rescue. The index
	// into rawSectors is recorded so the rescue can overwrite in place.
	struct Problem { size_t index; DWORD lba; int track; bool isAudio; };
	std::vector<Problem> problems;

	DWORD cur = 0;
	if (progress) progress(0, total);

	for (const auto& t : disc.tracks) {
		if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;
		DWORD start = (disc.pregapMode == PregapMode::Skip) ? t.startLBA : t.pregapLBA;
		int sectorSize = (disc.includeSubchannel && t.isAudio) ? RAW_SECTOR_SIZE : AUDIO_SECTOR_SIZE;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey())
				return false;

			std::vector<BYTE> sec(sectorSize, 0);
			int c2Errors = 0;
			bool ok = false;

			if (t.isAudio) {
				ScsiDrive::C2ReadOptions o;
				o.countBytes = false;
				BYTE* subPtr = (sectorSize > AUDIO_SECTOR_SIZE) ? sec.data() + AUDIO_SECTOR_SIZE : nullptr;
				ok = m_drive.ReadSectorWithC2Ex(lba, sec.data(), subPtr, c2Errors, nullptr, o);
			}
			else {
				ok = m_drive.ReadDataSector(lba, sec.data());
			}

			size_t idx = disc.rawSectors.size();
			disc.rawSectors.push_back(std::move(sec));

			if (t.isAudio && (!ok || c2Errors > 0)) {
				problems.push_back({ idx, lba, t.trackNumber, true });
			}
			else if (!t.isAudio && !ok) {
				problems.push_back({ idx, lba, t.trackNumber, false });
			}
			else {
				result.cleanFirstPass++;
			}

			cur++;
			if (progress) progress(cur, total);
		}
	}

	result.problemSectors = static_cast<int>(problems.size());

	if (problems.empty()) {
		result.confidence = 100.0;
		result.assessment = "Perfect — every sector read clean on the first pass";
		result.durationSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - overallStart).count();
		std::cout << "\n  All " << result.totalSectors
			<< " sectors read clean — no recovery needed\n";
		return true;
	}

	// ── Phase 2: consensus rescue ───────────────────────────────────────────
	std::cout << "\n  Phase 2: " << problems.size()
		<< " problem sector(s) — consensus rescue\n";

	ProgressIndicator rescueProgress;
	rescueProgress.SetLabel("  Recover");
	rescueProgress.Start();

	long long confirmedTotal = 0;   // confirmed audio bytes across rescued sectors
	int rescueCur = 0;
	int rescueTotal = static_cast<int>(problems.size());

	for (const auto& pr : problems) {
		if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
			rescueProgress.Finish(false, rescueTotal);
			return false;
		}

		BYTE* audio = disc.rawSectors[pr.index].data();   // overwrite audio in place
		RecoverySectorResult sr;
		sr.lba = pr.lba;
		sr.track = pr.track;
		sr.totalBytes = AUDIO_SECTOR_SIZE;

		if (pr.isAudio) {
			// Audio sectors are rebuilt by aligned per-byte consensus.
			RescueSectorConsensus(pr.lba, audio, config, disc.leadOutLBA,
				c2TieBreak, sr);
		}
		else {
			// Data sectors (mixed-mode discs) have no audio consensus path —
			// give them a handful of plain re-reads and capture what we can.
			std::vector<BYTE> dataBuf(AUDIO_SECTOR_SIZE, 0);
			bool got = false;
			for (int retry = 0; retry < 4 && !got; retry++) {
				if (config.cacheDefeat && retry > 0) DefeatDriveCache(pr.lba, disc.leadOutLBA);
				got = m_drive.ReadDataSector(pr.lba, dataBuf.data());
				sr.passesUsed = retry + 1;
			}
			if (got) {
				std::memcpy(audio, dataBuf.data(), AUDIO_SECTOR_SIZE);
				sr.status = RecoverySectorStatus::Partial;   // captured, not consensus-verified
			}
			else {
				sr.status = RecoverySectorStatus::Unrecovered;
			}
		}

		switch (sr.status) {
		case RecoverySectorStatus::Recovered:   result.recovered++;   break;
		case RecoverySectorStatus::Partial:     result.partial++;     break;
		default:
			result.unrecovered++;
			disc.errorCount++;
			disc.badSectors.push_back(pr.lba);
			break;
		}

		confirmedTotal += sr.confirmedBytes;
		if (sr.c2FlaggedBytes > 0) {
			result.c2DisputedBytes += sr.c2FlaggedBytes;
			result.c2DisputedSectors++;
		}
		result.sectorResults.push_back(sr);

		rescueProgress.Update(++rescueCur, rescueTotal);
	}
	rescueProgress.Finish(true, rescueTotal);

	// ── Summary ─────────────────────────────────────────────────────────────
	// Confidence = fraction of audio bytes that are trustworthy: every clean
	// first-pass sector counts fully; rescued sectors count their confirmed bytes.
	long long cleanBytes = static_cast<long long>(result.cleanFirstPass) * AUDIO_SECTOR_SIZE;
	long long totalBytes = static_cast<long long>(result.totalSectors) * AUDIO_SECTOR_SIZE;
	result.confidence = (totalBytes > 0)
		? 100.0 * static_cast<double>(cleanBytes + confirmedTotal) / static_cast<double>(totalBytes)
		: 0.0;

	result.assessment =
		(result.unrecovered > 0) ? "Incomplete — some sectors were never readable" :
		(result.partial > 0) ? "Partial — some bytes could not reach consensus" :
		"Recovered — all problem sectors rebuilt by consensus";

	result.durationSeconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - overallStart).count();

	std::cout << "\n  Recovery complete: "
		<< result.cleanFirstPass << " clean, "
		<< result.recovered << " recovered, "
		<< result.partial << " partial, "
		<< result.unrecovered << " unrecovered\n";
	std::cout << "  Byte confidence: " << std::fixed << std::setprecision(3)
		<< result.confidence << "%\n";
	if (result.c2DisputedBytes > 0) {
		std::cout << "  Note: " << result.c2DisputedBytes << " byte(s) in "
			<< result.c2DisputedSectors
			<< " sector(s) were accepted by consensus but flagged by C2 "
			<< "(see report)\n";
	}

	return true;
}

// ── Recovery report ─────────────────────────────────────────────────────────
bool OpticalDrive::SaveRecoveryReport(const RecoveryRipResult& result,
	const std::wstring& filename) {
	std::ofstream out(std::filesystem::path(filename), std::ios::out | std::ios::trunc);
	if (!out) return false;

	out << "# ==============================\n";
	out << "# Drive-Independent Recovery Rip Report\n";
	out << "# ==============================\n";
	out << "#\n";
	out << "# Assessment:        " << result.assessment << "\n";
	out << "# C2 tie-break:      " << (result.c2TieBreakActive
		? "ACTIVE (drive C2 validated)" : "off (pure consensus)") << "\n";
	out << "# Total sectors:     " << result.totalSectors << "\n";
	out << "# Clean first pass:  " << result.cleanFirstPass << "\n";
	out << "# Problem sectors:   " << result.problemSectors << "\n";
	out << "#   Recovered:       " << result.recovered << "\n";
	out << "#   Partial:         " << result.partial << "\n";
	out << "#   Unrecovered:     " << result.unrecovered << "\n";
	out << "# Byte confidence:   " << std::fixed << std::setprecision(3)
		<< result.confidence << "%\n";
	out << "# C2-disputed bytes: " << result.c2DisputedBytes
		<< " (in " << result.c2DisputedSectors << " sector(s))\n";
	out << "# Duration:          " << std::fixed << std::setprecision(1)
		<< result.durationSeconds << "s\n";
	out << "#\n";
	out << "# Note: partial/unrecovered bytes are best-effort majority values.\n";
	out << "# Audio reads cannot recover a sample that every pass misread the\n";
	out << "# same way; those positions are reported here, not silently faked.\n";
	out << "#\n";
	out << "# C2FlaggedBytes: bytes consensus confirmed but the drive's C2 flagged\n";
	out << "# bad on a majority of reads (consensus overrode C2). Only meaningful\n";
	out << "# when C2 tie-break was active; the kept value is consensus's best\n";
	out << "# guess, but the drive considered that sample uncorrectable.\n";
	out << "#\n";

	if (result.sectorResults.empty()) {
		out << "# No problem sectors — the disc read clean.\n";
		out.flush();
		return out.good();
	}

	auto statusName = [](RecoverySectorStatus s) -> const char* {
		switch (s) {
		case RecoverySectorStatus::Recovered:   return "RECOVERED";
		case RecoverySectorStatus::Partial:     return "PARTIAL";
		case RecoverySectorStatus::Unrecovered: return "UNRECOVERED";
		default:                                return "CLEAN";
		}
	};

	out << "LBA,Track,Status,Passes,ConfirmedBytes,TotalBytes,MaxJitterSamples,C2FlaggedBytes\n";
	for (const auto& s : result.sectorResults) {
		out << s.lba << "," << s.track << "," << statusName(s.status) << ","
			<< s.passesUsed << "," << s.confirmedBytes << "," << s.totalBytes << ","
			<< s.maxJitterSamples << "," << s.c2FlaggedBytes << "\n";
	}

	out.flush();
	return out.good();
}
