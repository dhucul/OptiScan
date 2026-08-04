// ============================================================================
// ScanQualityRating.h - Shared error-series statistics and rating vocabulary
// ----------------------------------------------------------------------------
// Every scan mode that judges a disc from a per-time-slice error series (CD
// quality scan / Q-Check, BLER scan, C2 scan, Disc Rot, Disc Balance) used to
// carry its own peak handling and its own tier thresholds. That produced two
// systematic false positives:
//
//   1. Single-slice peaks. A drive re-locking its servo for one time slice
//      produces one inflated sample. Rating "peak C1" off the raw maximum
//      turns that mechanical event into an archival verdict about the medium.
//      A genuine defect persists across several consecutive slices.
//
//   2. Peaks read at high scan speed. C1/BLER is only archivally meaningful at
//      the low speeds the Red Book measurement assumes. Drives that clamp their
//      floor high (e.g. Pioneer BDR-S13U at 48x) produce error series dominated
//      by tracking behaviour, not by the disc. The failure is asymmetric: a
//      clean reading at high speed is still strong evidence of a good disc,
//      while a bad reading at high speed is not evidence of a bad one.
//
// This header centralises both rules so all scan paths reach the same verdict
// from the same data.
// ============================================================================
#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace ScanQuality {

// Consecutive time slices a raised error rate must persist before it counts as
// a property of the disc rather than a drive/servo transient.
inline constexpr int kDefaultMinRunSamples = 3;

// Red Book block-error-rate ceiling, C1 errors per second.
inline constexpr double kRedBookBlerLimit = 220.0;

// Scan speed at or below which peak-based archival judgements are meaningful.
inline constexpr int kArchivalScanSpeedMax = 8;

// Scan speed above which the series is dominated by servo behaviour rather
// than by the medium.
inline constexpr int kIndicativeScanSpeedMax = 16;

// Smallest raw peak worth explaining to the reader. Below this the peak could
// not have changed any tier (it sits inside the best archival band), so
// printing "this peak was a transient" only teaches the reader to skip the
// line that matters when the peak really is 252/sec.
inline constexpr int kMinPeakWorthExplaining = 50;

// Same idea for the Pioneer E22 diagnostic, whose first tier boundary is 25.
inline constexpr int kMinE22PeakWorthExplaining = 25;

// ── Confidence in an error series, derived from the speed it was read at ────
enum class Confidence {
	Archival,     // <= 8x   peaks reflect the medium
	Indicative,   // <= 16x  trend usable, peaks inflated
	Unreliable    // > 16x   servo noise dominates; peaks are not disc evidence
};

// scanSpeedX of 0 means "maximum speed" throughout this codebase (SET CD SPEED
// 0xFFFF), which is the worst case for peak trust, not an unknown middle case.
Confidence ConfidenceForSpeed(int scanSpeedX);
const char* ConfidenceLabel(Confidence c);

// One-line caveat for the report, or nullptr when no caveat is warranted.
const char* ConfidenceCaveat(Confidence c);

// Whether a peak-derived finding may escalate a verdict at this confidence.
// Averages and zone ratios stay admissible at any speed; absolute peak
// thresholds do not.
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
	// is the figure every rating tier is judged against.
	int  sustainedPeak = 0;
	int  sustainedPeakIndex = -1;

	int  minRunSamples = kDefaultMinRunSamples;

	// True when the raw peak did not persist long enough to count as a defect.
	bool peakIsTransient = false;

	// False when the series is shorter than minRunSamples, i.e. too short for
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
	int minRunSamples = kDefaultMinRunSamples);

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
// Unrated is not a hedge: it means the measurement that would justify a worse
// tier was not trustworthy at the speed used. Reporting it as a tier would
// present servo behaviour as a disc verdict.
enum class Tier { Ideal, Good, Acceptable, Poor, Unrated };

const char* TierName(Tier t);            // Ideal / Good / Acceptable / Poor
const char* TierNameDiagnostic(Tier t);  // ... / Concerning  (E22 wording)

// Archival peak-C1 tier, judged on the sustained level rather than the raw
// peak, and withheld when a bad reading came from an untrustworthy speed.
Tier RateArchivalC1(const SeriesStats& c1, Confidence conf);

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
