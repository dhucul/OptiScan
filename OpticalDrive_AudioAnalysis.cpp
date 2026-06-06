#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstdlib>

// ============================================================================
// Audio Content Analysis
// ============================================================================

bool OpticalDrive::AnalyzeAudioContent(DiscInfo& disc, AudioAnalysisResult& result, int scanSpeed) {
	std::cout << "\n=== Audio Content Analysis ===\n";
	result = AudioAnalysisResult{};
	m_drive.SetSpeed(scanSpeed);

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) {
		std::cout << "No audio tracks to analyze.\n";
		return false;
	}

	// Adaptive sampling: minimum 100 samples, max 1 per 100 sectors
	int sampleInterval = std::max(1, static_cast<int>(totalSectors / 100));

	// Count expected samples accurately (sampling restarts per track)
	int totalSamples = 0;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		DWORD trackSectors = t.endLBA - start + 1;
		totalSamples += static_cast<int>((trackSectors + sampleInterval - 1) / sampleInterval);
	}
	totalSamples = std::max(1, totalSamples);

	std::cout << "Sampling ~" << totalSamples << " sectors (every " << sampleInterval << ")...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Audio Analysis");
	progress.Start();

	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
	int tested = 0;
	int readFailures = 0;

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba += sampleInterval) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				std::cout << "\n\n*** Analysis cancelled by user ***\n";
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			if (!m_drive.ReadSectorAudioOnly(lba, buf.data())) {
				readFailures++;
				tested++;
				progress.Update(tested, totalSamples);
				continue;
			}

			bool suspicious = false;
			bool silent = IsSectorSilent(buf.data());

			if (silent) {
				result.silentSectors++;
			}
			if (IsSectorClipped(buf.data())) {
				result.clippedSectors++;
				suspicious = true;
			}

			// Calculate RMS level for accurate low-level detection
			int64_t sampleSum = 0;
			int sampleCount = AUDIO_SECTOR_SIZE / 2;
			for (int i = 0; i < AUDIO_SECTOR_SIZE; i += 2) {
				int16_t sample = *reinterpret_cast<const int16_t*>(buf.data() + i);
				sampleSum += static_cast<int64_t>(sample) * sample;  // RMS calculation
			}
			double rms = std::sqrt(static_cast<double>(sampleSum) / sampleCount);

			// Low-level if RMS < 1000 (about 3% of max)
			if (rms > 50 && rms < 1000 && !silent) {
				result.lowLevelSectors++;
				suspicious = true;
			}

			// Per-channel DC offset detection
			// Check each channel independently so single-channel bias is not masked
			int64_t dcSumL = 0, dcSumR = 0;
			int channelSamples = AUDIO_SECTOR_SIZE / 4;  // samples per channel
			for (int i = 0; i < AUDIO_SECTOR_SIZE; i += 4) {
				dcSumL += *reinterpret_cast<const int16_t*>(buf.data() + i);
				dcSumR += *reinterpret_cast<const int16_t*>(buf.data() + i + 2);
			}
			double dcOffsetL = std::abs(static_cast<double>(dcSumL) / channelSamples);
			double dcOffsetR = std::abs(static_cast<double>(dcSumR) / channelSamples);
			double dcOffset = std::max(dcOffsetL, dcOffsetR);
			if (dcOffset > 1000.0) {  // ~3% of max: filters normal audio asymmetry
				result.dcOffsetSectors++;
				suspicious = true;
			}

			if (suspicious) {
				result.suspiciousLBAs.push_back(lba);
			}

			tested++;
			progress.Update(tested, totalSamples);
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "           AUDIO CONTENT ANALYSIS REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	std::cout << "\n--- Scan Summary ---\n";
	std::cout << "  Sectors sampled:   " << tested << "\n";
	std::cout << "  Read failures:     " << readFailures;
	if (tested > 0)
		std::cout << " (" << std::fixed << std::setprecision(1)
		<< (readFailures * 100.0 / tested) << "%)";
	std::cout << "\n";

	std::cout << "\n--- Content Analysis ---\n";

	auto printMetric = [&](const char* label, int count, const char* explanation, const char* concern) {
		std::cout << "  " << label << count;
		if (tested > 0)
			std::cout << " (" << std::fixed << std::setprecision(1)
			<< (count * 100.0 / tested) << "%)";
		std::cout << "\n";
		if (count > 0)
			std::cout << "    " << (count > tested / 10 ? ">> " : "   ") << concern << "\n";
		else
			std::cout << "    " << explanation << "\n";
		};

	printMetric("Silent sectors:    ", result.silentSectors,
		"No silence anomalies detected.",
		"Sectors with near-zero audio. Usually track gaps or intentional silence.");
	printMetric("Clipped sectors:   ", result.clippedSectors,
		"No digital clipping detected.",
		"Audio peaks hit max value. May indicate mastering choices or read errors.");
	printMetric("Low-level sectors: ", result.lowLevelSectors,
		"Normal signal levels throughout.",
		"Very quiet non-silent audio. Often fade-ins/outs or soft passages.");
	printMetric("DC offset sectors: ", result.dcOffsetSectors,
		"No DC offset detected.",
		"Waveform bias detected. Common from mastering equipment or asymmetric audio.");

	if (!result.suspiciousLBAs.empty()) {
		int showCount = std::min(10, static_cast<int>(result.suspiciousLBAs.size()));
		std::cout << "\n--- Suspicious Sectors (" << showCount << " of "
			<< result.suspiciousLBAs.size() << ") ---\n";
		std::cout << "  (Sectors flagged for clipping, DC offset, or low level - may need re-read)\n";
		for (int i = 0; i < showCount; i++) {
			DWORD lba = result.suspiciousLBAs[i];
			int trackNum = 0;
			for (const auto& t : disc.tracks) {
				DWORD tStart = (t.trackNumber == 1) ? 0 : t.pregapLBA;
				if (lba >= tStart && lba <= t.endLBA) { trackNum = t.trackNumber; break; }
			}
			std::cout << "  LBA " << std::setw(8) << lba << "  (Track " << trackNum << ")\n";
		}
	}

	// Interpretation note so users understand these are content-level heuristics
	std::cout << "\n--- Note ---\n";
	std::cout << "  This scan analyzes audio waveform characteristics, not raw read errors.\n";
	std::cout << "  Silence, DC offset, and low-level sectors are common in normal audio\n";
	std::cout << "  (track gaps, fade-outs, mastering bias, quiet passages).\n";
	if (readFailures == 0)
		std::cout << "  No read failures occurred -- flagged sectors likely reflect original\n"
		<< "  recording content rather than disc damage. Cross-reference with C2,\n"
		<< "  disc rot, and multi-pass scans for a definitive assessment.\n";
	else
		std::cout << "  Read failures were detected -- some flags may indicate real damage.\n"
		<< "  Cross-reference with C2, disc rot, and multi-pass scans.\n";

	std::cout << std::string(60, '=') << "\n";
	return true;
}

bool OpticalDrive::IsSectorSilent(const BYTE* data) {
	for (int i = 0; i < AUDIO_SECTOR_SIZE; i += 4) {
		int16_t left = *reinterpret_cast<const int16_t*>(data + i);
		int16_t right = *reinterpret_cast<const int16_t*>(data + i + 2);
		if (std::abs(left) > 10 || std::abs(right) > 10) return false;
	}
	return true;
}

bool OpticalDrive::IsSectorClipped(const BYTE* data) {
	int clippedSamples = 0;
	int totalSamples = AUDIO_SECTOR_SIZE / 2;
	for (int i = 0; i < AUDIO_SECTOR_SIZE; i += 2) {
		int16_t sample = *reinterpret_cast<const int16_t*>(data + i);
		if (sample == 32767 || sample == -32768) clippedSamples++;
	}
	return clippedSamples > (totalSamples / 100);
}