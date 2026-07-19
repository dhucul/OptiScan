#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleGraph.h"
#include <algorithm>
#include <iostream>

// ============================================================================
// BLER graphs - delegate to the unified Console::DrawBarGraph renderer so
// the C1 / C2 distribution charts share the same look-and-feel as every
// other graph in the GUI.
// ============================================================================

namespace {

	std::vector<int> ToBuckets(
		const std::vector<std::pair<DWORD, int>>& perSecond, int width) {
		std::vector<int> raw;
		raw.reserve(perSecond.size());
		for (const auto& p : perSecond) raw.push_back(p.second);
		return Console::BucketData(raw, width);
	}

	int PeakOf(const std::vector<std::pair<DWORD, int>>& perSecond) {
		int peak = 0;
		for (const auto& p : perSecond) peak = std::max(peak, p.second);
		return peak;
	}

}

void OpticalDrive::PrintBlerGraph(const BlerResult& result, int width, int height) {
	if (width <= 0 || height <= 0) return;

	// ── C1 Error Distribution ────────────────────────────────────────
	if (result.hasC1Data && !result.perSecondC1.empty()) {
		int peakC1 = PeakOf(result.perSecondC1);
		if (peakC1 > 0) {
			// Ensure the Red Book reference line is always visible — pad
			// scale to at least 250 even on pristine discs.
			int graphMax = std::max(peakC1, 250);

			Console::GraphOptions opts;
			opts.title = "C1 Error Distribution";
			opts.subtitle = "Peak C1 errors per second across the disc";
			opts.width = width;
			opts.height = height;
			opts.refLine = 220;
			opts.refLabel = "Red Book limit (220/sec)";
			opts.unitSuffix = "";

			auto buckets = ToBuckets(result.perSecondC1, width);
			Console::DrawBarGraph(buckets, graphMax, opts, result.totalSeconds);

			if (peakC1 > 220) {
				Console::SetColorRGB(Console::Theme::YellowR,
					Console::Theme::YellowG, Console::Theme::YellowB);
				std::cout << "  Peak C1 of " << peakC1
					<< "/sec exceeds Red Book BLER limit (220/sec).\n";
				Console::Reset();
			}
		}
	}

	// ── C2 Error Distribution ────────────────────────────────────────
	if (result.c2Unverified) {
		Console::SetColorRGB(Console::Theme::YellowR,
			Console::Theme::YellowG, Console::Theme::YellowB);
		std::cout << "\n  C2 graph omitted: C2 was not verified/measured.\n";
		Console::Reset();
		return;
	}
	if (result.perSecondC2.empty()) {
		std::cout << "\n  C2 graph unavailable: no per-second C2 series was recorded.\n";
		return;
	}
	if (result.totalC2Errors == 0 && result.totalReadFailures == 0) {
		Console::SetColorRGB(Console::Theme::WhiteR,
			Console::Theme::WhiteG, Console::Theme::WhiteB);
		std::cout << "\n  C2 Error Distribution\n";
		Console::SetColorRGB(Console::Theme::GreenR,
			Console::Theme::GreenG, Console::Theme::GreenB);
		std::cout << "  " << Console::Sym::Check
			<< " No C2 errors - chart omitted.\n";
		Console::Reset();
		return;
	}

	int peakC2 = PeakOf(result.perSecondC2);
	if (peakC2 > 0) {
		Console::GraphOptions opts;
		opts.title = "C2 Error Distribution";
		opts.subtitle = "Peak C2 errors per second across the disc";
		opts.width = width;
		opts.height = height;
		opts.unitSuffix = "/sec";
		opts.severityLowThreshold = 5;
		opts.severityHighThreshold = 20;
		opts.severityLowLabel = "1-4/sec low";
		opts.severityModerateLabel = "5-19/sec moderate";
		opts.severityHighLabel = "20+/sec high";

		auto buckets = ToBuckets(result.perSecondC2, width);
		Console::DrawBarGraph(buckets, peakC2, opts, result.totalSeconds);
	}
}
