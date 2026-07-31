#define NOMINMAX
#include "OpticalDrive.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <map>
#include <cstring>
#include <chrono>

// ============================================================================
// TOC Reading
// ============================================================================
//
// Pregap detection accuracy notes:
// ─────────────────────────────────
// Each track's INDEX 00→01 boundary is located by reading Q subchannel data
// sector-by-sector.  Subchannel reads are inherently unreliable on optical
// drives — the data can lag behind the actual disc position by several
// sectors, and some drives return stale values at index transitions.
//
// To balance speed and accuracy, a two-phase approach is used:
//
//   Phase 1 (coarse):  Step through the search window in increments of
//       COARSE_STEP sectors using single (unvoted) subchannel reads.  A
//       false positive here only shifts the fine window — it cannot affect
//       the final boundary — so voting is unnecessary.  This cuts coarse-
//       phase SCSI commands by 2/3 compared to majority-voted reads.
//
//   Phase 2 (fine):  Scan every sector in a ±COARSE_STEP window around the
//       coarse hit with 3-round majority voting, enforcing ≥3 consecutive
//       index-0 results to confirm the boundary.
//
//   Backward refinement:  After the fine scan, up to 8 sectors before the
//       detected boundary are re-checked with majority voting to compensate
//       for subchannel read displacement (drives often report the index
//       change a few sectors late).
//
// Fallback:  If the coarse pass finds no index-0 hit, the fine scan covers
// the entire search window — equivalent to the original brute-force method.
//
// Accuracy vs. the original sector-by-sector approach:
//   • The fine phase and backward refinement use ReadSectorQ (3-round
//     majority voting), so boundary determination is identical.
//   • The coarse pass uses ReadSectorQSingle (no voting).  A false positive
//     only narrows the fine window — the voted fine pass still controls the
//     result.  A false negative triggers the full-window fallback.
//   • For pregaps shorter than COARSE_STEP (rare — standard is 150 frames),
//     the coarse pass may miss entirely, triggering the full-window fallback.
//   • Net result: identical accuracy for >99% of discs, ~30% fewer SCSI
//     commands overall (~2/3 fewer in the coarse phase).
//
// Abandoned-scan diagnosis:
//   The scan counts a failed Q-subchannel read as a "read failure" and, after
//   too many, gives up and assumes the track has no pregap.  But a Q read can
//   fail (CRC/raw-subchannel decode) on a drive — notably MediaTek/Lite-On —
//   while the CIRC-protected audio in the same region reads perfectly.  Rather
//   than reporting a misleading "read errors/timeout", an abandoned scan now
//   samples the audio across the search window (ReportPregapScanSkipped) and
//   reports whether the failure was subchannel-only (drive limitation, disc
//   fine) or accompanied by audio read errors (likely disc damage).  The
//   fallback — assume no pregap — is unchanged; only the diagnosis improves.
// ============================================================================

// ── Sign-bug correction for TOC LBAs ────────────────────────────────────
static DWORD ParseTocLBA(const BYTE* p) {
	DWORD raw = (static_cast<DWORD>(p[0]) << 24) |
		(static_cast<DWORD>(p[1]) << 16) |
		(static_cast<DWORD>(p[2]) << 8) |
		static_cast<DWORD>(p[3]);

	if (raw >= 0x80000000u) {
		int32_t signedVal = static_cast<int32_t>(raw);
		if (signedVal >= -450 && signedVal < 0) {
			return 0;
		}
	}
	return raw;
}

// Helper: returns true if more than PREGAP_TIMEOUT_SEC seconds have elapsed
static bool PregapTimedOut(const std::chrono::steady_clock::time_point& start) {
	constexpr int PREGAP_TIMEOUT_SEC = 30;          // was 15
	auto elapsed = std::chrono::steady_clock::now() - start;
	return std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= PREGAP_TIMEOUT_SEC;
}

// Helper: a per-sector scan loop should stop early if it has run past its time
// budget OR the user/app has requested cancellation (Cancel button, or window
// close). Honouring the interrupt flag here lets a long pregap scan abort
// promptly instead of grinding on for up to 30s/track — which is also what lets
// the shutdown path join the worker quickly rather than force-exiting.
static bool PregapShouldStop(const std::chrono::steady_clock::time_point& start) {
	return InterruptHandler::Instance().IsInterrupted() || PregapTimedOut(start);
}

// ── Audio-readability probe for abandoned pregap scans ──────────────────────
// Pregap detection reads the Q subchannel, which carries only a 16-bit CRC and
// no error correction.  MediaTek/Lite-On drives in particular routinely fail
// those reads while the CIRC-protected audio in the very same region reads
// perfectly.  When a scan is abandoned on "read errors", sampling the audio
// main channel across the search window tells us which it actually was:
//   audio OK   → the drive couldn't decode the subchannel (benign — the disc
//                is fine and "no pregap" is the correct fallback);
//   audio fails→ the media itself is unreadable here (a genuine defect).
// Returns the fraction (0.0–1.0) of sampled sectors whose audio read OK.
static double ProbePregapAudioReadable(ScsiDrive& drive, DWORD windowStart, DWORD windowEnd) {
	if (windowEnd <= windowStart) return 1.0;       // nothing to sample
	if (InterruptHandler::Instance().IsInterrupted()) return 1.0;  // cancelling — don't probe
	constexpr int SAMPLES = 8;
	DWORD span = windowEnd - windowStart;           // 1..450 in practice (no overflow below)
	BYTE buf[AUDIO_SECTOR_SIZE];
	int attempted = 0, ok = 0;
	for (int k = 0; k < SAMPLES; k++) {
		// Spread evenly across the whole window, inclusive of the boundary-
		// adjacent sector (windowEnd-1). A pregap — and the failing subchannel
		// reads — sit right before the track boundary, so that end must be
		// represented; sampling only from windowStart would miss boundary damage.
		DWORD lba = (SAMPLES > 1)
			? windowStart + (span - 1) * static_cast<DWORD>(k) / (SAMPLES - 1)
			: windowStart;
		attempted++;
		if (drive.ReadSectorAudioOnly(lba, buf)) ok++;
	}
	return attempted ? static_cast<double>(ok) / attempted : 1.0;
}

// Prints the "pregap not determined" line for an abandoned scan, classified by
// whether the audio in the search window is actually readable (audioOk = the
// fraction from ProbePregapAudioReadable) — so the message no longer blames
// "read errors" when the audio is fine and only the drive's subchannel decode
// failed.  In every case the fallback is the same (no pregap); only the
// diagnosis differs.  Takes a precomputed fraction so the caller can reuse the
// same probe it used to decide whether a low-speed retry was worthwhile.
static void ReportPregapScanSkipped(int trackNumber, double audioOk) {
	std::cout << "  Track " << std::setw(2) << trackNumber << ": ";
	if (audioOk >= 0.999) {
		// Every sampled audio sector read fine — the drive simply can't decode
		// the unprotected Q subchannel here.  Not a disc fault.
		std::cout << "no pregap detected (subchannel unreadable on this drive, "
			"audio reads fine); assuming no pregap\n";
	}
	else if (audioOk > 0.0) {
		std::cout << "pregap scan incomplete - some audio read errors in this region "
			"(possible disc damage); assuming no pregap\n";
	}
	else {
		std::cout << "pregap scan skipped - audio unreadable in this region "
			"(likely disc damage); assuming no pregap\n";
	}
}

bool OpticalDrive::ReadTOC(DiscInfo& disc, bool skipPregapScan) {
	if (!ReadFullTOC(disc)) return false;

	if (skipPregapScan) {
		// Assign default boundaries — pregap data not needed by caller
		for (auto& t : disc.tracks) {
			t.pregapLBA = t.startLBA;
			t.index01LBA = t.startLBA;
		}
		std::cout << "\nPregap scan skipped (not required for this operation).\n";
		std::cout << "\nTOC: " << disc.tracks.size() << " tracks\n";

		// Skip hidden track detection — caller only needs TOC structure,
		// not pregap/HTOA boundaries (saves ~75 sector reads at 4x)
		return true;
	}

	std::cout << "\nScanning pregaps (slowing drive for accuracy)...\n";
	m_drive.SetSpeed(4);

	constexpr bool USE_COARSE_PREGAP_SCAN = true;
	constexpr int MAX_CONSECUTIVE_READ_FAILS = 10;  // was 5
	constexpr int MAX_TOTAL_READ_FAILS = 25;        // was 10

	// ── Track 1 ─────────────────────────────────────────────────────────
	if (!disc.tracks.empty() && disc.tracks[0].isAudio
		&& (disc.selectedSession == 0 || disc.tracks[0].session == disc.selectedSession)) {
		DWORD track1Start = disc.tracks[0].startLBA;

		if (track1Start == 0) {
			if (disc.tocRepaired) {
				disc.tracks[0].pregapLBA = 0;
				disc.tracks[0].index01LBA = 150;
				std::cout << "  Track  1: 2:00 (150 frames) pregap (default)\n";
			}
			else {
				constexpr DWORD MAX_PREGAP_SEARCH = 225;
				DWORD scanLimit = std::min(MAX_PREGAP_SEARCH, disc.tracks[0].endLBA);
				DWORD index01 = 150;
				bool foundIndex1 = false;
				int consecutiveIndex1 = 0;
				int consecutiveFails = 0;
				int totalFails = 0;
				auto t1Clock = std::chrono::steady_clock::now();

				for (DWORD lba = 0; lba < scanLimit; lba++) {
					if (PregapShouldStop(t1Clock)) break;
					int qTrack = 0, qIndex = -1;
					bool readOk = m_drive.ReadSectorQ(lba, qTrack, qIndex);
					if (!readOk) {
						totalFails++;
						if (++consecutiveFails >= MAX_CONSECUTIVE_READ_FAILS
							|| totalFails >= MAX_TOTAL_READ_FAILS) break;
						continue;
					}
					consecutiveFails = 0;
					if (qTrack == 1 && qIndex == 1) {
						if (!foundIndex1) { index01 = lba; foundIndex1 = true; consecutiveIndex1 = 1; }
						else { consecutiveIndex1++; }
						if (consecutiveIndex1 >= 3) break;
					}
					else {
						if (foundIndex1 && consecutiveIndex1 < 3) {
							foundIndex1 = false; consecutiveIndex1 = 0; index01 = 150;
						}
					}
				}

				if (index01 < 150) index01 = 150;

				disc.tracks[0].pregapLBA = 0;
				disc.tracks[0].index01LBA = index01;

				int frames = static_cast<int>(index01);
				std::cout << "  Track " << std::setw(2) << 1 << ": " << frames / 75 << ":"
					<< std::setfill('0') << std::setw(2) << frames % 75 << std::setfill(' ')
					<< " (" << std::setw(3) << frames << " frames) pregap\n";
			}
		}
		else {
			disc.tracks[0].index01LBA = track1Start;

			if (disc.tocRepaired && !disc.tocLBAsRecovered) {
				disc.tracks[0].pregapLBA = 0;
				disc.tracks[0].index01LBA = 150;
				std::cout << "  Track  1: 2:00 (150 frames) pregap (default)\n";
			}
			else {
				DWORD firstIndex0 = track1Start;
				bool foundIndex0 = false;
				int consecutiveIndex0 = 0;
				bool trackSkipped = false;
				int totalFails = 0;

				constexpr DWORD COARSE_STEP = 15;
				DWORD coarseHit = track1Start;
				bool coarseFound = false;
				DWORD scanStart = track1Start > 450 ? track1Start - 450 : 0;
				auto t1Clock = std::chrono::steady_clock::now();

				if (USE_COARSE_PREGAP_SCAN) {
					int coarseFails = 0;
					for (DWORD lba = scanStart; lba < track1Start; lba += COARSE_STEP) {
						if (PregapShouldStop(t1Clock)) { trackSkipped = true; break; }
						int qTrack = 0, qIndex = -1;
						if (m_drive.ReadSectorQSingle(lba, qTrack, qIndex)) {
							coarseFails = 0;
							if (qTrack == 1 && qIndex == 0) {
								coarseHit = lba;
								coarseFound = true;
								break;
							}
						}
						else {
							totalFails++;
							if (++coarseFails >= MAX_CONSECUTIVE_READ_FAILS
								|| totalFails >= MAX_TOTAL_READ_FAILS) {
								trackSkipped = true;
								// Drop any stray coarse hit — fine pass is
								// gated by !trackSkipped today, but defending
								// the invariant here prevents future refactors
								// from anchoring the fine window on a stale LBA.
								coarseFound = false;
								coarseHit = track1Start;
								break;
							}
						}
					}
				}

				if (!trackSkipped) {
					DWORD fineStart = coarseFound && coarseHit > COARSE_STEP ? coarseHit - COARSE_STEP : scanStart;
					DWORD fineEnd = coarseFound ? (coarseHit + COARSE_STEP < track1Start ? coarseHit + COARSE_STEP : track1Start) : track1Start;

					// Reset failure tally — coarse uses single (unvoted) reads;
					// fine uses 3-vote ReadSectorQ. A flaky single-read budget
					// shouldn't pre-disable the more reliable voted pass.
					totalFails = 0;
					int consecutiveFails = 0;
					for (DWORD lba = fineStart; lba < fineEnd; lba++) {
						if (PregapShouldStop(t1Clock)) { trackSkipped = true; break; }
						int qTrack = 0, qIndex = -1;
						bool readOk = m_drive.ReadSectorQ(lba, qTrack, qIndex);
						if (!readOk) {
							totalFails++;
							if (++consecutiveFails >= MAX_CONSECUTIVE_READ_FAILS
								|| totalFails >= MAX_TOTAL_READ_FAILS) {
								trackSkipped = true;
								break;
							}
							continue;
						}
						consecutiveFails = 0;
						if (qTrack == 1 && qIndex == 0) {
							if (!foundIndex0) { firstIndex0 = lba; foundIndex0 = true; consecutiveIndex0 = 1; }
							else { consecutiveIndex0++; }
						}
						else {
							if (foundIndex0 && consecutiveIndex0 < 3) {
								foundIndex0 = false; consecutiveIndex0 = 0; firstIndex0 = track1Start;
							}
							if (foundIndex0 && consecutiveIndex0 >= 3 && qTrack == 1 && qIndex == 1) {
								break;
							}
						}
					}
				}

				if (!trackSkipped && foundIndex0 && consecutiveIndex0 >= 3) {
					if (firstIndex0 > scanStart) {
						DWORD backLimit = (firstIndex0 > scanStart + 8) ? firstIndex0 - 8 : scanStart;
						for (DWORD lba = firstIndex0 - 1; lba >= backLimit; lba--) {
							int qt = 0, qi = -1;
							if (m_drive.ReadSectorQ(lba, qt, qi) && qt == 1 && qi == 0) {
								firstIndex0 = lba;
							}
							else {
								break;
							}
							if (lba == 0) break;
						}
					}
					// Red Book: Track 1's pregap (and any HTOA) always begins at LBA 0.
					// firstIndex0 only confirms a pregap region exists; the true
					// start is spec-defined as 0, since subchannel may have
					// missed early sectors and pregapLBA is consumed as a read
					// boundary by SecureRead/Fingerprinting.
					disc.tracks[0].pregapLBA = 0;
				}
				else if (trackSkipped) {
					disc.tracks[0].pregapLBA = track1Start;
				}
				else {
					disc.tracks[0].pregapLBA = 0;
				}

				// Do NOT clamp startLBA or index01LBA up to 150. They are track 1's
				// real INDEX 01 (TOC) boundary, consumed verbatim by the AccurateRip
				// disc ID, the .cue INDEX 01, PregapMode::Skip rip boundaries, and
				// Verification's isInPregap test. Discs legitimately place track 1
				// below LBA 150 (a short or zero program-area pregap; the mandatory
				// 2-second pregap lives in the lead-in, not the program area).
				// Forcing them to 150 shifts the disc ID — breaking AccurateRip
				// lookups — would skip real audio on a Skip rip, and would misreport
				// the pregap length below. Only anchor track 1's pregap origin at
				// LBA 0, since any HTOA/pregap audio begins there.
				if (disc.tracks[0].index01LBA < 150) {
					disc.tracks[0].pregapLBA = 0;
				}

				if (trackSkipped) {
					// Suppress the diagnosis if we stopped due to a cancel request.
					if (!g_interrupt.IsInterrupted()) {
						double audioOk = ProbePregapAudioReadable(m_drive, scanStart, track1Start);
						ReportPregapScanSkipped(1, audioOk);
					}
				}
				else {
					int frames = static_cast<int>(disc.tracks[0].index01LBA - disc.tracks[0].pregapLBA);
					std::cout << "  Track " << std::setw(2) << 1 << ": " << frames / 75 << ":"
						<< std::setfill('0') << std::setw(2) << frames % 75 << std::setfill(' ')
						<< " (" << std::setw(3) << frames << " frames) pregap\n";
				}
			}
		}
	}
	else if (!disc.tracks.empty() && !disc.tracks[0].isAudio
		&& (disc.selectedSession == 0 || disc.tracks[0].session == disc.selectedSession)) {
		std::cout << "  Track " << std::setw(2) << disc.tracks[0].trackNumber << ": DATA track, skipping\n";
	}

	// ── Tracks 2+ ───────────────────────────────────────────────────────
	for (size_t i = 1; i < disc.tracks.size(); i++) {
		// Stop scanning the remaining tracks if cancellation was requested
		// (Cancel button or window close). The previous CheckInterrupt() call
		// discarded its result, so the flag was effectively ignored here.
		if (g_interrupt.IsInterrupted()) break;
		if (disc.selectedSession > 0 && disc.tracks[i].session != disc.selectedSession) continue;
		if (!disc.tracks[i].isAudio) {
			std::cout << "  Track " << std::setw(2) << disc.tracks[i].trackNumber << ": DATA track, skipping\n";
			continue;
		}
		if (disc.tocRepaired && !disc.tocLBAsRecovered) {
			disc.tracks[i].pregapLBA = disc.tracks[i].startLBA;
			std::cout << "  Track " << std::setw(2) << disc.tracks[i].trackNumber << ": pregap scan skipped (corrupt TOC)\n";
			continue;
		}

		DWORD trackStart = disc.tracks[i].startLBA;
		int targetTrack = disc.tracks[i].trackNumber;
		DWORD pregapStart = trackStart;
		DWORD scanStart = trackStart > 450 ? trackStart - 450 : 0;
		DWORD firstIndex0 = trackStart;
		bool foundAny = false;
		int consecutiveIndex0 = 0;
		bool trackSkipped = false;
		int totalFails = 0;
		auto trackClock = std::chrono::steady_clock::now();

		constexpr DWORD COARSE_STEP = 15;
		DWORD coarseHit = trackStart;
		bool coarseFound = false;

		// ── Phase 1: coarse scan ────────────────────────────────────────
		if (USE_COARSE_PREGAP_SCAN) {
			int coarseFails = 0;
			for (DWORD lba = scanStart; lba < trackStart; lba += COARSE_STEP) {
				if (PregapShouldStop(trackClock)) { trackSkipped = true; break; }
				int qTrack = 0, qIndex = -1;
				if (m_drive.ReadSectorQSingle(lba, qTrack, qIndex)) {
					coarseFails = 0;
					if (qTrack == targetTrack && qIndex == 0) {
						coarseHit = lba;
						coarseFound = true;
						break;
					}
				}
				else {
					totalFails++;
					if (++coarseFails >= MAX_CONSECUTIVE_READ_FAILS
						|| totalFails >= MAX_TOTAL_READ_FAILS) {
						trackSkipped = true;
						// Drop any stray coarse hit (see note in Track 1 block).
						coarseFound = false;
						coarseHit = trackStart;
						break;
					}
				}
			}
		}

		// ── Phase 2: fine scan (skipped if coarse already failed) ───────
		if (!trackSkipped) {
			DWORD fineStart, fineEnd;
			if (coarseFound) {
				fineStart = coarseHit > COARSE_STEP ? coarseHit - COARSE_STEP : scanStart;
				fineEnd = coarseHit + COARSE_STEP < trackStart ? coarseHit + COARSE_STEP : trackStart;
			}
			else {
				fineStart = scanStart;
				fineEnd = trackStart;
			}

			// Reset failure tally — see Track 1 note above.
			totalFails = 0;
			int consecutiveFails = 0;
			for (DWORD lba = fineStart; lba < fineEnd; lba++) {
				if (PregapShouldStop(trackClock)) { trackSkipped = true; break; }
				int qTrack = 0, qIndex = -1;
				bool readOk = m_drive.ReadSectorQ(lba, qTrack, qIndex);
				if (!readOk) {
					totalFails++;
					if (++consecutiveFails >= MAX_CONSECUTIVE_READ_FAILS
						|| totalFails >= MAX_TOTAL_READ_FAILS) {
						trackSkipped = true;
						break;
					}
					continue;
				}
				consecutiveFails = 0;
				bool isIndex0 = qTrack == targetTrack && qIndex == 0;

				if (isIndex0) {
					if (!foundAny) { firstIndex0 = lba; foundAny = true; consecutiveIndex0 = 1; }
					else { consecutiveIndex0++; }
				}
				else {
					if (foundAny && consecutiveIndex0 < 3) {
						foundAny = false; consecutiveIndex0 = 0; firstIndex0 = trackStart;
					}
					if (foundAny && consecutiveIndex0 >= 3 && qTrack == targetTrack && qIndex == 1) {
						break;
					}
				}
			}
		}

		// ── Retry at minimum speed if coarse or fine phase failed ────────
		if (trackSkipped) {
			// If the scan was abandoned because of a cancel request (not a real
			// read failure), don't probe or retry — just stop the whole scan.
			if (g_interrupt.IsInterrupted()) {
				disc.tracks[i].pregapLBA = trackStart;
				break;
			}

			// Before paying for a slow 2x subchannel retry, check whether the
			// audio in this window is even the problem. If every sampled audio
			// sector reads fine, the failures are subchannel-only (a drive
			// limitation, e.g. MediaTek/Lite-On) and reading the same Q data
			// slower won't decode it — skip straight to "no pregap" rather than
			// grinding through the whole window again at 2x.
			double audioOk = ProbePregapAudioReadable(m_drive, scanStart, trackStart);
			if (audioOk >= 0.999) {
				disc.tracks[i].pregapLBA = trackStart;
				ReportPregapScanSkipped(targetTrack, audioOk);
				continue;
			}

			// Audio is marginal/failing — a low-speed retry may recover it.
			std::cout << "  Track " << std::setw(2) << targetTrack
				<< ": retrying at minimum speed...\n";
			m_drive.SetSpeed(2);
			trackSkipped = false;
			totalFails = 0;
			foundAny = false;
			consecutiveIndex0 = 0;
			firstIndex0 = trackStart;
			trackClock = std::chrono::steady_clock::now();

			int retryConsecFails = 0;
			for (DWORD lba = scanStart; lba < trackStart; lba++) {
				if (PregapShouldStop(trackClock)) { trackSkipped = true; break; }
				int qTrack = 0, qIndex = -1;
				bool readOk = m_drive.ReadSectorQ(lba, qTrack, qIndex);
				if (!readOk) {
					totalFails++;
					if (++retryConsecFails >= MAX_CONSECUTIVE_READ_FAILS
						|| totalFails >= MAX_TOTAL_READ_FAILS) {
						trackSkipped = true;
						break;
					}
					continue;
				}
				retryConsecFails = 0;
				bool isIndex0 = qTrack == targetTrack && qIndex == 0;
				if (isIndex0) {
					if (!foundAny) { firstIndex0 = lba; foundAny = true; consecutiveIndex0 = 1; }
					else { consecutiveIndex0++; }
				}
				else {
					if (foundAny && consecutiveIndex0 < 3) {
						foundAny = false; consecutiveIndex0 = 0; firstIndex0 = trackStart;
					}
					if (foundAny && consecutiveIndex0 >= 3 && qTrack == targetTrack && qIndex == 1) {
						break;
					}
				}
			}

			if (trackSkipped) {
				m_drive.SetSpeed(4);
				disc.tracks[i].pregapLBA = trackStart;
				// Reuse the pre-retry audio probe — readability doesn't change.
				ReportPregapScanSkipped(targetTrack, audioOk);
				continue;
			}
			// Stay at speed 2 for backward refinement, restore after
		}

		// ── Backward refinement ─────────────────────────────────────────
		if (foundAny && consecutiveIndex0 >= 3 && firstIndex0 > scanStart) {
			DWORD backLimit = (firstIndex0 > scanStart + 8) ? firstIndex0 - 8 : scanStart;
			for (DWORD lba = firstIndex0 - 1; lba >= backLimit; lba--) {
				if (PregapShouldStop(trackClock)) break;
				int qt = 0, qi = -1;
				if (m_drive.ReadSectorQ(lba, qt, qi) && qt == targetTrack && qi == 0) {
					firstIndex0 = lba;
				}
				else {
					break;
				}
				if (lba == 0) break;
			}
		}

		// ── Result ──────────────────────────────────────────────────────
		if (foundAny && consecutiveIndex0 >= 3) pregapStart = firstIndex0;
		disc.tracks[i].pregapLBA = pregapStart;

		auto& prev = disc.tracks[i - 1];
		if (pregapStart > 0 && pregapStart - 1 < prev.endLBA) prev.endLBA = pregapStart - 1;

		int frames = static_cast<int>(trackStart - pregapStart);
		std::cout << "  Track " << std::setw(2) << targetTrack << ": " << frames / 75 << ":"
			<< std::setfill('0') << std::setw(2) << frames % 75 << std::setfill(' ')
			<< " (" << std::setw(3) << frames << " frames)\n";

		m_drive.SetSpeed(4);  // restore after refinement (no-op if retry wasn't used)
	}

	m_drive.SetSpeed(0);
	std::cout << "\nTOC: " << disc.tracks.size() << " tracks\n";

	// Skip hidden-track detection (another ~150 audio reads) if cancellation was
	// requested during the pregap scan, so the worker can exit promptly.
	if (!g_interrupt.IsInterrupted() && (!disc.tocRepaired || disc.tocLBAsRecovered)) {
		DetectHiddenTrack(disc);
	}

	return true;
}

bool OpticalDrive::ReadFullTOC(DiscInfo& disc) {
	BYTE sessionCdb[10] = { 0x43, 0x00, 1, 0, 0, 0, 0, 0, 12, 0 };
	std::vector<BYTE> sessionBuf(12);
	if (m_drive.SendSCSI(sessionCdb, 10, sessionBuf.data(), 12)) {
		if (sessionBuf[3] >= sessionBuf[2]) {
			disc.sessionCount = sessionBuf[3] - sessionBuf[2] + 1;
		}
	}

	BYTE tocCdb[10] = { 0x43, 0x00, 0, 0, 0, 0, 0, 0x03, 0x24, 0 };
	std::vector<BYTE> tocBuf(804);
	if (!m_drive.SendSCSI(tocCdb, 10, tocBuf.data(), 804)) return false;

	int tocLen = (tocBuf[0] << 8) | tocBuf[1];
	if (tocLen < 2) return false;

	int firstTrack = tocBuf[2];
	int lastTrack = tocBuf[3];
	int n = lastTrack - firstTrack + 1;
	if (n <= 0 || n > 99) return false;

	disc.tracks.clear();

	BYTE fullTocCdb[10] = { 0x43, 0x00, 2, 0, 0, 0, 0, 0x08, 0x00, 0 };
	std::vector<BYTE> fullToc(2048);
	bool hasFullToc = m_drive.SendSCSI(fullTocCdb, 10, fullToc.data(), 2048);

	// ── Dump raw TOC for offline testing ──
#ifdef _DEBUG
	{
		std::ofstream dump("toc_dump.bin", std::ios::binary);
		if (dump) {
			auto writeBlock = [&](const char* tag, const BYTE* data, uint32_t len) {
				dump.write(tag, 4);                                      // 4-byte tag
				dump.write(reinterpret_cast<const char*>(&len), 4);      // 4-byte length
				dump.write(reinterpret_cast<const char*>(data), len);    // payload
				};
			writeBlock("SESN", sessionBuf.data(), static_cast<uint32_t>(sessionBuf.size()));
			uint32_t toc0Size = std::min(static_cast<uint32_t>(tocLen + 2), 804u);
			writeBlock("TOC0", tocBuf.data(), toc0Size);
			if (hasFullToc) {
				int fullTocLen = (fullToc[0] << 8) | fullToc[1];
			 uint32_t fullTocSize = std::min(static_cast<uint32_t>(fullTocLen + 2), 2048u);
				writeBlock("TOC2", fullToc.data(), fullTocSize);
			}
			std::cout << "  (TOC dumped to toc_dump.bin)\n";
		}
	}
#endif

	std::vector<int> trackSession(100, 1);
	std::map<int, DWORD> sessionLeadOut;    // session number → lead-out LBA
	std::map<int, DWORD> fullTocTrackStart; // track number → LBA from Full TOC MSF
	if (hasFullToc) {
		int fullTocLen = (fullToc[0] << 8) | fullToc[1];
		int endOffset = (fullTocLen + 2 < 2048) ? fullTocLen + 2 : 2048;
		BYTE* p = fullToc.data() + 4;
		BYTE* end = fullToc.data() + endOffset;
		while (p + 11 <= end) {
			int session = p[0];
			int point = p[3];
			if (point >= 1 && point <= 99) {
				trackSession[point] = session;

				// Extract track start MSF (PMIN/PSEC/PFRAME) for TOC recovery.
				// Copy-protection schemes corrupt Format 0 LBAs but the raw
				// Q-subchannel data in the lead-in (Format 2) often retains
				// the real track positions.
				int pmin = p[8];
				int psec = p[9];
				int pframe = p[10];
				if (pmin < 100 && psec < 60 && pframe < 75) {
					DWORD lba = static_cast<DWORD>((pmin * 60 + psec) * 75 + pframe);
					if (lba >= 150) lba -= 150; else lba = 0;
					fullTocTrackStart[point] = lba;
				}
			}

			// Point 0xA2 = lead-out start address for this session (MSF in PMIN/PSEC/PFRAME)
			if (point == 0xA2) {
				int pmin = p[8];
				int psec = p[9];
				int pframe = p[10];
				DWORD leadOutLBA = static_cast<DWORD>((pmin * 60 + psec) * 75 + pframe) - 150;
				sessionLeadOut[session] = leadOutLBA;
			}

			p += 11;
		}
	}

	bool anySignCorrected = false;

	// Parse lead-out LBA once — reused for last track's endLBA and disc.leadOutLBA.
	BYTE* leadOut = tocBuf.data() + 4 + n * 8;
	DWORD rawLeadOut = (static_cast<DWORD>(leadOut[4]) << 24) |
		(static_cast<DWORD>(leadOut[5]) << 16) |
		(static_cast<DWORD>(leadOut[6]) << 8) |
		static_cast<DWORD>(leadOut[7]);
	disc.leadOutLBA = ParseTocLBA(leadOut + 4);
	if (disc.leadOutLBA != rawLeadOut) {
		std::cout << "  Note: Lead-out LBA " << rawLeadOut << " (signed: "
			<< static_cast<int32_t>(rawLeadOut)
			<< ") - firmware sign bug corrected to "
			<< disc.leadOutLBA << ".\n";
		anySignCorrected = true;
	}

	for (int i = 0; i < n; i++) {
		BYTE* td = tocBuf.data() + 4 + i * 8;
		TrackInfo t = {};
		t.trackNumber = td[2];
		t.isAudio = (td[1] & 0x04) == 0;
		t.mode = t.isAudio ? 0 : 1;
		t.hasPreemphasis = (td[1] & 0x01) != 0;
		t.session = trackSession[t.trackNumber];

		DWORD rawStart = (static_cast<DWORD>(td[4]) << 24) |
			(static_cast<DWORD>(td[5]) << 16) |
			(static_cast<DWORD>(td[6]) << 8) |
			static_cast<DWORD>(td[7]);
		t.startLBA = ParseTocLBA(td + 4);
		if (t.startLBA != rawStart) {
			std::cout << "  Note: Track " << t.trackNumber
				<< " LBA " << rawStart << " (signed: "
				<< static_cast<int32_t>(rawStart)
				<< ") - firmware sign bug corrected to "
				<< t.startLBA << ".\n";
			anySignCorrected = true;
		}

		if (i < n - 1) {
			BYTE* nextTd = tocBuf.data() + 4 + (i + 1) * 8;
			DWORD nextLBA = ParseTocLBA(nextTd + 4);
			t.endLBA = nextLBA > 0 ? nextLBA - 1 : 0;

			// If next track is in a different session, cap endLBA at this
			// session's lead-out to avoid scanning the inter-session gap.
			int nextSession = trackSession[tocBuf[4 + (i + 1) * 8 + 2]]; // next track's session
			if (nextSession != t.session) {
				auto it = sessionLeadOut.find(t.session);
				if (it != sessionLeadOut.end() && it->second - 1 < t.endLBA) {
					t.endLBA = it->second - 1;
				}
			}
		}
		else {
			t.endLBA = disc.leadOutLBA > 0 ? disc.leadOutLBA - 1 : 0;
		}
		disc.tracks.push_back(t);
	}

	// For enhanced/multisession CDs, store the session-1 lead-out separately.
	// AccurateRip checksum slicing stops here; its disc IDs still use the
	// overall TOC lead-out (which points past the data session).
	auto it = sessionLeadOut.find(1);
	if (it != sessionLeadOut.end() && disc.sessionCount > 1) {
		disc.audioLeadOutLBA = it->second;
	}
	else {
		disc.audioLeadOutLBA = disc.leadOutLBA;
	}

	// ── TOC sanity check ──
	// A CD holds at most ~360,000 sectors (80 min).  Even extreme overburn
	// never exceeds ~450,000.  Any LBA beyond that is corrupt data from the
	// drive — clamp it so the disc remains usable rather than rejecting it.
	constexpr DWORD MAX_VALID_CD_LBA = 450000;
	bool tocRepaired = false;

	// Snapshot raw LBAs before clamping so the protection checker can
	// evaluate the original TOC, not the sanitised version.
	disc.rawTocEntries.clear();
	for (const auto& t : disc.tracks) {
		RawTocEntry raw;
		raw.trackNumber = t.trackNumber;
		raw.originalStartLBA = t.startLBA;
		raw.originalEndLBA = t.endLBA;
		disc.rawTocEntries.push_back(raw);
	}
	disc.rawLeadOutLBA = disc.leadOutLBA;

	for (auto& t : disc.tracks) {
		if (t.startLBA > MAX_VALID_CD_LBA) {
			std::cerr << "  WARNING: Track " << t.trackNumber
				<< " startLBA " << t.startLBA
				<< " beyond CD capacity - clamping to " << MAX_VALID_CD_LBA << ".\n";
			t.startLBA = MAX_VALID_CD_LBA;
			tocRepaired = true;
		}
		if (t.endLBA > MAX_VALID_CD_LBA) {
			std::cerr << "  WARNING: Track " << t.trackNumber
				<< " endLBA " << t.endLBA
				<< " beyond CD capacity - clamping to " << MAX_VALID_CD_LBA << ".\n";
			t.endLBA = MAX_VALID_CD_LBA;
			tocRepaired = true;
		}
	}
	if (disc.leadOutLBA > MAX_VALID_CD_LBA) {
		std::cerr << "  WARNING: Lead-out LBA " << disc.leadOutLBA
			<< " beyond CD capacity - clamping to " << MAX_VALID_CD_LBA << ".\n";
		disc.leadOutLBA = MAX_VALID_CD_LBA;
		tocRepaired = true;
	}
	if (disc.audioLeadOutLBA > MAX_VALID_CD_LBA) {
		disc.audioLeadOutLBA = MAX_VALID_CD_LBA;
	}
	if (tocRepaired) {
		// Fix inverted ranges caused by clamping startLBA past endLBA
		// (e.g. bogus startLBA=16M clamped to 450k, but endLBA=19999 from
		// a valid next-track pointer).  Set endLBA = startLBA so the track
		// has zero length rather than a DWORD-wrapping underflow.
		for (auto& t : disc.tracks) {
			if (t.startLBA > t.endLBA) {
				t.endLBA = t.startLBA;
			}
		}
		disc.tocRepaired = true;
		Console::SetColor(Console::Color::Yellow);
		std::cerr << "  TOC contained out-of-range LBAs (corrupt drive response). "
			<< "Values clamped - verify results.\n";
		Console::Reset();
	}

	// ── TOC recovery from Full TOC (Format 2) ──────────────────────────
	// Copy-protection schemes (Cactus Data Shield, Key2Audio, etc.) inject
	// bogus LBA values into the Format 0 TOC response.  The disc still
	// plays because CD players use the raw Q-subchannel TOC in the lead-in,
	// which is physically pressed and harder to forge.  Format 2 (READ TOC
	// with format=2) returns that raw lead-in data with MSF addresses.
	// If Format 0 was corrupt but Format 2 has valid entries, recover.
	if (tocRepaired && hasFullToc && !fullTocTrackStart.empty()) {
		// Save state — revert if recovery produces an invalid layout
		auto savedTracks = disc.tracks;
		DWORD savedLeadOut = disc.leadOutLBA;
		DWORD savedAudioLeadOut = disc.audioLeadOutLBA;
		bool anyRecovered = false;

		// Replace clamped startLBAs with Full TOC MSF-derived values
		for (auto& t : disc.tracks) {
			if (t.startLBA != MAX_VALID_CD_LBA) continue; // not clamped
			auto fit = fullTocTrackStart.find(t.trackNumber);
			if (fit != fullTocTrackStart.end() && fit->second < MAX_VALID_CD_LBA) {
				Console::SetColor(Console::Color::Cyan);
				std::cout << "  TOC recovery: Track " << t.trackNumber
					<< " startLBA " << MAX_VALID_CD_LBA
					<< " -> " << fit->second << " (from Full TOC)\n";
				Console::Reset();
				t.startLBA = fit->second;
				anyRecovered = true;
			}
		}

		// Recover lead-out from Full TOC session data
		if (disc.leadOutLBA == MAX_VALID_CD_LBA && !sessionLeadOut.empty()) {
			DWORD bestLeadOut = sessionLeadOut.rbegin()->second;
			if (bestLeadOut < MAX_VALID_CD_LBA) {
				disc.leadOutLBA = bestLeadOut;
				anyRecovered = true;
			}
		}

		if (anyRecovered) {
			// Recalculate endLBAs from recovered starts
			for (size_t i = 0; i < disc.tracks.size(); i++) {
				if (i + 1 < disc.tracks.size()) {
					disc.tracks[i].endLBA = disc.tracks[i + 1].startLBA - 1;
					int nextSess = disc.tracks[i + 1].session;
					if (nextSess != disc.tracks[i].session) {
						auto sit = sessionLeadOut.find(disc.tracks[i].session);
						if (sit != sessionLeadOut.end() && sit->second - 1 < disc.tracks[i].endLBA)
							disc.tracks[i].endLBA = sit->second - 1;
					}
				}
				else {
					disc.tracks[i].endLBA = disc.leadOutLBA - 1;
				}
			}

			// Recalculate audioLeadOutLBA
			auto s1it = sessionLeadOut.find(1);
			if (s1it != sessionLeadOut.end() && disc.sessionCount > 1)
				disc.audioLeadOutLBA = s1it->second;
			else
				disc.audioLeadOutLBA = disc.leadOutLBA;

			// Validate: all LBAs in range, start <= end, monotonically increasing
			bool valid = true;
			for (size_t i = 0; i < disc.tracks.size(); i++) {
				if (disc.tracks[i].startLBA >= MAX_VALID_CD_LBA
					|| disc.tracks[i].startLBA > disc.tracks[i].endLBA) {
					valid = false; break;
				}
				if (i > 0 && disc.tracks[i].startLBA <= disc.tracks[i - 1].startLBA) {
					valid = false; break;
				}
			}

			if (valid) {
				// Track positions are now trustworthy — allow pregap scanning
				// and hidden-track detection.  tocRepaired stays true for the
				// protection checker (which uses rawTocEntries to evaluate the
				// original corruption).
				disc.tocLBAsRecovered = true;
				Console::Success("  TOC LBAs recovered from Full TOC data.\n");
			}
			else {
				// Recovery produced invalid layout — revert to clamped values
				disc.tracks = savedTracks;
				disc.leadOutLBA = savedLeadOut;
				disc.audioLeadOutLBA = savedAudioLeadOut;
				Console::SetColor(Console::Color::Yellow);
				std::cerr << "  TOC recovery failed - Full TOC data also appears corrupt.\n";
				Console::Reset();
			}
		}
	}

	for (auto& t : disc.tracks) {
		t.pregapLBA = t.startLBA;  // safe default until pregap scan runs
		t.index01LBA = t.startLBA; // default: INDEX 01 matches TOC start
	}
	disc.tocSignCorrected = anySignCorrected;
	return true;
}
