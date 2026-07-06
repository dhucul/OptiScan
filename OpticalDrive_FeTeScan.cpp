// ============================================================================
// OpticalDrive_FeTeScan.cpp - Focus/tracking-error (FE/TE) servo scan (LiteOn)
//
// Uses the LiteOn 0xDF/0x08 command family via ScsiDrive::LiteOnFeTe*(). FE/TE
// are servo-loop metrics: focus error (how well the objective lens tracks the
// disc surface vertically) and tracking error (radial mistracking). A rising
// FE/TE across a region flags mechanical/reflectivity trouble a pure C1/C2 scan
// can miss. Experimental — the reply field layout is tentative (see
// ScsiDrive.LiteOnFeTe.cpp); treat magnitudes as relative.
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

bool OpticalDrive::RunFeTeScan(const DiscInfo& disc, FeTeResult& result, int scanSpeed) {
	std::cout << "\n=== CD Focus / Tracking-Error Scan (experimental) ===\n";

	if (!m_drive.SupportsLiteOnFeTe()) {
		std::cout << "ERROR: FE/TE scan not supported on this drive.\n";
		std::cout << "       Requires LiteOn/MediaTek 0xDF/0x08 servo commands.\n";
		result.supported = false;
		return false;
	}
	result.supported = true;

	// Scan range — audio tracks only, matches the C1/C2 and jitter scans.
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
	if (!m_drive.LiteOnFeTeStart(firstLBA, lastLBA)) {
		std::cout << "ERROR: Failed to start FE/TE scan.\n";
		m_drive.SetSpeed(0);
		return false;
	}

	auto startTime = std::chrono::steady_clock::now();
	bool scanDone = false;
	int sampleIndex = 0;
	DWORD lastReportedLBA = DWORD(-1);
	int lastLineLength = 0;
	constexpr int BAR_WIDTH = 25;

	while (!scanDone) {
		if (InterruptHandler::Instance().IsInterrupted()
			|| InterruptHandler::Instance().CheckEscapeKey()) {
			m_drive.LiteOnFeTeStop();
			m_drive.SetSpeed(0);
			std::cout << "\n*** FE/TE scan cancelled ***\n";
			return false;
		}

		int fe = 0, te = 0;
		DWORD currentLBA = 0;
		if (!m_drive.LiteOnFeTePoll(fe, te, currentLBA, scanDone)) {
			m_drive.LiteOnFeTeStop();
			if (!result.samples.empty()) break;
			std::cout << "\nERROR: Lost communication with drive.\n";
			m_drive.SetSpeed(0);
			return false;
		}

		if (currentLBA == 0 && fe == 0 && te == 0 && !scanDone) continue;
		if (currentLBA == lastReportedLBA && !scanDone) continue;
		lastReportedLBA = currentLBA;
		if (sampleIndex < 3) { sampleIndex++; continue; }   // discard startup artefacts
		if (currentLBA >= lastLBA) scanDone = true;

		FeTeSample s;
		s.lba = currentLBA; s.fe = fe; s.te = te;
		result.samples.push_back(s);
		result.totalFe += fe; result.totalTe += te;

		int idx = static_cast<int>(result.samples.size()) - 1;
		if (fe > result.maxFe) { result.maxFe = fe; result.maxFeSampleIndex = idx; }
		if (te > result.maxTe) { result.maxTe = te; result.maxTeSampleIndex = idx; }
		sampleIndex++;

		// ── Progress line (same shape as the jitter / QCheck scans) ──
		double pct = 0.0;
		if (result.totalSectors > 0 && currentLBA >= firstLBA) {
			pct = static_cast<double>(currentLBA - firstLBA) * 100.0 / result.totalSectors;
			if (pct > 100.0) pct = 100.0;
		}
		auto now = std::chrono::steady_clock::now();
		int elapsed = static_cast<int>(
			std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count());
		int eta = -1;
		if (pct > 1.0 && pct < 100.0) eta = static_cast<int>(elapsed * (100.0 - pct) / pct);

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
			if (eta >= 60) line << (eta / 60) << "m" << std::setfill('0') << std::setw(2) << (eta % 60) << "s";
			else line << eta << "s";
		}
		line << "  FE=" << fe << " TE=" << te;

		std::string out = line.str();
		if (static_cast<int>(out.size()) < lastLineLength)
			out.append(lastLineLength - out.size(), ' ');
		lastLineLength = static_cast<int>(out.size());
		std::cout << out << std::flush;
	}

	m_drive.LiteOnFeTeStop();
	m_drive.SetSpeed(0);

	if (!result.samples.empty()) {
		result.avgFe = static_cast<double>(result.totalFe) / result.samples.size();
		result.avgTe = static_cast<double>(result.totalTe) / result.samples.size();
	}

	std::cout << "\n";
	PrintFeTeReport(result);
	return true;
}

bool OpticalDrive::SaveFeTeLog(const FeTeResult& result, const std::wstring& filename) {
	std::ofstream f(filename);
	if (!f) return false;
	f << "lba,focus_error,tracking_error\n";
	for (const auto& s : result.samples)
		f << s.lba << "," << s.fe << "," << s.te << "\n";
	return true;
}

void OpticalDrive::PrintFeTeReport(const FeTeResult& result) {
	std::cout << "\n--- Focus / Tracking-Error Report (experimental) ---\n";
	std::cout << "  Samples:        " << result.samples.size() << "\n";
	std::cout << "  Avg focus err:  " << std::fixed << std::setprecision(1) << result.avgFe << "\n";
	std::cout << "  Max focus err:  " << result.maxFe;
	if (result.maxFeSampleIndex >= 0) std::cout << " (sample " << result.maxFeSampleIndex << ")";
	std::cout << "\n";
	std::cout << "  Avg track err:  " << std::fixed << std::setprecision(1) << result.avgTe << "\n";
	std::cout << "  Max track err:  " << result.maxTe;
	if (result.maxTeSampleIndex >= 0) std::cout << " (sample " << result.maxTeSampleIndex << ")";
	std::cout << "\n";
	std::cout << "  Note: FE/TE field layout is tentative — magnitudes are relative.\n";
}
