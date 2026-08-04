#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct DiscCrcComparisonSummary {
    int matched = 0;
    int mismatched = 0;
    int missingOnCopy = 0;
    int extraOnCopy = 0;

    bool Identical() const {
        return mismatched == 0 && missingOnCopy == 0 && extraOnCopy == 0;
    }
};

inline DiscCrcComparisonSummary AnalyzeDiscCrcSets(
    const std::vector<std::pair<int, uint32_t>>& originalCRCs,
    const std::vector<std::pair<int, uint32_t>>& copyCRCs) {
    std::unordered_map<int, uint32_t> copyByTrack;
    for (const auto& entry : copyCRCs) {
        copyByTrack[entry.first] = entry.second;
    }

    std::unordered_set<int> originalTracks;
    DiscCrcComparisonSummary summary;
    for (const auto& original : originalCRCs) {
        originalTracks.insert(original.first);
        const auto copy = copyByTrack.find(original.first);
        if (copy == copyByTrack.end()) {
            ++summary.missingOnCopy;
        }
        else if (copy->second == original.second) {
            ++summary.matched;
        }
        else {
            ++summary.mismatched;
        }
    }

    for (const auto& copy : copyCRCs) {
        if (originalTracks.find(copy.first) == originalTracks.end()) {
            ++summary.extraOnCopy;
        }
    }
    return summary;
}
