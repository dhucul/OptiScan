#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>

// ============================================================================
// Multi-Pass Verification & Subchannel Integrity
// ============================================================================

bool OpticalDrive::RunMultiPassVerification(DiscInfo& disc, std::vector<MultiPassResult>& results, int passes, int scanSpeed) {
	std::cout << "\n=== Multi-Pass Verification (" << passes << " passes) ===\n";
	std::cout << "Testing read consistency with hash-based comparison...\n\n";
	results.clear();

	EnsureCapabilitiesDetected();

	m_drive.SetSpeed(scanSpeed);

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) {
		std::cout << "No audio tracks to verify.\n";
		return false;
	}

	int sampleInterval = std::max(1, static_cast<int>(totalSectors / 1000));

	// Count expected samples accurately (sampling restarts per track)
	int totalSamples = 0;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		if (t.endLBA < start) continue; // guard malformed track bounds
		DWORD trackSectors = t.endLBA - start + 1;
		totalSamples += static_cast<int>((trackSectors + sampleInterval - 1) / sampleInterval);
	}
	totalSamples = std::max(1, totalSamples);

	int tested = 0, perfectMatches = 0, partialMatches = 0, failures = 0;

	std::cout << "Testing ~" << totalSamples << " sample sectors...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Multi-Pass");
	progress.Start();

	std::vector<std::vector<BYTE>> reads(passes, std::vector<BYTE>(AUDIO_SECTOR_SIZE));
	std::vector<uint32_t> hashes(passes);

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		if (t.endLBA < start) continue; // guard malformed track bounds

		for (DWORD lba = start; lba <= t.endLBA; lba += sampleInterval) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				std::cout << "\n\n*** Verification cancelled by user ***\n";
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			bool readSuccess = true;

			for (int i = 0; i < passes; i++) {
				if (i > 0 && !m_hasAccurateStream) {
					DefeatDriveCache(lba, t.endLBA);
				}

				if (!m_drive.ReadSectorAudioOnly(lba, reads[i].data())) {
					readSuccess = false;
					break;
				}

				hashes[i] = CalculateSectorHash(reads[i].data());
			}

			if (!readSuccess) {
				MultiPassResult r{};
				r.lba = lba;
				r.passesMatched = 0;
				r.totalPasses = passes;
				r.allMatch = false;
				r.majorityHash = 0;
				results.push_back(r);
				failures++;
				tested++;
				progress.Update(tested, totalSamples);
				continue;
			}

			std::unordered_map<uint32_t, int> counts;
			counts.reserve(static_cast<size_t>(passes));
			for (int i = 0; i < passes; i++) {
				counts[hashes[i]]++;
			}

			int distinctCount = static_cast<int>(counts.size());
			uint32_t majorityHash = 0;
			int maxCount = 0;
			for (const auto& kv : counts) {
				if (kv.second > maxCount) {
					maxCount = kv.second;
					majorityHash = kv.first;
				}
			}

			int matchCount = 0;
			int majorityIdx = -1;
			for (int i = 0; i < passes; i++) {
				if (hashes[i] == majorityHash) {
					if (majorityIdx < 0) majorityIdx = i;
					matchCount++;
				}
			}
			if (distinctCount > 1 && majorityIdx >= 0) {
				for (int i = 0; i < passes; i++) {
					if (i == majorityIdx) continue;
					if (hashes[i] == majorityHash &&
						memcmp(reads[i].data(), reads[majorityIdx].data(), AUDIO_SECTOR_SIZE) != 0) {
						matchCount--;
					}
				}
			}

			bool allMatch = (matchCount == passes);

			MultiPassResult r{};
			r.lba = lba;
			r.passesMatched = matchCount;
			r.totalPasses = passes;
			r.allMatch = allMatch;
			r.majorityHash = majorityHash;

			if (allMatch) {
				perfectMatches++;
			}
			else if (matchCount >= (passes + 1) / 2) {
				partialMatches++;
				results.push_back(r);
			}
			else {
				failures++;
				results.push_back(r);
			}

			tested++;
			progress.Update(tested, totalSamples);
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	std::cout << "\n=== Multi-Pass Results ===\n";
	std::cout << "  Total sectors tested: " << tested << "\n";
	Console::Success("  Perfect matches: ");
	std::cout << perfectMatches << " ("
		<< std::fixed << std::setprecision(1)
		<< (tested > 0 ? (perfectMatches * 100.0 / tested) : 0.0) << "%)\n";

	if (partialMatches > 0) {
		Console::Warning("  Partial matches: ");
		std::cout << partialMatches << "\n";
	}

	if (failures > 0) {
		Console::Error("  Failed/Inconsistent: ");
		std::cout << failures << "\n";
	}

	if (!results.empty()) {
		std::cout << "\n=== Most Inconsistent Sectors ===\n";
		std::sort(results.begin(), results.end(),
			[](const MultiPassResult& a, const MultiPassResult& b) {
				return a.passesMatched < b.passesMatched;
			});

		int shown = 0;
		for (const auto& r : results) {
			if (shown++ >= 10) break;
			if (r.passesMatched == 0 && r.majorityHash == 0)
				std::cout << "  LBA " << std::setw(6) << r.lba << ": READ FAILURE\n";
			else
				std::cout << "  LBA " << std::setw(6) << r.lba
				<< ": " << r.passesMatched << "/" << r.totalPasses
				<< " matches (hash: " << std::hex << r.majorityHash << std::dec << ")\n";
		}
	}

	return true;
}

uint32_t OpticalDrive::CalculateSectorHash(const BYTE* data) {
	uint32_t hash = 2166136261u;
	for (int i = 0; i < AUDIO_SECTOR_SIZE; i++) {
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

bool OpticalDrive::FlushDriveCache() {
	BYTE cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	return m_drive.SendSCSI(cdb, 10, nullptr, 0);
}

bool OpticalDrive::VerifySubchannelIntegrity(DiscInfo& disc, int& errorCount, int scanSpeed) {
	std::cout << "\n=== Subchannel Integrity Verification ===\n";
	errorCount = 0;

	DWORD totalSectors = CalculateTotalAudioSectors(disc);

	if (totalSectors == 0) {
		std::cout << "No audio tracks to verify.\n";
		return false;
	}

	m_drive.SetSpeed(scanSpeed);
	if (scanSpeed == 0) std::cout << "Using max speed for subchannel verification...\n";
	else std::cout << "Using " << scanSpeed << "x speed for subchannel verification...\n";

	std::cout << "Verifying Q-subchannel data consistency...\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Subchannel");
	progress.Start();

	DWORD scannedSectors = 0;
	int crcErrors = 0;
	int trackMismatches = 0;
	int indexErrors = 0;
	int indexWarnings = 0;
	int consecutiveErrors = 0;
	int peakBurst = 0;
	DWORD peakBurstStartLBA = 0;
	DWORD currentBurstStartLBA = 0;

	// Early-abort: stop after this many consecutive errors to avoid grinding
	// through a severely damaged or unreadable region indefinitely.
	constexpr int CONSECUTIVE_ERROR_ABORT = 200;
	bool abortedEarly = false;

	auto startTime = std::chrono::steady_clock::now();
	auto lastSpeedUpdate = startTime;
	constexpr double CD_1X_BYTES_PER_SEC = 176400.0;

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		if (abortedEarly) break;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			// Early-abort on sustained unreadable regions
			if (consecutiveErrors >= CONSECUTIVE_ERROR_ABORT) {
				abortedEarly = true;
				Console::Warning("\nAborting: ");
				std::cout << CONSECUTIVE_ERROR_ABORT
					<< " consecutive errors starting at LBA " << currentBurstStartLBA
					<< " - disc may be severely damaged.\n";
				break;
			}

			bool sectorError = false;
			int qTrack = 0, qIndex = -1;

			// Adaptive read: single read mid-track, majority voting near transitions
			if (m_drive.ReadSectorQAdaptive(lba, qTrack, qIndex, t.pregapLBA, t.startLBA)) {
				// Validate track number:
				// - Within track: must match current track
				// - At pregap: allow current track (index 00) or previous track
				//   (pregap may read old track due to drive latency)
				bool isInPregap = (lba >= t.pregapLBA && lba < t.startLBA);
				bool trackOk = (qTrack == t.trackNumber) ||
					(isInPregap && qTrack == t.trackNumber - 1);

				if (!trackOk) {
					trackMismatches++;
					errorCount++;
					sectorError = true;
				}

				// Index range: 0-99 (0 = pregap, 1-99 = track data)
				if (qIndex < 0 || qIndex > 99) {
					indexErrors++;
					errorCount++;
					sectorError = true;
				}
				// Track index vs region consistency (informational, not an error)
				else {
					bool shouldBeIndex0 = isInPregap;
					bool isIndex0 = (qIndex == 0);
					if (shouldBeIndex0 != isIndex0) {
						indexWarnings++;
					}
				}
			}
			else {
				crcErrors++;
				errorCount++;
				sectorError = true;
			}

			if (sectorError) {
				if (consecutiveErrors == 0) currentBurstStartLBA = lba;
				consecutiveErrors++;
				if (consecutiveErrors > peakBurst) {
					peakBurst = consecutiveErrors;
					peakBurstStartLBA = currentBurstStartLBA;
				}
			}
			else {
				consecutiveErrors = 0;
			}

			scannedSectors++;

			auto now = std::chrono::steady_clock::now();
			auto timeSinceUpdate = std::chrono::duration<double>(now - lastSpeedUpdate).count();

			if (timeSinceUpdate >= 0.5) {
				double totalElapsed = std::chrono::duration<double>(now - startTime).count();
				if (totalElapsed > 0) {
					double bytesRead = scannedSectors * 2352.0;
					double actualSpeedX = bytesRead / (totalElapsed * CD_1X_BYTES_PER_SEC);

					char speedLabel[64];
					snprintf(speedLabel, sizeof(speedLabel), "  Subchannel [%.1fx]", actualSpeedX);
					progress.SetLabel(speedLabel);
				}
				lastSpeedUpdate = now;
			}

			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}

	progress.Finish(!abortedEarly);
	m_drive.SetSpeed(0);

	auto endTime = std::chrono::steady_clock::now();
	double totalSeconds = std::chrono::duration<double>(endTime - startTime).count();
	double avgSpeedX = 0.0;
	if (totalSeconds > 0) {
		double totalBytes = scannedSectors * 2352.0;
		avgSpeedX = totalBytes / (totalSeconds * CD_1X_BYTES_PER_SEC);
	}

	std::cout << "\n=== Speed Verification ===\n";
	std::cout << "Requested speed: " << (scanSpeed == 0 ? "Max" : std::to_string(scanSpeed) + "x") << "\n";
	std::cout << "Measured throughput: " << std::fixed << std::setprecision(1) << avgSpeedX << "x\n";

	WORD actualRead = 0, actualWrite = 0;
	if (m_drive.GetActualSpeed(actualRead, actualWrite)) {
		double reportedSpeedX = actualRead / 176.4;
		std::cout << "Drive-reported speed: " << std::fixed << std::setprecision(1)
			<< reportedSpeedX << "x (" << actualRead << " KB/s)\n";

		if (scanSpeed > 0 && avgSpeedX > scanSpeed * 1.5) {
			std::cout << "** Warning: Drive may be ignoring speed limit **\n";
		}
	}

	std::cout << "\n=== Subchannel Verification Results ===\n";
	std::cout << "Sectors verified: " << scannedSectors;
	if (abortedEarly) std::cout << " (aborted early)";
	std::cout << "\n";
	std::cout << "CRC/Read errors: " << crcErrors << "\n";
	std::cout << "Track mismatches: " << trackMismatches << "\n";
	std::cout << "Index errors: " << indexErrors << "\n";
	if (indexWarnings > 0) {
		std::cout << "Index warnings: " << indexWarnings << " (near transition boundaries, informational)\n";
	}
	std::cout << "Total errors: " << errorCount << "\n";
	std::cout << "Scan time: " << std::fixed << std::setprecision(1) << totalSeconds << " seconds\n";

	if (errorCount == 0) {
		std::cout << "*** Subchannel data is CLEAN ***\n";
	}
	else {
		std::cout << "*** Subchannel errors detected - may affect accurate ripping ***\n";
	}

	if (peakBurst > 1) {
		std::cout << "Peak error burst: " << peakBurst
			<< " sectors starting at LBA " << peakBurstStartLBA << "\n";
	}

	// ── Error rate interpretation ──────────────────────────────────────
	double errorRate = (scannedSectors > 0)
		? (static_cast<double>(errorCount) / scannedSectors) * 100.0
		: 0.0;

	std::cout << "\n=== Interpreting Results ===\n";
	std::cout << "Error rate: " << std::fixed << std::setprecision(2) << errorRate << "%\n";

	std::cout << std::left << std::setw(14) << "Error rate"
		<< std::setw(50) << "Assessment" << "\n";
	std::cout << std::string(64, '-') << "\n";

	if (errorRate == 0.0) {
		Console::Success("0%            ");
		std::cout << "Exceptionally clean - uncommon even on new discs\n";
	}
	else if (errorRate < 1.0) {
		Console::Success("< 1%          ");
		std::cout << "Excellent - no impact on ripping\n";
	}
	else if (errorRate <= 3.0) {
		Console::Info("1-3%          ");
		std::cout << "Normal - typical baseline for most CDs and drives\n";
	}
	else if (errorRate <= 5.0) {
		Console::Warning("3-5%          ");
		std::cout << "Elevated - may indicate disc wear, but audio extraction is unaffected\n";
	}
	else if (errorRate <= 10.0) {
		Console::Warning("5-10%         ");
		std::cout << "High - disc surface may be degraded; cross-reference with C2 scan\n";
	}
	else {
		Console::Error("> 10%         ");
		std::cout << "Severe - likely physical damage; prioritize backup with secure rip mode\n";
	}

	std::cout << "\nYour disc: " << std::fixed << std::setprecision(2) << errorRate << "% -> ";
	if (errorRate == 0.0)       Console::Success("EXCEPTIONAL\n");
	else if (errorRate < 1.0)   Console::Success("EXCELLENT\n");
	else if (errorRate <= 3.0)  Console::Info("NORMAL\n");
	else if (errorRate <= 5.0)  Console::Warning("ELEVATED\n");
	else if (errorRate <= 10.0) Console::Warning("HIGH\n");
	else                        Console::Error("SEVERE\n");

	return true;
}

// ============================================================================
// Subchannel Burn Status Verification
//
// Samples sectors across the disc and inspects the raw 96-byte subchannel
// block to determine whether subchannel data was genuinely mastered/burned.
// Checks Q-channel CRC, P-channel state, R-W non-zero data, and MSF timing.
// ============================================================================

// Helper: Red Book stores Q-channel CRC as ones-complement.  Accept both
// the inverted form (per spec) and the non-inverted form (some drives
// return the CRC already corrected).
static bool IsQCrcValid(uint16_t calcCrc, uint16_t storedCrc) {
	return (calcCrc == static_cast<uint16_t>(~storedCrc))
		|| (calcCrc == storedCrc);
}

bool OpticalDrive::VerifySubchannelBurnStatus(DiscInfo& disc, SubchannelBurnResult& result, int scanSpeed) {
	std::cout << "\n=== Subchannel Burn Status Verification ===\n";
	result = {};

	m_drive.GetMediaProfile(result.mediaProfile, result.mediaTypeName);
	if (!result.mediaTypeName.empty()) {
		std::cout << "Media type detected: " << result.mediaTypeName << "\n";
	}

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) {
		std::cout << "No audio tracks to verify.\n";
		return false;
	}

	constexpr int TARGET_SAMPLES = 500;
	int sampleInterval = std::max(1, static_cast<int>(totalSectors / TARGET_SAMPLES));
	int expectedSamples = std::max(1, static_cast<int>(totalSectors / sampleInterval));

	m_drive.SetSpeed(scanSpeed);
	if (scanSpeed == 0) std::cout << "Using max speed...\n";
	else std::cout << "Using " << scanSpeed << "x speed...\n";

	std::cout << "Sampling ~" << expectedSamples << " sectors across the disc...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Burn Check");
	progress.Start();

	std::vector<BYTE> audioBuf(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> subBuf(SUBCHANNEL_SIZE);

	int prevAbsMin = -1, prevAbsSec = -1, prevAbsFrame = -1;

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		prevAbsMin = -1;
		prevAbsSec = -1;
		prevAbsFrame = -1;

		for (DWORD lba = start; lba <= t.endLBA; lba += sampleInterval) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			result.totalSampled++;

			if (!m_drive.ReadSector(lba, audioBuf.data(), subBuf.data())) {
				result.readFailures++;
				progress.Update(result.totalSampled, expectedSamples);
				continue;
			}

			bool allZero = true;
			for (int i = 0; i < SUBCHANNEL_SIZE; i++) {
				if (subBuf[i] != 0) { allZero = false; break; }
			}
			if (allZero) {
				result.emptySubchannel++;
				progress.Update(result.totalSampled, expectedSamples);
				continue;
			}

			BYTE pChannel[12] = {};
			BYTE qChannel[12] = {};
			int rwNonZeroBits = 0;

			for (int i = 0; i < 96; i++) {
				int byteIdx = i / 8;
				int bitIdx = 7 - (i % 8);
				if (subBuf[i] & 0x80) pChannel[byteIdx] |= (1 << bitIdx);
				if (subBuf[i] & 0x40) qChannel[byteIdx] |= (1 << bitIdx);

				BYTE rw = subBuf[i] & 0x3F;
				if (rw) {
					for (BYTE b = rw; b; b &= b - 1) rwNonZeroBits++;
				}
			}

			constexpr int RW_BIT_DENSITY_THRESHOLD = 12;
			if (rwNonZeroBits >= RW_BIT_DENSITY_THRESHOLD) {
				result.rwDataPresent++;

				int cdgPacks = 0;
				for (int pack = 0; pack < 4; pack++) {
					BYTE cmd = subBuf[pack * 24] & 0x3F;
					if (cmd == 0x09) cdgPacks++;
				}
				if (cdgPacks > 0) result.cdgPacketsFound++;
			}

			uint16_t calcCrc = SubchannelCRC16(qChannel, 10);
			uint16_t storedCrc = (static_cast<uint16_t>(qChannel[10]) << 8) | qChannel[11];
			if (IsQCrcValid(calcCrc, storedCrc)) {
				result.validQCrc++;

				BYTE adr = qChannel[0] & 0x0F;
				if (adr == 1) {
					int absMin = BcdToBin(qChannel[7]);
					int absSec = BcdToBin(qChannel[8]);
					int absFrame = BcdToBin(qChannel[9]);

					if (prevAbsMin >= 0) {
						int prevTotal = prevAbsMin * 4500 + prevAbsSec * 75 + prevAbsFrame;
						int curTotal = absMin * 4500 + absSec * 75 + absFrame;
						if (curTotal > prevTotal) {
							result.validMsfTiming++;
						}
					}
					prevAbsMin = absMin;
					prevAbsSec = absSec;
					prevAbsFrame = absFrame;
				}
			}
			else {
				result.invalidQCrc++;
			}

			bool isInPregap = (lba >= t.pregapLBA && lba < t.startLBA);
			BYTE expectedP = isInPregap ? 0xFF : 0x00;
			bool pOk = true;
			for (int i = 0; i < 12; i++) {
				if (pChannel[i] != expectedP) { pOk = false; break; }
			}
			if (pOk) result.pChannelCorrect++;

			progress.Update(result.totalSampled, expectedSamples);
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	int successfulReads = result.totalSampled - result.readFailures;
	int crcTestedSectors = result.validQCrc + result.invalidQCrc;

	if (crcTestedSectors > 0) {
		result.qCrcValidPercent =
			(static_cast<double>(result.validQCrc) / crcTestedSectors) * 100.0;
	}

	double emptyPercent = (successfulReads > 0)
		? (static_cast<double>(result.emptySubchannel) / successfulReads) * 100.0
		: 100.0;

	if (emptyPercent > 50.0 || successfulReads == 0) {
		int probeOk = 0, probeAttempts = 0;
		for (const auto& t : disc.tracks) {
			if (!t.isAudio || probeAttempts >= 5) break;
			DWORD mid = t.startLBA + (t.endLBA - t.startLBA) / 2;
			int qTrack = 0, qIndex = 0;
			probeAttempts++;
			if (m_drive.ReadSectorQSingle(mid, qTrack, qIndex))
				probeOk++;
		}

		if (probeOk > 0) {
			bool isBurnedMedia = (result.mediaProfile == 0x0009 || result.mediaProfile == 0x000A);
			result.subchannelBurned = false;

			if (isBurnedMedia) {
				result.verdict =
					"STANDARD BURNED CD - Q-channel timing data verified via formatted mode.\n"
					"           This drive does not support raw subchannel reading, so R-W content\n"
					"           (CD-G, CD-TEXT) cannot be verified.\n"
					"           P+Q timing data is always written by the drive automatically.";
			}
			else {
				result.verdict =
					"STANDARD PRESSED CD - Q-channel timing data verified via formatted mode.\n"
					"           This drive does not support raw subchannel reading, so R-W content\n"
					"           (CD-G, CD-TEXT) cannot be verified.";
			}

			PrintSubchannelBurnReport(result);
			return true;
		}
	}

	int nonEmpty = successfulReads - result.emptySubchannel;
	double rwPercent = (nonEmpty > 0)
		? (static_cast<double>(result.rwDataPresent) / nonEmpty) * 100.0
		: 0.0;
	bool hasRWContent = (rwPercent >= 25.0);
	bool isBurnedMedia = (result.mediaProfile == 0x0009 || result.mediaProfile == 0x000A);

	if (result.qCrcValidPercent >= 90.0 && emptyPercent < 5.0) {
		if (hasRWContent) {
			result.subchannelBurned = true;
			result.verdict =
				"CONFIRMED - Full subchannel data was burned to disc.\n"
				"           Includes R-W content (CD-G graphics, CD-TEXT, or similar).";
		}
		else if (isBurnedMedia) {
			result.subchannelBurned = false;
			result.verdict =
				"STANDARD BURNED CD - Basic timing data (P+Q) is healthy.\n"
				"           No CD-G or CD-TEXT content was found in the R-W channels.\n"
				"           The \"burn with subchannel\" option only copies what exists on the source.";
		}
		else {
			result.subchannelBurned = false;
			result.verdict =
				"STANDARD PRESSED CD - Basic timing data (P+Q) is healthy.\n"
				"           No CD-G or CD-TEXT content was found in the R-W channels.\n"
				"           This is normal for most factory-pressed audio CDs.";
		}
	}
	else if (result.qCrcValidPercent >= 50.0 && emptyPercent < 30.0) {
		if (hasRWContent) {
			result.subchannelBurned = true;
			result.verdict =
				"CONFIRMED - Full subchannel data was burned to disc.\n"
				"           Includes R-W content (CD-G graphics, CD-TEXT, or similar).\n"
				"           Some timing errors were detected, which is normal for burned (CD-R) media.";
		}
		else if (isBurnedMedia) {
			result.subchannelBurned = false;
			result.verdict =
				"STANDARD BURNED CD - Basic timing data (P+Q) is present with some CRC errors.\n"
				"           This is normal for CD-R media and does not affect audio quality.\n"
				"           No CD-G or CD-TEXT content was found in the R-W channels.\n"
				"           The \"burn with subchannel\" option only copies what exists on the source.";
		}
		else {
			result.subchannelBurned = false;
			result.verdict =
				"STANDARD PRESSED CD - Basic timing data (P+Q) is present with some CRC errors.\n"
				"           This may indicate minor disc wear but does not affect audio quality.\n"
				"           No CD-G or CD-TEXT content was found in the R-W channels.";
		}
	}
	else if (emptyPercent >= 80.0) {
		result.subchannelBurned = false;
		result.verdict =
			"EMPTY - No subchannel data found on this disc.\n"
			"           The disc was burned without any subchannel writing.";
	}
	else {
		result.subchannelBurned = false;
		result.verdict =
			"INCONCLUSIVE - Subchannel data could not be reliably read.\n"
			"           This may indicate a low-quality burn, disc damage, or a drive\n"
			"           that does not support raw subchannel reading.";
	}

	PrintSubchannelBurnReport(result);
	return true;
}

void OpticalDrive::PrintSubchannelBurnReport(const SubchannelBurnResult& result) {

	// ── Plain-language explanation ───────────────────────────────────────
	std::cout << "\n=== What Is Subchannel Data? ===\n";
	std::cout << "Every audio CD contains hidden data alongside the music:\n";
	std::cout << "  P+Q channels  - Track numbers, timing, and pause markers.\n";
	std::cout << "                  These are ALWAYS written by the drive automatically.\n";
	std::cout << "  R-W channels  - Optional extra data such as CD-G karaoke graphics\n";
	std::cout << "                  or CD-TEXT (artist/title info). Only present if the\n";
	std::cout << "                  source disc contained them AND they were burned.\n";

	std::cout << "\n=== Subchannel Burn Status Report ===\n";
	if (!result.mediaTypeName.empty()) {
		std::cout << "Media type:            " << result.mediaTypeName;
		if (result.mediaProfile == 0x0008)      std::cout << " (factory-pressed)";
		else if (result.mediaProfile == 0x0009) std::cout << " (recordable)";
		else if (result.mediaProfile == 0x000A) std::cout << " (rewritable)";
		std::cout << "\n";
	}
	std::cout << "Sectors sampled:       " << result.totalSampled << "\n";
	std::cout << "Read failures:         " << result.readFailures << "\n";
	std::cout << "Empty subchannel:      " << result.emptySubchannel << "\n";
	std::cout << "Valid Q-channel CRC:   " << result.validQCrc
		<< " (" << std::fixed << std::setprecision(1) << result.qCrcValidPercent << "%)\n";
	std::cout << "Invalid Q-channel CRC: " << result.invalidQCrc << "\n";
	std::cout << "P-channel correct:     " << result.pChannelCorrect << "\n";
	std::cout << "Valid MSF timing:      " << result.validMsfTiming << "\n";

	// ── R-W / CD-G classification ───────────────────────────────────────
	int nonEmpty = result.totalSampled - result.readFailures - result.emptySubchannel;
	double rwPercent = (nonEmpty > 0)
		? (static_cast<double>(result.rwDataPresent) / nonEmpty) * 100.0
		: 0.0;
	double cdgPercent = (nonEmpty > 0)
		? (static_cast<double>(result.cdgPacketsFound) / nonEmpty) * 100.0
		: 0.0;

	std::cout << "R-W data present:      " << result.rwDataPresent;
	if (result.rwDataPresent == 0) {
		std::cout << " (none - standard audio CD)";
	}
	else if (cdgPercent >= 50.0) {
		std::cout << " (" << std::fixed << std::setprecision(1) << cdgPercent
			<< "% verified CD-G - disc contains karaoke/graphics data)";
	}
	else if (cdgPercent >= 10.0) {
		std::cout << " (" << std::fixed << std::setprecision(1) << cdgPercent
			<< "% verified CD-G - partial graphics data detected)";
	}
	else if (rwPercent < 10.0) {
		std::cout << " (negligible - likely electrical noise from the drive, not real data)";
	}
	else {
		std::cout << " (non-graphics R-W data detected - possibly CD-TEXT metadata)";
	}
	std::cout << "\n";

	std::cout << "\n";
	if (result.subchannelBurned) {
		Console::Success("Verdict: ");
	}
	else {
		Console::Info("Verdict: ");
	}
	std::cout << result.verdict << "\n";

	// ── Recommendations ─────────────────────────────────────────────────
	bool isBurnedMedia = (result.mediaProfile == 0x0009 || result.mediaProfile == 0x000A);

	std::cout << "\n=== Recommendations ===\n";
	if (result.subchannelBurned && result.qCrcValidPercent >= 90.0) {
		Console::Success("This disc has subchannel data worth preserving.\n");
		Console::Info("  - Enable subchannel extraction when ripping this disc.\n");
		Console::Info("  - Q-channel timing provides accurate track index positions.\n");
		if (result.cdgPacketsFound > 0) {
			Console::Info("  - CD-G graphics detected: extract R-W channels to preserve them.\n");
		}
	}
	else if (result.subchannelBurned) {
		Console::Success("This disc has subchannel data, but quality is reduced (normal for CD-R).\n");
		Console::Info("  - Subchannel extraction is still recommended to preserve R-W content.\n");
		Console::Info("  - For track boundaries, TOC-based indexing may be more reliable than\n");
		Console::Info("    raw Q-channel timing on this disc.\n");
	}
	else {
		Console::Info("This disc has no extra subchannel content to extract.\n");
		Console::Info("  - Subchannel extraction can be skipped when ripping.\n");
		Console::Info("  - Track boundaries will be read from the disc's table of contents (TOC).\n");
		if (isBurnedMedia) {
			Console::Info("\n");
			Console::Info("  Note: If you burned this disc with subchannel writing enabled but see\n");
			Console::Info("  no R-W content, the source disc did not have CD-G or CD-TEXT data.\n");
			Console::Info("  For standard audio CDs, \"burn with subchannel\" and \"burn without\n");
			Console::Info("  subchannel\" produce the same result because the R-W channels are empty\n");
			Console::Info("  on the source. The P+Q timing data is always written by the drive.\n");
		}
	}
}

bool OpticalDrive::CompareDiscCRCs(const std::vector<std::pair<int, uint32_t>>& originalCRCs,
	const std::vector<std::pair<int, uint32_t>>& copyCRCs) {
	Console::Heading("\n=== CRC Comparison Results ===\n");

	std::unordered_map<int, uint32_t> copyByTrack;
	for (const auto& p : copyCRCs) {
		copyByTrack[p.first] = p.second;
	}

	int matchCount = 0;
	int mismatchCount = 0;
	int missingCount = 0;

	std::cout << "  Track   Original CRC   Copy CRC       Result\n";
	std::cout << "  -----   ------------   --------       ------\n";

	for (const auto& o : originalCRCs) {
		const int trackNo = o.first;
		const uint32_t origCRC = o.second;

		auto it = copyByTrack.find(trackNo);
		if (it == copyByTrack.end()) {
			missingCount++;
			std::cout << "  " << std::setw(5) << trackNo << "   "
				<< std::hex << std::setfill('0') << std::setw(8) << origCRC
				<< "       " << "--------" << std::dec << std::setfill(' ')
				<< "       ";
			Console::SetColor(Console::Color::Yellow);
			std::cout << "MISSING";
			Console::Reset();
			std::cout << "\n";
			continue;
		}

		const uint32_t copyCRC = it->second;
		const bool match = (origCRC == copyCRC);

		if (match) matchCount++;
		else mismatchCount++;

		std::cout << "  " << std::setw(5) << trackNo << "   "
			<< std::hex << std::setfill('0') << std::setw(8) << origCRC
			<< "       "
			<< std::setw(8) << copyCRC << std::dec << std::setfill(' ')
			<< "       ";

		if (match) {
			Console::SetColor(Console::Color::Green);
			std::cout << "MATCH";
		}
		else {
			Console::SetColor(Console::Color::Red);
			std::cout << "MISMATCH";
		}
		Console::Reset();
		std::cout << "\n";
	}

	std::cout << "\n  Summary: " << matchCount << " matched, " << mismatchCount << " mismatched";
	if (missingCount > 0) {
		std::cout << ", " << missingCount << " missing on copy";
	}
	std::cout << "\n";

	if (mismatchCount == 0 && missingCount == 0) {
		Console::Success("\n  *** ALL TRACKS MATCH - COPY IS IDENTICAL ***\n");
	}
	else {
		Console::Error("\n  *** CRC MISMATCH DETECTED - COPY DIFFERS FROM ORIGINAL ***\n");
	}

	return mismatchCount == 0 && missingCount == 0;
}

int OpticalDrive::DetectSampleOffset(
	const std::vector<std::vector<BYTE>>& origSectors,
	const std::vector<std::vector<BYTE>>& copySectors,
	int maxOffsetSamples)
{
	// Convert raw sector bytes to int16 sample arrays for correlation.
	// Only use the first ~100 sectors -- enough for reliable detection
	// without excessive memory or compute.
	auto toSamples = [](const std::vector<std::vector<BYTE>>& sectors, size_t maxSecs) {
		std::vector<int16_t> out;
		size_t n = std::min(maxSecs, sectors.size());
		out.reserve(n * (AUDIO_SECTOR_SIZE / 2));
		for (size_t i = 0; i < n; i++) {
			const auto& s = sectors[i];
			for (size_t j = 0; j + 1 < s.size() && j + 1 < AUDIO_SECTOR_SIZE; j += 2) {
				out.push_back(static_cast<int16_t>(s[j] | (s[j + 1] << 8)));
			}
		}
		return out;
	};

	auto orig = toSamples(origSectors, 100);
	auto copy = toSamples(copySectors, 100);

	if (orig.size() < 5000 || copy.size() < 5000)
		return 0;

	// Compare in the middle of the probe to avoid lead-in / boundary artefacts.
	size_t winStart = orig.size() / 4;
	size_t winLen = std::min<size_t>(orig.size() / 4, 20000);

	int64_t bestSSD = INT64_MAX;
	int     bestOff = 0;
	size_t  bestCnt = 0;

	for (int off = -maxOffsetSamples; off <= maxOffsetSamples; off++) {
		// Each stereo sample pair = 2 int16 values in the interleaved array.
		int shift = off * 2;
		int64_t ssd = 0;
		size_t  cnt = 0;

		for (size_t i = 0; i < winLen; i++) {
			ptrdiff_t oi = static_cast<ptrdiff_t>(winStart + i);
			ptrdiff_t ci = oi + shift;
			if (ci < 0 || ci >= static_cast<ptrdiff_t>(copy.size()))
				continue;

			int d = static_cast<int>(orig[oi]) - static_cast<int>(copy[ci]);
			ssd += static_cast<int64_t>(d) * d;
			cnt++;
		}

		if (cnt > 0 && ssd < bestSSD) {
			bestSSD = ssd;
			bestOff = off;
			bestCnt = cnt;
		}
	}

	// Reject if the best alignment is still noisy -- means genuinely different audio.
	if (bestCnt == 0) return 0;
	double avgSSD = static_cast<double>(bestSSD) / bestCnt;
	if (avgSSD > 10.0) return 0;

	return bestOff;
}

void OpticalDrive::ApplySampleOffset(std::vector<std::vector<BYTE>>& rawSectors, int offsetSamples)
{
	if (offsetSamples == 0 || rawSectors.empty()) return;

	int64_t byteOffset = static_cast<int64_t>(offsetSamples) * 4;

	// Flatten into one contiguous buffer
	std::vector<BYTE> all;
	all.reserve(rawSectors.size() * AUDIO_SECTOR_SIZE);
	for (auto& s : rawSectors)
		all.insert(all.end(), s.begin(),
			s.begin() + std::min(s.size(), static_cast<size_t>(AUDIO_SECTOR_SIZE)));

	// Shift
	std::vector<BYTE> shifted;
	if (byteOffset > 0) {
		if (static_cast<size_t>(byteOffset) >= all.size()) return;
		shifted.assign(all.begin() + byteOffset, all.end());
		shifted.resize(all.size(), 0);
	}
	else {
		// Negative shift: prepend |byteOffset| zeros, then copy the input up
		// to (end - |byteOffset|). Bound check prevents iterator underflow
		// when an absurdly large negative offset is requested for a tiny buffer.
		size_t absOff = static_cast<size_t>(-byteOffset);
		if (absOff >= all.size()) return;
		shifted.resize(absOff, 0);
		shifted.insert(shifted.end(), all.begin(), all.end() + byteOffset);
	}

	// Write back into per-sector buffers
	size_t pos = 0;
	for (auto& s : rawSectors) {
		size_t len = std::min(static_cast<size_t>(AUDIO_SECTOR_SIZE), shifted.size() - pos);
		if (pos < shifted.size())
			memcpy(s.data(), &shifted[pos], len);
		pos += AUDIO_SECTOR_SIZE;
	}
}

uint32_t OpticalDrive::CalculateTrackCRC(const DiscInfo& disc, int trackIndex) {
	if (trackIndex < 0 || trackIndex >= static_cast<int>(disc.tracks.size())) return 0;
	if (disc.rawSectors.empty()) return 0;

	// For disc-to-disc comparison, use INDEX 01 boundaries (startLBA) and
	// avoid fragile pregap-derived edge sectors that can differ on CD-R copies.
	constexpr DWORD EDGE_TRIM_SECTORS = 16; // 16 * 2352 = 37632 bytes at each edge

	const auto& track = disc.tracks[trackIndex];

	// Start from INDEX 01 (stable TOC anchor)
	DWORD trackStart = track.startLBA;

	// Prefer canonical end from next track INDEX 01 when available
	DWORD canonicalEnd = track.endLBA;
	if (trackIndex + 1 < static_cast<int>(disc.tracks.size())) {
		DWORD nextStart = disc.tracks[trackIndex + 1].startLBA;
		if (nextStart > 0) {
			DWORD endFromNext = nextStart - 1;
			if (endFromNext < canonicalEnd) canonicalEnd = endFromNext;
		}
	}

	if (canonicalEnd < trackStart) return 0;
	DWORD trackSectors = canonicalEnd - trackStart + 1;

	// Compute starting raw-sector index using the same canonical scheme
	size_t sectorIdx = 0;
	for (int i = 0; i < trackIndex; i++) {
		DWORD s = disc.tracks[i].startLBA;
		DWORD e = disc.tracks[i].endLBA;
		if (i + 1 < static_cast<int>(disc.tracks.size())) {
			DWORD nextStart = disc.tracks[i + 1].startLBA;
			if (nextStart > 0) {
				DWORD endFromNext = nextStart - 1;
				if (endFromNext < e) e = endFromNext;
			}
		}
		if (e >= s) sectorIdx += static_cast<size_t>(e - s + 1);
	}

	// Trim edge sectors to remove boundary/pregap variance
	DWORD trim = (trackSectors > EDGE_TRIM_SECTORS * 2) ? EDGE_TRIM_SECTORS : 0;
	DWORD startSector = trim;
	DWORD endSectorExclusive = trackSectors - trim;

	uint32_t crc = 0xFFFFFFFF;
	const uint32_t polynomial = 0xEDB88320;

	for (DWORD i = startSector; i < endSectorExclusive; i++) {
		size_t idx = sectorIdx + i;
		if (idx >= disc.rawSectors.size()) break;

		const auto& sector = disc.rawSectors[idx];
		const size_t len = std::min(sector.size(), static_cast<size_t>(AUDIO_SECTOR_SIZE));
		for (size_t j = 0; j < len; j++) {
			crc ^= sector[j];
			for (int bit = 0; bit < 8; bit++) {
				crc = (crc >> 1) ^ ((crc & 1) ? polynomial : 0);
			}
		}
	}

	return ~crc;
}
