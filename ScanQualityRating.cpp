// ============================================================================
// ScanQualityRating.cpp - Shared error-series statistics and rating vocabulary
// ============================================================================
#define NOMINMAX
#include "ScanQualityRating.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <iomanip>
#include <limits>
#include <climits>
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
	PrintWrapped(os, "Average C1 bands (OptiScan): EXCELLENT <5; GOOD 5-<50; "
		"FAIR 50-<220; POOR >=220 errors/sec. Total C1 is an ungraded count. "
		"Rates use measured disc duration, not host scan time or poll count.", indent);
	PrintWrapped(os, "Peaks and complete 10-second windows describe local activity. "
		"These measurements do not certify archival suitability.", indent);
	PrintWrapped(os, "Red Book compliance is not evaluated by this scan.", indent);
}

C1Statistics SummarizeC1(const std::vector<C1Interval>& samples, bool verified) {
	C1Statistics result;
	result.verified = verified;
	result.samples = samples.size();
	result.timingKnown = !samples.empty();
	std::uint64_t previousEnd = 0;
	bool havePrevious = false;
	for (const auto& sample : samples) {
		if (sample.errors < 0) { result.verified = false; result.timingKnown = false; continue; }
		result.total += sample.errors;
		result.rawPeakCount = std::max(result.rawPeakCount, sample.errors);
		const auto end = std::uint64_t{sample.lba} + sample.sectors;
		if (sample.sectors == 0 || (havePrevious && sample.lba < previousEnd) ||
			end > std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1) {
			result.timingKnown = false;
		}
		result.measuredSectors += sample.sectors;
		previousEnd = end;
		havePrevious = true;
	}
	// Do not divide counts with unknown/overlapping coverage by a guessed time.
	if (!result.RateAvailable()) return result;
	result.average = result.total / result.MeasuredSeconds();
	std::vector<int> seconds;
	std::vector<unsigned long> positions;
	std::deque<C1Interval> window;
	std::uint64_t windowSectors = 0;
	long long windowErrors = 0;
	unsigned bucketSectors = 0;
	long long bucketErrors = 0;
	std::uint32_t bucketLba = 0;
	previousEnd = 0;
	havePrevious = false;
	for (const auto& sample : samples) {
		result.peakRate = std::max(result.peakRate, sample.errors * 75.0 / sample.sectors);
		if (!havePrevious || sample.lba != previousEnd) {
			bucketSectors = 0; bucketErrors = 0;
			window.clear(); windowSectors = 0; windowErrors = 0;
		}
		// Never split an aggregate count proportionally across invented seconds.
		if (bucketSectors + std::uint64_t{sample.sectors} > 75) {
			bucketSectors = 0; bucketErrors = 0;
		}
		if (sample.sectors <= 75) {
			if (bucketSectors == 0) bucketLba = sample.lba;
			bucketSectors += sample.sectors; bucketErrors += sample.errors;
			if (bucketSectors == 75) {
				seconds.push_back(static_cast<int>(std::min<long long>(bucketErrors, INT_MAX)));
				positions.push_back(bucketLba);
				bucketSectors = 0; bucketErrors = 0;
			}
		}
		window.push_back(sample);
		windowSectors += sample.sectors; windowErrors += sample.errors;
		while (windowSectors > 750 && !window.empty()) {
			windowSectors -= window.front().sectors;
			windowErrors -= window.front().errors; window.pop_front();
		}
		if (windowSectors == 750) {
			result.tenSecondWindowAvailable = true;
			result.worstTenSecondAverage = std::max(result.worstTenSecondAverage, windowErrors / 10.0);
		}
		previousEnd = std::uint64_t{sample.lba} + sample.sectors;
		havePrevious = true;
	}
	result.fullSeconds = Analyze(seconds, kDefaultMinRunSamples, positions);
	return result;
}

C1Statistics SummarizeCompletedC1(const std::vector<C1Interval>& samples,
	std::size_t minimumSamples, bool completed) {
	return SummarizeC1(samples, completed && minimumSamples > 0 && samples.size() >= minimumSamples);
}

TimedCounterGraph BuildTimedCounterGraph(const std::vector<C1Interval>& samples,
	std::uint32_t firstLba, std::uint64_t sectorCount, int width) {
	TimedCounterGraph graph;
	constexpr std::uint64_t addressLimit = std::uint64_t{UINT32_MAX} + 1;
	if (width <= 0 || sectorCount == 0 || sectorCount > addressLimit - firstLba) return graph;
	graph.firstLba = firstLba;
	graph.sectorCount = sectorCount;
	graph.values.assign(width, -1);
	graph.partialCoverage.assign(width, false);
	const auto stats = SummarizeC1(samples);
	if (!stats.RateAvailable()) return graph;
	const auto endLba = std::uint64_t{firstLba} + sectorCount;
	for (const auto& sample : samples) {
		if (sample.lba < firstLba || std::uint64_t{sample.lba} + sample.sectors > endLba)
			return graph; // an out-of-range aggregate cannot be split into invented counts
	}
	graph.valid = true;
	graph.average = stats.average;
	graph.peak = stats.peakRate;
	graph.peakLba = samples.front().lba;
	double peak = -1;
	std::vector<std::uint64_t> coverage(width, 0);
	const auto columns = static_cast<std::uint64_t>(width);
	for (const auto& sample : samples) {
		const double rate = sample.errors * 75.0 / sample.sectors;
		if (rate > peak) { peak = rate; graph.peakLba = sample.lba; }
		const int value = static_cast<int>(std::min(rate, double(INT_MAX)));
		// Work in sector * column units to preserve exact fractional bin edges.
		const auto start = (std::uint64_t{sample.lba} - firstLba) * columns;
		const auto end = (std::uint64_t{sample.lba} - firstLba + sample.sectors) * columns;
		const auto firstColumn = start / sectorCount;
		const auto lastColumn = (end - 1) / sectorCount;
		for (auto column = firstColumn; column <= lastColumn; ++column) {
			graph.values[column] = std::max(graph.values[column], value);
			coverage[column] += std::min(end, (column + 1) * sectorCount)
				- std::max(start, column * sectorCount);
		}
	}
	for (int column = 0; column < width; ++column)
		graph.partialCoverage[column] = coverage[column] > 0 && coverage[column] < sectorCount;
	return graph;
}

void AppendC1Sector(std::vector<C1Interval>& samples, std::uint32_t lba,
	int errors, bool startNewInterval) {
	if (!startNewInterval && !samples.empty() && samples.back().sectors < 75 &&
		std::uint64_t{samples.back().lba} + samples.back().sectors == lba) {
		++samples.back().sectors;
		samples.back().errors += errors;
	}
	else samples.push_back({lba, 1, errors});
}

void PrintC1Summary(std::ostream& os, const C1Statistics& c1,
	std::uint64_t requestedSectors, const char* indent) {
	const auto flags = os.flags(); const auto precision = os.precision();
	if (c1.samples > 0) os << indent << "Total C1 observed: " << c1.total << " (ungraded)\n";
	else os << indent << "Total C1 observed: unavailable (no measurements)\n";
	if (c1.timingKnown) {
		const auto minutes = c1.measuredSectors / 4500;
		const double seconds = (c1.measuredSectors % 4500) / 75.0;
		os << indent << "Measured audio: " << minutes << ":" << std::fixed << std::setprecision(3)
			<< (seconds < 10 ? "0" : "") << seconds << " (" << c1.measuredSectors << " sectors)\n";
		if (requestedSectors > 0 && c1.measuredSectors <= requestedSectors)
			os << indent << "Scan coverage: " << std::setprecision(2)
				<< c1.measuredSectors * 100.0 / requestedSectors << "% of requested audio\n";
		else os << indent << "Scan coverage: sampled region only\n";
	}
	else os << indent << "Measured audio / coverage: unavailable (sample duration or ordering unknown)\n";
	if (c1.RateAvailable()) {
		os << indent << "Average C1: " << std::fixed << std::setprecision(2) << c1.average
			<< "/sec - " << C1RatingName(c1.Rating()) << " (OptiScan band)\n";
		os << indent << "Peak C1 rate: " << c1.peakRate << "/sec (measured interval)\n";
	}
	else os << indent << "Average C1: NOT RATED (measurement or duration unverified)\n";
	if (c1.samples > 0)
		os << indent << "Raw peak C1 count: " << c1.rawPeakCount << " in one sample\n";
	else os << indent << "Raw peak C1 count: unavailable (no measurements)\n";
	if (c1.RateAvailable() && c1.fullSeconds.persistenceMeasurable)
		os << indent << "Sustained C1: " << c1.fullSeconds.sustainedPeak
			<< "/sec (minimum across three consecutive full seconds)\n";
	else os << indent << "Sustained C1: unavailable (needs three contiguous measured seconds)\n";
	if (c1.RateAvailable() && c1.tenSecondWindowAvailable)
		os << indent << "Worst observed 10-second average: " << std::fixed << std::setprecision(2)
			<< c1.worstTenSecondAverage << "/sec (complete sample-aligned windows; not a compliance test)\n";
	else os << indent << "Worst observed 10-second average: unavailable (no complete timed window)\n";
	os.flags(flags); os.precision(precision);
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
