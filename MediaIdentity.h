#pragma once
#include <cstdint>
#include <optional>

struct MediaIdentity {
    uint64_t openSession = 0;
    uint32_t changeCount = 0;
    bool operator==(const MediaIdentity&) const = default;
};

// Only a counter observed unchanged across the scan can certify cached data.
template<class Disc>
bool CommitScanIdentity(Disc& disc, const std::optional<MediaIdentity>& before,
                        const std::optional<MediaIdentity>& after) {
    disc.mediaIdentity.reset();
    if (before) {
        if (!after || *before != *after) return false;
        disc.mediaIdentity = before;
    }
    return true;
}

template<class Disc, class Query, class Scan>
bool ScanWithMediaIdentity(Disc& disc, Query query, Scan scan) {
    disc.mediaIdentity.reset();
    const auto before = query();
    if (!scan() || !CommitScanIdentity(disc, before, query())) {
        // A retry must not inherit repaired-TOC flags or partial tracks from
        // the failed attempt, particularly if it now sees a different disc.
        disc = Disc{};
        return false;
    }
    return true;
}
