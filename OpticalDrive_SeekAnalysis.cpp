#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cmath>

// ============================================================================
// Seek Time Analysis - Measures head movement speed to detect mechanical issues
// ============================================================================

bool OpticalDrive::RunSeekTimeAnalysis(DiscInfo& disc, std::vector<SeekTimeResult>& results) {
	std::cout << "\n=== Seek Time Analysis ===\n";
	results.clear();

	// Build a list of contiguous audio LBA ranges from the TOC so that
	// seek tests only target readable audio sectors (skipping data tracks).
	std::vector<std::pair<DWORD, DWORD>> audioRanges;
	DWORD totalSectors = 0;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		// Track 1 may have a hidden pre-gap starting at LBA 0; every other
		// track's audio begins at its pregapLBA.
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		audioRanges.push_back({ start, t.endLBA });
		totalSectors += t.endLBA - start + 1;
	}

	if (totalSectors == 0) {
		std::cout << "No audio tracks to analyze.\n";
		return false;
	}

	// Drop to minimum (1×) speed so the drive doesn't mask slow seeks
	// behind high-speed read-ahead buffering.
	m_drive.SetSpeed(0);
	std::cout << "Testing seek times across disc surface...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	// Maps a fractional position (0.0–1.0) across the total audio surface
	// into a concrete LBA.  This lets us space test points evenly across
	// audio content even when audio ranges are non-contiguous (e.g. an
	// Enhanced CD with a data track in the middle).
	auto mapToAudioLBA = [&](double fraction) -> DWORD {
		DWORD target = static_cast<DWORD>(totalSectors * fraction);
		DWORD cumulative = 0;
		for (const auto& [start, end] : audioRanges) {
			DWORD rangeLen = end - start + 1;
			// If the target offset falls within this audio range,
			// return the exact LBA inside it.
			if (cumulative + rangeLen > target) {
				return start + (target - cumulative);
			}
			cumulative += rangeLen;
		}
		// Clamp to the last audio sector if rounding pushes us past the end.
		return audioRanges.back().second;
		};

	// 11 evenly-spaced positions give 110 directional seek pairs (11×10),
	// each repeated 5 times to get a stable median.
	constexpr int NUM_POSITIONS = 11;
	constexpr int REPEATS_PER_PAIR = 5;

	// Generate the 11 test LBAs; deduplicate in case multiple fractions
	// map to the same sector (happens on very short discs).
	std::vector<DWORD> testPositions;
	for (int i = 0; i < NUM_POSITIONS; i++) {
		DWORD lba = mapToAudioLBA(static_cast<double>(i) / (NUM_POSITIONS - 1));
		if (testPositions.empty() || lba != testPositions.back()) {
			testPositions.push_back(lba);
		}
	}

	if (testPositions.size() < 2) {
		std::cout << "Not enough distinct positions to test.\n";
		return false;
	}

	// Warm the drive's servo by reading the first and last test sectors.
	// This moves the head through the full range once so the first timed
	// seek isn't penalised by a cold-start spin-up or focus acquisition.
	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
	m_drive.ReadSectorAudioOnly(testPositions.front(), buf.data());
	m_drive.ReadSectorAudioOnly(testPositions.back(), buf.data());

	// Total number of ordered seek pairs: N × (N-1), since we test
	// both directions (inner→outer and outer→inner) separately.
	int totalTests = static_cast<int>(testPositions.size() * (testPositions.size() - 1));
	ProgressIndicator progress(40);
	progress.SetLabel("  Seek Test");
	progress.Start();

	// Pre-allocate the per-pair timings vector to avoid repeated heap
	// allocations inside the hot loop.
	std::vector<double> timings;
	timings.reserve(REPEATS_PER_PAIR);

	int tested = 0;
	// Iterate every ordered pair (i→j, i≠j) of test positions.
	for (size_t i = 0; i < testPositions.size(); i++) {
		for (size_t j = 0; j < testPositions.size(); j++) {
			if (i == j) continue;   // Skip self-seeks (distance = 0)

			// Allow the user to abort mid-scan with ESC or Ctrl+C.
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				std::cout << "\n\n*** Test cancelled by user ***\n";
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			DWORD fromLBA = testPositions[i];
			DWORD toLBA = testPositions[j];

			timings.clear();
			bool anyReadFailed = false;

			for (int rep = 0; rep < REPEATS_PER_PAIR; rep++) {
				// Force the head to physically move to fromLBA by defeating
				// the drive's read-ahead cache, then read that sector so
				// the head is genuinely positioned there.
				DefeatDriveCache(fromLBA, audioRanges.back().second);
				m_drive.ReadSectorAudioOnly(fromLBA, buf.data());
				// Also defeat the cache around the destination so the
				// subsequent seek can't be satisfied from the buffer.
				DefeatDriveCache(toLBA, audioRanges.back().second);

				// Time only the seek command itself (no data transfer).
				auto startTime = std::chrono::high_resolution_clock::now();
				bool readOk = m_drive.SeekToLBA(toLBA);
				auto endTime = std::chrono::high_resolution_clock::now();

				double seekMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
				timings.push_back(seekMs);
				if (!readOk) anyReadFailed = true;
			}

			// Take the median of 5 repeats — robust against outliers caused
			// by occasional OS scheduling jitter or drive retries.
			std::sort(timings.begin(), timings.end());
			double medianSeekMs = timings[REPEATS_PER_PAIR / 2];

			// Record the result for this directed pair.
			SeekTimeResult r;
			r.fromLBA = fromLBA;
			r.toLBA = toLBA;
			r.seekTimeMs = medianSeekMs;
			r.abnormal = anyReadFailed;  // Mark if any seek outright failed
			results.push_back(r);

			tested++;
			progress.Update(tested, totalTests);
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);   // Reset to maximum speed for subsequent operations
	m_drive.SpinDown();    // Let the disc stop spinning to reduce wear

	if (results.empty()) {
		std::cout << "No seek tests completed.\n";
		return false;
	}

	// Compute descriptive statistics across all measured seek times.
	double sum = 0, maxSeek = 0;
	for (const auto& r : results) {
		sum += r.seekTimeMs;
		if (r.seekTimeMs > maxSeek) maxSeek = r.seekTimeMs;
	}
	double avgSeek = sum / results.size();

	// Sample standard deviation (Bessel-corrected, N-1 denominator).
	double varianceSum = 0;
	for (const auto& r : results) {
		double diff = r.seekTimeMs - avgSeek;
		varianceSum += diff * diff;
	}
	double stddev = std::sqrt(varianceSum / (results.size() - 1));

	// Flag seeks that are more than 3 standard deviations above the mean.
	// These outliers typically indicate a region where the head had to
	// retry focus or tracking — a sign of scratches or disc rot.
	double abnormalThreshold = avgSeek + 3.0 * stddev;
	int abnormalCount = 0;
	for (auto& r : results) {
		if (r.seekTimeMs > abnormalThreshold || r.abnormal) {
			r.abnormal = true;
			abnormalCount++;
		}
	}

	// ── Formatted report ────────────────────────────────────────────────
	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              SEEK TIME ANALYSIS REPORT\n";
	std::cout << std::string(60, '=') << "\n";
	std::cout << "  (Measures head movement speed to detect mechanical or surface issues)\n\n";

	// Classify average seek time into health buckets:
	//   < 80 ms   = normal (healthy drive)
	//   80–150 ms = slow (possible surface degradation)
	//   > 150 ms  = very slow (likely mechanical fault)
	std::cout << "--- Timing Statistics ---\n";
	std::cout << "  Tests performed:  " << results.size() << "\n";
	std::cout << "  Average seek:     " << std::fixed << std::setprecision(1) << avgSeek << " ms";
	if (avgSeek < 80) std::cout << "  (normal)";
	else if (avgSeek < 150) std::cout << "  (slow - may indicate surface issues)";
	else std::cout << "  (very slow - possible mechanical problem)";
	std::cout << "\n";

	// High variability (stddev > 50% of mean) suggests inconsistent
	// head tracking — some zones read fine while others struggle.
	std::cout << "  Std deviation:    " << std::setprecision(1) << stddev << " ms";
	if (stddev > avgSeek * 0.5) std::cout << "  (high variability)";
	std::cout << "\n";
	std::cout << "  Maximum seek:     " << std::setprecision(1) << maxSeek << " ms\n";
	std::cout << "  Abnormal cutoff:  " << std::setprecision(1) << abnormalThreshold << " ms  (avg + 3 std dev)\n";
	if (abnormalCount > 0) {
		std::cout << "  Abnormal seeks:   " << abnormalCount;
		std::cout << " (" << std::setprecision(1) << (abnormalCount * 100.0 / results.size()) << "%)";
		std::cout << "  ** regions where head struggled to read **\n";
	}
	else {
		std::cout << "  Abnormal seeks:   None - consistent mechanical performance\n";
	}
	std::cout << std::string(60, '=') << "\n";

	return true;
}