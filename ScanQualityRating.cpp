// ============================================================================
// ScanQualityRating.cpp - Shared error-series statistics and rating vocabulary
// ============================================================================
#define NOMINMAX
#include "ScanQualityRating.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <ostream>
#include <sstream>

namespace ScanQuality {

namespace {

int PercentileOf(const std::vector<int>& sorted, double pct) {
	if (sorted.empty()) return 0;
	// Nearest-rank percentile.
	size_t rank = static_cast<size_t>(std::ceil(pct / 100.0 * sorted.size())) - 1;
	if (rank >= sorted.size()) rank = sorted.size() - 1;
	return sorted[rank];
}

} // namespace

// ── Confidence ──────────────────────────────────────────────────────────────

Confidence ConfidenceForSpeed(int scanSpeedX) {
	// 0 means "maximum speed" in this codebase, and a negative value means the
	// speed was never established. Both are the worst case for peak trust, so
	// neither can be described as a low-speed measurement.
	if (scanSpeedX <= 0) return Confidence::Unreliable;
	if (scanSpeedX <= kLowScanSpeedMax) return Confidence::LowSpeed;
	if (scanSpeedX <= kIndicativeScanSpeedMax) return Confidence::Indicative;
	return Confidence::Unreliable;
}

const char* ConfidenceLabel(Confidence c) {
	switch (c) {
	case Confidence::LowSpeed: return "Low speed (up to 8x)";
	case Confidence::Indicative: return "Higher speed (9x-16x)";
	default: return "High speed or speed unknown";
	}
}

const char* ConfidenceCaveat(Confidence c) {
	switch (c) {
	case Confidence::LowSpeed:
		return "Low scan speed does not establish calibrated measurement accuracy. "
			   "Rates depend on the drive, firmware and disc.";
	case Confidence::Indicative:
		return "Rates may change with scan speed and drive tracking. Compare repeat "
			   "scans on the same drive at the same speed.";
	default:
		return "High or unknown scan speed: elevated counts may reflect the disc, "
			   "drive or tracking behaviour. Confirm with repeat scans; speed "
			   "alone cannot identify the cause or establish disc health.";
	}
}

bool PeakEvidenceAdmissible(Confidence c) {
	return c != Confidence::Unreliable;
}

// ── Series statistics ───────────────────────────────────────────────────────

SeriesStats Analyze(const std::vector<int>& values, int minRunSamples,
	const std::vector<unsigned long>& sampleLbas) {
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
	auto consecutive = [&](size_t left, size_t right) {
		if (sampleLbas.empty()) return true;
		return sampleLbas.size() == values.size() && sampleLbas[right] > sampleLbas[left]
			&& sampleLbas[right] - sampleLbas[left] == 75;
	};
	const int win = s.minRunSamples;
	s.persistenceMeasurable = false;
	if (static_cast<int>(values.size()) < win) {
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
			bool contiguous = sampleLbas.empty() || sampleLbas.size() == values.size();
			for (int k = 1; k < win && contiguous; ++k)
				contiguous = consecutive(i + k - 1, i + k);
			if (!contiguous) continue;
			s.persistenceMeasurable = true;
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
	// Excursion width describes persistence, not its physical cause.
	if (s.peakIndex >= 0 && s.peak > 0) {
		const int halfPeak = std::max(1, s.peak / 2);
		int run = 1;
		for (int i = s.peakIndex - 1; i >= 0 && consecutive(i, i + 1) && values[i] >= halfPeak; i--) run++;
		for (size_t i = static_cast<size_t>(s.peakIndex) + 1;
			i < values.size() && consecutive(i - 1, i) && values[i] >= halfPeak; i++) run++;
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

// Legacy conservative E22 diagnostic gate. This is an application precaution,
// not proof that high speed caused the excursion. C1 observed-rate labels do
// not use this gate: the same measured rate gets the same label at every speed.
Tier ApplyConfidenceGate(Tier raw, Confidence conf) {
	if (conf != Confidence::Unreliable) return raw;
	if (raw == Tier::Poor) return Tier::Unrated;
	return raw;
}

} // namespace

C1Rating RateC1(double rate, bool measured) {
	if (!measured || !std::isfinite(rate) || rate < 0.0) return C1Rating::Unrated;
	if (rate < kC1ExcellentLimit) return C1Rating::Excellent;
	if (rate < kC1ElevatedLimit) return C1Rating::Good;
	if (rate < kRedBookBlerLimit) return C1Rating::Fair;
	return C1Rating::Poor;
}

C1Rating RateSustainedC1(const SeriesStats& c1) {
	return RateC1(c1.sustainedPeak, c1.persistenceMeasurable);
}

const char* C1RatingName(C1Rating rating) {
	switch (rating) {
	case C1Rating::Excellent: return "EXCELLENT";
	case C1Rating::Good: return "GOOD";
	case C1Rating::Fair: return "FAIR";
	case C1Rating::Poor: return "POOR";
	default: return "NOT RATED";
	}
}

std::string CombineC1Quality(C1Rating c1, const std::string& readRating) {
	auto severity = [](const std::string& rating) {
		if (rating == "BAD") return 5;
		if (rating == "POOR") return 4;
		if (rating == "FAIR") return 3;
		if (rating == "ACCEPTABLE") return 2;
		if (rating == "GOOD") return 1;
		return 0;
	};
	if (c1 == C1Rating::Unrated)
		return severity(readRating) > 0 ? readRating : "NOT RATED";
	const std::string c1Name = C1RatingName(c1);
	return severity(readRating) > severity(c1Name) ? readRating : c1Name;
}

void PrintC1Policy(std::ostream& os, const char* indent) {
	PrintWrapped(os, "C1 rate bands (OptiScan): EXCELLENT <5; GOOD 5-<50; "
		"FAIR 50-<220; POOR >=220 errors/sec. Average and sustained "
		"(highest three-sample minimum) use the same bands; raw peaks are retained.", indent);
	PrintWrapped(os, "These describe observed rates, not archival suitability or "
		"remaining correction capacity. Red Book BLER uses a 10-second "
		"measuring period; compliance is not evaluated by this scan.", indent);
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
		<< "/sec had " << s.peakRunLength
		<< (s.peakRunLength == 1 ? " slice" : " slices")
		<< " at or above half peak (persistence rule: " << s.minRunSamples
		<< ") - brief excursion; cause unconfirmed. "
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
	out << seriesLabel << " not rated: data may be unavailable, unverified or too "
		"short to establish persistence. Some diagnostic tiers are also withheld "
		"at high/unknown speed (reported speed " << scanSpeedX << "x). "
		"A missing rating is not a clean result.";
	return out.str();
}

} // namespace ScanQuality
