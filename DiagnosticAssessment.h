#pragma once
#include "ScanResults.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>

namespace Diagnostics {
inline bool IntegrityComplete(bool aborted, bool cancelled, DWORD scanned, DWORD expected) {
    return !aborted && !cancelled && expected > 0 && scanned == expected;
}

inline void AssessSubchannel(SubchannelBurnResult& result) {
    result.subchannelBurned = false;
    result.complete = false;
    const int successful = result.totalSampled - result.readFailures;
    const int crcTested = result.validQCrc + result.invalidQCrc;
    result.qCrcValidPercent = crcTested > 0 ? result.validQCrc * 100.0 / crcTested : 0.0;
    if (successful <= 0 || crcTested == 0) {
        result.verdict = result.formattedQVerified
            ? "INCOMPLETE - Formatted Q timing was readable, but raw P-W and R-W content remain unverified."
            : "INCONCLUSIVE - No usable raw subchannel measurements. Optional R-W content is unknown.";
        return;
    }
    if (result.readFailures > 0 || result.emptySubchannel > 0 || result.qCrcValidPercent < 90.0) {
        result.verdict = "INCOMPLETE - Raw subchannel coverage or Q CRC validity was insufficient. "
            "Read failures and zero-filled responses do not establish absent R-W content.";
        return;
    }
    result.complete = true;
    const int nonEmpty = successful - result.emptySubchannel;
    result.subchannelBurned = nonEmpty > 0 && result.rwDataPresent * 100.0 / nonEmpty >= 25.0;
    result.verdict = result.subchannelBurned
        ? "R-W content observed in sampled sectors; preserve raw subchannels when extracting."
        : "No substantial R-W content observed in the sampled program-area sectors. "
          "This is a sampling result, not proof that the entire disc has no optional content.";
}

inline int ContinuousScore(double ratio, std::initializer_list<std::pair<double, int>> points) {
    const std::vector<std::pair<double,int>> bp(points);
    if (ratio <= bp.front().first) return bp.front().second;
    if (ratio >= bp.back().first) return bp.back().second;
    const double logR = std::log(std::max(ratio, 1e-9));
    for (size_t i = 1; i < bp.size(); ++i) {
        if (ratio > bp[i].first) continue;
        const double lo = std::log(std::max(bp[i-1].first, 1e-9));
        const double hi = std::log(std::max(bp[i].first, 1e-9));
        const double t = hi > lo ? (logR-lo)/(hi-lo) : 0;
        return std::clamp(static_cast<int>(bp[i-1].second + t*(bp[i].second-bp[i-1].second) + 0.5), 0, 100);
    }
    return bp.back().second;
}

inline bool HasDistinctBalanceSpeeds(const std::vector<int>& speeds,
    const std::vector<int>& validSamples, int minimum) {
    std::set<int> measured;
    for (size_t i=0; i<std::min(speeds.size(), validSamples.size()); ++i)
        if (speeds[i] > 0 && validSamples[i] >= minimum) measured.insert(speeds[i]);
    return measured.size() >= 2;
}

inline int BalanceCoverageCap(const std::vector<int>& validSamples, int requested,
    int first, int last) {
    if (requested <= 0 || first < 0 || last < first || last >= static_cast<int>(validSamples.size())) return 0;
    double worst = 0;
    for (int i=first; i<=last; ++i)
        worst = std::max(worst, 1.0 - static_cast<double>(validSamples[i])/requested);
    return ContinuousScore(worst, {{0.0,100},{0.02,90},{0.10,60},{0.25,30},{0.50,0}});
}
} // namespace Diagnostics
