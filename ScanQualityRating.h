// Shared error statistics and observed-rate ratings for every scan mode.
// C1 bands are OptiScan heuristics, not archival or Red Book certification.
// See docs/c1-rating-policy.md for thresholds, sources and measurement limits.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <iosfwd>
#include <string>
#include <vector>

namespace ScanQuality {

// Consecutive samples used by the sustained-level diagnostic. A short spike
// remains evidence; its duration alone cannot identify its physical cause.
inline constexpr int kDefaultMinRunSamples = 3;

// BLER reference: 220 blocks/sec over a 10-second measuring period.
// Comparing a whole-scan mean or a three-sample minimum is not a compliance test.
inline constexpr double kRedBookBlerLimit = 220.0;

// Application speed caution bands, not calibrated measurement guarantees.
inline constexpr int kLowScanSpeedMax = 8;

// Higher speeds warrant additional caution when interpreting peak evidence.
inline constexpr int kIndicativeScanSpeedMax = 16;

// Explain brief C1 excursions once they reach the elevated-rate band.
inline constexpr int kMinPeakWorthExplaining = 50;

// Same idea for the Pioneer E22 diagnostic, whose first tier boundary is 25.
inline constexpr int kMinE22PeakWorthExplaining = 25;

// ── Confidence in an error series, derived from the speed it was read at ────
enum class Confidence {
	LowSpeed,    // <= 8x   low-speed comparison
	Indicative,   // <= 16x  speed-dependent measurements
	Unreliable    // > 16x or unknown: conservative diagnostic gate
};

// scanSpeedX of 0 means "maximum speed" throughout this codebase (SET CD SPEED
// 0xFFFF), which is the worst case for peak trust, not an unknown middle case.
Confidence ConfidenceForSpeed(int scanSpeedX);
const char* ConfidenceLabel(Confidence c);

// One-line caveat for the report, or nullptr when no caveat is warranted.
const char* ConfidenceCaveat(Confidence c);

// Conservative gate for separate E22 diagnostics; C1 observed-rate labels
// retain the measured value at every speed and show a speed caution.
bool PeakEvidenceAdmissible(Confidence c);

// ── Summary of one per-time-slice error series ──────────────────────────────
struct SeriesStats {
	int       count = 0;
	long long total = 0;
	double    mean = 0.0;
	int       median = 0;
	int       p95 = 0;
	int       p99 = 0;

	int  peak = 0;             // raw maximum of the series
	int  peakIndex = -1;
	int  peakRunLength = 0;    // consecutive slices around the peak at >= half peak

	// Highest level held for at least minRunSamples consecutive slices. This
	// is a persistence diagnostic, separate from the average and raw peak.
	int  sustainedPeak = 0;
	int  sustainedPeakIndex = -1;

	int  minRunSamples = kDefaultMinRunSamples;

	// True when the half-height peak excursion lasted fewer than minRunSamples.
	bool peakIsTransient = false;

	// False when there is no contiguous window of minRunSamples, so
	// "held for N consecutive slices" to mean anything. sustainedPeak stays 0
	// in that case and every tier judged from it comes back Unrated - a
	// two-sample capture can neither convict nor clear a disc.
	bool persistenceMeasurable = false;

	// True when the raw peak actually stands out from the body of the series.
	// A flat series technically has a maximum, but it is not an *event*, and
	// treating it as one produces spurious cross-series correlations.
	bool peakIsExcursion = false;
};

SeriesStats Analyze(const std::vector<int>& values,
	int minRunSamples = kDefaultMinRunSamples,
	const std::vector<unsigned long>& sampleLbas = {});

// True when two series spike at (nearly) the same slice. A correlated second
// series is the same physical event counted twice, not independent evidence.
// Both series must contain a genuine excursion: two flat series trivially
// "peak" at index 0 and must not be reported as one shared event.
bool PeaksCorrelated(const SeriesStats& a, const SeriesStats& b,
	int toleranceSlices = 1);

// Longest run of consecutive slices at or above `threshold`. Used by the report
// paths to say how long an excursion actually stayed above a specific line
// (e.g. the Red Book limit) — SeriesStats::peakRunLength answers a different
// question (the width of the peak at half its own height) and must not be
// substituted for this.
int LongestRunAtOrAbove(const std::vector<int>& values, int threshold);

// ── Shared rating vocabulary ────────────────────────────────────────────────
// Separate Pioneer E22 diagnostic vocabulary; not an archival certification.
enum class Tier { Ideal, Good, Acceptable, Poor, Unrated };

const char* TierName(Tier t);            // Ideal / Good / Acceptable / Poor
const char* TierNameDiagnostic(Tier t);  // ... / Concerning  (E22 wording)

// One observed C1 scale for means and sustained levels. Excellent/Good/Fair
// are application bands; they do not establish a disc's lifetime or copyability.
inline constexpr double kC1ExcellentLimit = 5.0;
inline constexpr double kC1ElevatedLimit = 50.0;
inline constexpr int kC1GraphLowThreshold = static_cast<int>(kC1ElevatedLimit);
inline constexpr int kC1GraphHighThreshold = static_cast<int>(kRedBookBlerLimit);
inline constexpr const char* kC1GraphLowLabel = "<50/sec low";
inline constexpr const char* kC1GraphModerateLabel = "50-219/sec elevated";
inline constexpr const char* kC1GraphHighLabel = "220+/sec high";
inline constexpr const char* kC1ReferenceLabel = "220/sec reference (not a compliance test)";

enum class C1Rating { Excellent, Good, Fair, Poor, Unrated };
C1Rating RateC1(double rate, bool measured = true);
C1Rating RateSustainedC1(const SeriesStats& c1);
const char* C1RatingName(C1Rating rating);
// Keep the more adverse measured C1/read result. Unknown C1 is never a clean pass.
std::string CombineC1Quality(C1Rating c1, const std::string& readRating);
void PrintC1Policy(std::ostream& os, const char* indent = "  ");

// A raw count with independently established disc coverage. Zero sectors means
// duration is unknown; never infer it from elapsed host time or poll count.
struct C1Interval {
	std::uint32_t lba = 0;
	std::uint32_t sectors = 0;
	int errors = 0;
};

struct C1Statistics {
	long long total = 0;
	std::size_t samples = 0;
	std::uint64_t measuredSectors = 0;
	bool verified = false;
	bool timingKnown = false;
	double average = 0.0;
	double peakRate = 0.0;
	int rawPeakCount = 0;
	SeriesStats fullSeconds;
	bool tenSecondWindowAvailable = false;
	double worstTenSecondAverage = 0.0;
	bool RateAvailable() const { return verified && timingKnown && measuredSectors > 0 && std::isfinite(average); }
	double MeasuredSeconds() const { return measuredSectors / 75.0; }
	C1Rating Rating() const { return RateC1(average, RateAvailable()); }
};

C1Statistics SummarizeC1(const std::vector<C1Interval>& samples, bool verified = true);
// Acquisition validity is independent of whether any errors occurred.
C1Statistics SummarizeCompletedC1(const std::vector<C1Interval>& samples,
	std::size_t minimumSamples, bool completed);

// Equal-width disc-position columns. -1 is missing data, distinct from a
// measured zero. Partially covered columns retain their observed maximum.
struct TimedCounterGraph {
	std::vector<int> values;
	std::vector<bool> partialCoverage;
	std::uint32_t firstLba = 0;
	std::uint64_t sectorCount = 0;
	bool valid = false;
	bool rawCounts = false; // Observed counts remain plottable without verified duration.
	double average = 0;
	double peak = 0;
	std::uint32_t peakLba = 0;
	bool RateAvailable() const { return valid && !rawCounts; }
	const char* UnitSuffix() const { return rawCounts ? "/sample" : "/sec"; }
};
TimedCounterGraph BuildTimedCounterGraph(const std::vector<C1Interval>& samples,
	std::uint32_t firstLba, std::uint64_t sectorCount, int width);
// Prefer measured rates; otherwise plot raw counts at their reported positions.
// This display fallback never changes the rate-based quality assessment.
TimedCounterGraph BuildObservedCounterGraph(const std::vector<C1Interval>& samples,
	std::uint32_t firstLba, std::uint64_t sectorCount, int width, bool allowRates = true);
std::string CounterAverageText(const TimedCounterGraph& graph);
std::string CounterPeakText(const TimedCounterGraph& graph);
void PrintCounterSummary(std::ostream& os, const char* label,
	const TimedCounterGraph& graph, const char* indent = "  ");

// Successful READ CD observations only; failed/missing sectors form real gaps.
void AppendC1Sector(std::vector<C1Interval>& samples, std::uint32_t lba,
	int errors, bool startNewInterval = false);
void PrintC1Summary(std::ostream& os, const C1Statistics& c1,
	std::uint64_t requestedSectors, const char* indent = "  ");

// Pioneer E22 diagnostic tier. `correlatedWithC1` suppresses escalation when
// the E22 peak coincides with the C1 peak.
Tier RatePioneerE22(long long total, double avgPerSecond,
	const SeriesStats& e22, Confidence conf, bool correlatedWithC1);

// Whether a rejected peak is worth a line of explanation. A peak too small to
// have influenced any tier needs no defence.
bool TransientNoteWarranted(int rawPeak, bool peakIsTransient,
	int minPeakWorthExplaining = kMinPeakWorthExplaining);

// Explanation printed beneath a raw peak the rating declined to use.
std::string TransientNote(const char* seriesLabel, const SeriesStats& s);

// Explanation printed when a tier came back Unrated.
std::string UnratedNote(const char* seriesLabel, int scanSpeedX);

// ── Shared report helpers ───────────────────────────────────────────────────
// Word-wrap `text` at `width` columns, prefixing every line with `indent`.
// Used by all scan reports so multi-line caveats line up identically.
void PrintWrapped(std::ostream& os, const std::string& text,
	const char* indent = "  ", size_t width = 74);

// Print the speed caveat for this confidence, or nothing when none applies.
void PrintConfidenceCaveat(std::ostream& os, Confidence c,
	const char* indent = "  ");

} // namespace ScanQuality
