#pragma once
#include <cstdint>

enum class LiteOnScanMethod { Unknown, MeasuredIntervals, CounterSamples };

// Prefer the protocol whose reads establish exact coverage and provide CU.
// Falling back never depends on whether a healthy sample happened to be zero.
template<class MeasuredProbe, class CounterProbe>
LiteOnScanMethod ProbeLiteOnScanMethod(MeasuredProbe&& measured, CounterProbe&& counters) {
	if (measured()) return LiteOnScanMethod::MeasuredIntervals;
	if (counters()) return LiteOnScanMethod::CounterSamples;
	return LiteOnScanMethod::Unknown;
}

inline bool LiteOnMethodMeasuresCu(LiteOnScanMethod method) {
	return method == LiteOnScanMethod::MeasuredIntervals;
}

enum class ScanPositionResult { Sample, NoSample, Complete, Invalid };

// Firmware positions identify observations, not measured time intervals.
// Ignore duplicate polls, reject regressions, and never store end markers.
struct LiteOnPositionGuard {
	std::uint32_t first = 0, last = 0, previous = 0;
	bool havePrevious = false, complete = false;
	void Reset(std::uint32_t start, std::uint32_t end) {
		first = start; last = end; previous = start;
		havePrevious = false; complete = false;
	}
	ScanPositionResult Observe(std::uint32_t position, bool terminal = false) {
		if (complete) return ScanPositionResult::Complete;
		// A normal end marker can cross the final partial CD-second interval.
		// It cannot stand in for traversal of the rest of the requested range.
		const bool nearEnd = havePrevious && std::uint64_t{last} - previous <= 75;
		if (terminal) {
			if (!havePrevious) return ScanPositionResult::NoSample;
			if (!nearEnd) return ScanPositionResult::Invalid;
			complete = true;
			return ScanPositionResult::Complete;
		}
		if (position >= last && (!nearEnd && (havePrevious || first != last)))
			return ScanPositionResult::Invalid;
		if (position > last) {
			if (std::uint64_t{position} - last > 75) return ScanPositionResult::Invalid;
			complete = true;
			return ScanPositionResult::Complete;
		}
		if (position < first)
			return havePrevious ? ScanPositionResult::Invalid : ScanPositionResult::NoSample;
		if (havePrevious && position < previous) return ScanPositionResult::Invalid;
		if (havePrevious && position == previous) return ScanPositionResult::NoSample;
		previous = position; havePrevious = true;
		complete = position == last;
		return ScanPositionResult::Sample;
	}
};
