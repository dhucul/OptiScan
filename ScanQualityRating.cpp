// ============================================================================
// ScanQualityRating.cpp - Shared error-series statistics and rating vocabulary
// ============================================================================
#define NOMINMAX
#include "ScanQualityRating.h"

#include <algorithm>
#include <cstdlib>
#include <ostream>
#include <sstream>

namespace ScanQuality {

namespace {

int PercentileOf(const std::vector<int>& sorted, double pct) {
	if (sorted.empty()) return 0;
	// Nearest-rank percentile.
	size_t rank = static_cast<size_t>(pct / 100.0 * static_cast<double>(sorted.size()));
	if (rank >= sorted.size()) rank = sorted.size() - 1;
	return sorted[rank];
}

} // namespace

// ── Confidence ──────────────────────────────────────────────────────────────

Confidence ConfidenceForSpeed(int scanSpeedX) {
	// 0 means "maximum speed" in this codebase, and a negative value means the
	// speed was never established. Both are the worst case for peak trust, so
	// neither may claim archival confidence.
	if (scanSpeedX <= 0) return Confidence::Unreliable;
	if (scanSpeedX <= kArchivalScanSpeedMax) return Confidence::Archival;
	if (scanSpeedX <= kIndicativeScanSpeedMax) return Confidence::Indicative;
	return Confidence::Unreliable;
}

const char* ConfidenceLabel(Confidence c) {
	switch (c) {
	case Confidence::Archival:   return "Archival (peaks meaningful)";
	case Confidence::Indicative: return "Indicative (peaks inflated)";
	default:                     return "Unreliable for peaks (servo-dominated)";
	}
}

const char* ConfidenceCaveat(Confidence c) {
	switch (c) {
	case Confidence::Archival:
		return nullptr;
	case Confidence::Indicative:
		return "Scanned above 8x: error peaks are inflated by drive tracking "
		       "behaviour. Averages and zone trends remain usable.";
	default:
		return "Scanned above 16x: the per-slice error series is dominated by "
		       "servo behaviour, not by the disc. A clean reading here is still "
		       "good evidence; a high reading is not evidence of a bad disc.";
	}
}

bool PeakEvidenceAdmissible(Confidence c) {
	return c != Confidence::Unreliable;
}

// ── Series statistics ───────────────────────────────────────────────────────

SeriesStats Analyze(const std::vector<int>& values, int minRunSamples) {
	SeriesStats s;
	s.minRunSamples = std::max(1, minRunSamples);
	s.count = static_cast<int>(values.size());
	if (values.empty()) return s;

	for (size_t i = 0; i < values.size(); i++) {
		int v = values[i];
		s.total += v;
		if (v > s.peak) {
			s.peak = v;
			s.peakIndex = static_cast<int>(i);
		}
	}
	s.mean = static_cast<double>(s.total) / static_cast<double>(values.size());

	std::vector<int> sorted = values;
	std::sort(sorted.begin(), sorted.end());
	s.median = PercentileOf(sorted, 50.0);
	s.p95 = PercentileOf(sorted, 95.0);
	s.p99 = PercentileOf(sorted, 99.0);

	// Sustained peak: the highest level held across `minRunSamples` consecutive
	// slices, i.e. max over every window of the window's minimum. A one-slice
	// spike contributes only its neighbours' (low) values, so it cannot raise
	// this figure.
	const int win = s.minRunSamples;
	s.persistenceMeasurable = static_cast<int>(values.size()) >= win;
	if (!s.persistenceMeasurable) {
		// Too short for "held for N consecutive slices" to mean anything. Leave
		// sustainedPeak at 0 and let persistenceMeasurable drive the tier to
		// Unrated. Taking the series minimum here (the previous behaviour) made
		// a one-sample capture of 252/sec report a sustained 252/sec - exactly
		// the single-slice verdict this module exists to prevent.
		s.sustainedPeak = 0;
		s.sustainedPeakIndex = -1;
	}
	else {
		for (size_t i = 0; i + static_cast<size_t>(win) <= values.size(); i++) {
			int windowMin = values[i];
			for (int k = 1; k < win; k++)
				windowMin = std::min(windowMin, values[i + static_cast<size_t>(k)]);
			if (windowMin > s.sustainedPeak) {
				s.sustainedPeak = windowMin;
				s.sustainedPeakIndex = static_cast<int>(i);
			}
		}
	}

	// Width of the excursion around the raw peak, measured at half its height.
	// A genuine defect broadens; a servo transient does not.
	if (s.peakIndex >= 0 && s.peak > 0) {
		const int halfPeak = std::max(1, s.peak / 2);
		int run = 1;
		for (int i = s.peakIndex - 1; i >= 0 && values[i] >= halfPeak; i--) run++;
		for (size_t i = static_cast<size_t>(s.peakIndex) + 1;
			i < values.size() && values[i] >= halfPeak; i++) run++;
		s.peakRunLength = run;
		s.peakIsTransient = (run < s.minRunSamples);

		// An excursion is a peak that rises above the body of the series. On a
		// flat series peak == p95 == sustainedPeak, so nothing stands out and
		// there is no event to attribute to anything.
		s.peakIsExcursion = (s.peak > s.sustainedPeak) && (s.peak > s.p95);
	}

	return s;
}

int LongestRunAtOrAbove(const std::vector<int>& values, int threshold) {
	int best = 0, run = 0;
	for (int v : values) {
		if (v >= threshold) { run++; if (run > best) best = run; }
		else run = 0;
	}
	return best;
}

bool PeaksCorrelated(const SeriesStats& a, const SeriesStats& b,
	int toleranceSlices) {
	if (a.peakIndex < 0 || b.peakIndex < 0) return false;
	if (a.peak <= 0 || b.peak <= 0) return false;
	// Both sides must be real excursions. Without this, two flat series each
	// "peak" at index 0 and get reported as one shared physical event.
	if (!a.peakIsExcursion || !b.peakIsExcursion) return false;
	return std::abs(a.peakIndex - b.peakIndex) <= toleranceSlices;
}

bool TransientNoteWarranted(int rawPeak, bool peakIsTransient,
	int minPeakWorthExplaining) {
	return peakIsTransient && rawPeak >= minPeakWorthExplaining;
}

// ── Rating vocabulary ───────────────────────────────────────────────────────

const char* TierName(Tier t) {
	switch (t) {
	case Tier::Ideal:      return "Ideal";
	case Tier::Good:       return "Good";
	case Tier::Acceptable: return "Acceptable";
	case Tier::Poor:       return "Poor";
	default:               return "NOT RATED";
	}
}

const char* TierNameDiagnostic(Tier t) {
	switch (t) {
	case Tier::Ideal:      return "Ideal";
	case Tier::Good:       return "Good";
	case Tier::Acceptable: return "Acceptable";
	case Tier::Poor:       return "Concerning";
	default:               return "NOT RATED";
	}
}

namespace {

// Shared asymmetric confidence gate.
//
// High-speed scanning inflates error counts; it does not invent quiet ones. So
// a clean or middling reading taken at a bad speed is still informative and
// passes through unchanged (the report prints the speed caveat alongside it).
// Only the worst tier is withheld: an adverse verdict is exactly the claim a
// servo-dominated measurement cannot support, and reporting it anyway is what
// made every disc in a fast-clamping drive look bad.
Tier ApplyConfidenceGate(Tier raw, Confidence conf) {
	if (conf != Confidence::Unreliable) return raw;
	if (raw == Tier::Poor) return Tier::Unrated;
	return raw;
}

} // namespace

Tier RateArchivalC1(const SeriesStats& c1, Confidence conf) {
	// A capture too short to establish persistence supports no tier at all -
	// neither an adverse one nor a clean one.
	if (!c1.persistenceMeasurable) return Tier::Unrated;

	// Judged on the sustained level, never the raw peak.
	const int level = c1.sustainedPeak;
	Tier raw;
	if (level < 50)                                raw = Tier::Ideal;
	else if (level < 100)                          raw = Tier::Good;
	else if (level <= static_cast<int>(kRedBookBlerLimit)) raw = Tier::Acceptable;
	else                                           raw = Tier::Poor;
	return ApplyConfidenceGate(raw, conf);
}

Tier RatePioneerE22(long long total, double avgPerSecond,
	const SeriesStats& e22, Confidence conf, bool correlatedWithC1) {
	if (!e22.persistenceMeasurable) return Tier::Unrated;
	if (total == 0) return Tier::Ideal;

	const int level = e22.sustainedPeak;
	Tier raw;
	if (avgPerSecond < 0.25 && level < 25)      raw = Tier::Good;
	else if (avgPerSecond < 1.0 && level < 100) raw = Tier::Acceptable;
	else                                        raw = Tier::Poor;

	// An E22 excursion at the same slice as the C1 excursion is one event seen
	// by two counters. It must not independently escalate the tier.
	if (correlatedWithC1 && raw == Tier::Poor && avgPerSecond < 1.0)
		raw = Tier::Acceptable;

	return ApplyConfidenceGate(raw, conf);
}

std::string TransientNote(const char* seriesLabel, const SeriesStats& s) {
	std::ostringstream out;
	out << "Peak " << seriesLabel << " of " << s.peak
		<< "/sec lasted " << s.peakRunLength
		<< (s.peakRunLength == 1 ? " slice" : " slices")
		<< " (needs " << s.minRunSamples
		<< ") - treated as a drive transient, not a disc defect. "
		<< "Sustained level: " << s.sustainedPeak << "/sec.";
	return out.str();
}

void PrintWrapped(std::ostream& os, const std::string& text,
	const char* indent, size_t width) {
	std::istringstream in(text);
	std::string word, line;
	auto flush = [&]() {
		if (!line.empty()) { os << indent << line << "\n"; line.clear(); }
	};
	while (in >> word) {
		if (!line.empty() && line.size() + 1 + word.size() > width) flush();
		if (!line.empty()) line += ' ';
		line += word;
	}
	flush();
}

void PrintConfidenceCaveat(std::ostream& os, Confidence c, const char* indent) {
	if (const char* caveat = ConfidenceCaveat(c))
		PrintWrapped(os, caveat, indent);
}

std::string UnratedNote(const char* seriesLabel, int scanSpeedX) {
	std::ostringstream out;
	if (ConfidenceForSpeed(scanSpeedX) != Confidence::Unreliable) {
		// Not a speed problem, so the series was too short to judge.
		out << seriesLabel << " rating withheld: the scan produced fewer than "
			<< kDefaultMinRunSamples << " time slices, too short to establish "
			   "whether any error level persisted. Re-run the scan to completion.";
		return out.str();
	}
	out << seriesLabel << " rating withheld: scanned at " << scanSpeedX
		<< "x, above the " << kIndicativeScanSpeedMax
		<< "x ceiling. Past that, per-slice peaks reflect the drive's tracking "
		   "rather than the disc, so an adverse verdict is not supportable. "
		   "Averages and zone trends remain valid. For a real archival "
		   "assessment use a drive that honours 4x-8x, or verify the copy with "
		   "secure extraction / AccurateRip.";
	return out.str();
}

} // namespace ScanQuality
