// ============================================================================
// OpticalDrive_JitterScan.cpp - Hardware-driven jitter / beta scan (LiteOn)
//
// Uses the LiteOn 0xDF/0x1B command pair (cmd_cd_jb_init / cmd_cd_jb_block)
// exposed via ScsiDrive::LiteOnJitter*().  Complements the C1/C2/CU QCheck
// scan: jitter is a physical-layer metric (timing variation in the EFM pit
// stream) that flags a marginal disc or pickup well before BLER does.
// Beta tracks pit/land asymmetry (~0 = ideal).
// ============================================================================
#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <cmath>

bool OpticalDrive::RunJitterScan(const DiscInfo& disc, JitterResult& result, int scanSpeed) {
	std::cout << "\n=== CD Jitter / Beta Scan ===\n";

	if (!m_drive.SupportsLiteOnJitter()) {
		std::cout << "ERROR: Jitter scan not supported on this drive.\n";
		std::cout << "       Requires legacy LiteOn jitter/beta commands (0xDF/0x1B).\n";
		std::cout << "       A MediaTek/LiteOn 0xF3 quality scan does not imply jitter support.\n";
		result.supported = false;
		return false;
	}
	result.supported = true;

	// Scan range — audio sectors only, matches QCheck.
	DWORD firstLBA = 0, lastLBA = 0;
	bool first = true;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		if (first) { firstLBA = start; first = false; }
		lastLBA = t.endLBA;
	}
	if (first) { std::cout << "No audio tracks.\n"; return false; }

	result.totalSectors = lastLBA - firstLBA + 1;
	result.totalSeconds = (result.totalSectors + 74) / 75;

	std::cout << "Scan range: LBA " << firstLBA << " - " << lastLBA
		<< " (" << result.totalSectors << " sectors, ~"
		<< result.totalSeconds << " sec)\n";
	std::cout << "Scan speed: " << (scanSpeed == 0 ? "Max" : (std::to_string(scanSpeed) + "x")) << "\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	m_drive.SetSpeed(scanSpeed);
	if (!m_drive.LiteOnJitterStart(firstLBA, lastLBA)) {
		std::cout << "ERROR: Failed to start jitter scan.\n";
		m_drive.SetSpeed(0);
		return false;
	}

	auto start = std::chrono::steady_clock::now();
	bool scanDone = false;
	int sampleIndex = 0;
	DWORD lastReportedLBA = DWORD(-1);
	int lastLineLength = 0;
	constexpr int BAR_WIDTH = 25;

	while (!scanDone) {
		if (InterruptHandler::Instance().IsInterrupted()
			|| InterruptHandler::Instance().CheckEscapeKey()) {
			m_drive.LiteOnJitterStop();
			m_drive.SetSpeed(0);
			std::cout << "\n*** Jitter scan cancelled ***\n";
			return false;
		}

		int jitter = 0, beta = 0;
		DWORD currentLBA = 0;
		if (!m_drive.LiteOnJitterPoll(jitter, beta, currentLBA, scanDone)) {
			m_drive.LiteOnJitterStop();
			if (!result.samples.empty()) break;   // partial data — still useful
			std::cout << "\nERROR: Lost communication with drive.\n";
			m_drive.SetSpeed(0);
			return false;
		}

		// Skip empty / duplicate responses while the drive seeks/spins up
		if (currentLBA == 0 && jitter == 0 && beta == 0 && !scanDone) continue;
		if (currentLBA == lastReportedLBA && !scanDone) continue;
		lastReportedLBA = currentLBA;

		// Discard first 3 raw samples — startup artefacts (matches QCheck)
		if (sampleIndex < 3) { sampleIndex++; continue; }

		if (currentLBA >= lastLBA) scanDone = true;

		JitterSample s;
		s.lba = currentLBA;
		s.jitter = jitter;
		s.beta = beta;
		result.samples.push_back(s);
		result.totalJitter += jitter;

		int idx = static_cast<int>(result.samples.size()) - 1;
		if (jitter > result.maxJitter) {
			result.maxJitter = jitter;
			result.maxJitterSampleIndex = idx;
		}
		if (idx == 0) {
			result.minBeta = beta;
			result.maxBeta = beta;
		}
		else {
			if (beta < result.minBeta) result.minBeta = beta;
			if (beta > result.maxBeta) result.maxBeta = beta;
		}

		sampleIndex++;

		// ── Progress line (same shape as QCheck) ─────────────
		double pct = 0.0;
		if (result.totalSectors > 0 && currentLBA >= firstLBA) {
			pct = static_cast<double>(currentLBA - firstLBA) * 100.0 / result.totalSectors;
			if (pct > 100.0) pct = 100.0;
		}
		auto now = std::chrono::steady_clock::now();
		int elapsed = static_cast<int>(
			std::chrono::duration_cast<std::chrono::seconds>(now - start).count());
		int eta = -1;
		if (pct > 1.0 && pct < 100.0) {
			eta = static_cast<int>(elapsed * (100.0 - pct) / pct);
		}

		int filled = static_cast<int>(pct * BAR_WIDTH / 100.0);
		std::ostringstream line;
		line << "\r  [";
		for (int i = 0; i < BAR_WIDTH; i++)
			line << (i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91");
		line << "] " << std::fixed << std::setprecision(1) << pct << "%"
			<< "  " << (elapsed / 60) << ":" << std::setfill('0')
			<< std::setw(2) << (elapsed % 60);
		if (eta >= 0) {
			line << " ETA:";
			if (eta >= 60)
				line << (eta / 60) << "m"
				<< std::setfill('0') << std::setw(2) << (eta % 60) << "s";
			else
				line << eta << "s";
		}
		line << "  jitter=" << jitter << " beta=" << beta;

		std::string out = line.str();
		if (static_cast<int>(out.size()) < lastLineLength)
			out.append(lastLineLength - out.size(), ' ');
		lastLineLength = static_cast<int>(out.size());
		std::cout << out << std::flush;
	}

	m_drive.LiteOnJitterStop();
	m_drive.SetSpeed(0);

	if (!result.samples.empty()) {
		result.avgJitter = static_cast<double>(result.totalJitter) / result.samples.size();
		long long absBetaSum = 0;
		for (const auto& s : result.samples) absBetaSum += std::abs(s.beta);
		result.avgAbsBeta = static_cast<double>(absBetaSum) / result.samples.size();
	}

	std::cout << "\n";
	PrintJitterReport(result);
	return true;
}

bool OpticalDrive::SaveJitterLog(const JitterResult& result, const std::wstring& filename) {
	std::ofstream f(filename);
	if (!f) return false;
	f << "lba,jitter,beta\n";
	for (const auto& s : result.samples)
		f << s.lba << "," << s.jitter << "," << s.beta << "\n";
	return true;
}

void OpticalDrive::PrintJitterReport(const JitterResult& result) {
	std::cout << "\n--- Jitter Scan Report ---\n";
	std::cout << "  Samples:       " << result.samples.size() << "\n";
	std::cout << "  Avg jitter:    " << std::fixed << std::setprecision(1)
		<< result.avgJitter << "\n";
	std::cout << "  Max jitter:    " << result.maxJitter;
	if (result.maxJitterSampleIndex >= 0)
		std::cout << " (sample " << result.maxJitterSampleIndex << ")";
	std::cout << "\n";
	std::cout << "  Beta range:    " << result.minBeta << " .. " << result.maxBeta
		<< "  (avg |beta| " << std::fixed << std::setprecision(1)
		<< result.avgAbsBeta << ")\n";
}
