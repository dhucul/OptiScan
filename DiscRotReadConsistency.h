#pragma once
#include "Constants.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace DiscRot {
using AudioRanges = std::vector<std::pair<DWORD, DWORD>>;

struct Phase1SectorResult {
    bool readable = false;
    bool cancelled = false;
    bool observedC2 = false;
    bool recoveredReadFailure = false;
    bool verificationReadFailure = false;
    bool transientC2 = false;
    int c2Errors = 0;

    bool HasUnrecoveredFailure() const { return !readable || verificationReadFailure; }
};

// Keep initial/retry/verification outcomes distinct. The caller records this
// result once, only after checking cancellation. Read callbacks fill one audio
// sector plus its C2 count; failed commands do not supply valid C2 evidence.
template<class ReadC2, class Evict, class Cancelled>
Phase1SectorResult ReadPhase1Sector(DWORD lba, bool pioneer,
    ReadC2 readC2, Evict evict, Cancelled cancelled) {
    Phase1SectorResult result;
    auto stop = [&]() { result.cancelled = cancelled(); return result.cancelled; };
    if (stop()) return result;
    std::array<BYTE, AUDIO_SECTOR_SIZE> audio{}, verify{};
    result.readable = readC2(lba, audio.data(), result.c2Errors);
    if (stop()) return result;
    if (!result.readable && pioneer) {
        evict(lba);
        if (stop()) return result;
        result.c2Errors = 0;
        result.readable = readC2(lba, audio.data(), result.c2Errors);
        if (stop()) return result;
        result.recoveredReadFailure = result.readable;
    }
    if (!result.readable) { result.c2Errors = 0; return result; }
    result.observedC2 = result.c2Errors > 0;
    if (pioneer && result.observedC2) {
        const bool cacheCleared = evict(lba);
        if (stop()) return result;
        int verifyC2 = 0;
        const bool verified = readC2(lba, verify.data(), verifyC2);
        if (stop()) return result;
        if (!verified) result.verificationReadFailure = true;
        else if (verifyC2 == 0 && cacheCleared && audio == verify) {
            result.c2Errors = 0;
            result.transientC2 = true;
        }
        else result.c2Errors = std::max(result.c2Errors, verifyC2);
    }
    return result;
}

inline AudioRanges NormalizeAudioRanges(AudioRanges ranges) {
    std::sort(ranges.begin(), ranges.end());
    AudioRanges merged;
    for (const auto& range : ranges) {
        if (range.second < range.first) continue;
        if (!merged.empty() && std::uint64_t{range.first} <= std::uint64_t{merged.back().second} + 1)
            merged.back().second = std::max(merged.back().second, range.second);
        else merged.push_back(range);
    }
    return merged;
}

// Read more unique audio bytes than the drive's reported buffer holds. A seek
// alone does not evict audio. Accurate Stream says nothing about caching.
// Unknown capacity, insufficient audio, failed reads and cancellation must not
// qualify matching rereads as independent. Keep away from target read-ahead.
template<class ReadBlock, class Cancelled>
bool EvictAudioCacheRange(DWORD first, DWORD last, const AudioRanges& ranges, int bufferSizeKB,
    ReadBlock readBlock, Cancelled cancelled) {
    if (bufferSizeKB <= 0 || last < first) return false;
    const std::uint64_t required = std::uint64_t{static_cast<unsigned>(bufferSizeKB)} * 1024 / AUDIO_SECTOR_SIZE + 1;
    const std::uint64_t excludedStart = first > 75 ? first - 75 : 0;
    const std::uint64_t excludedEnd = std::uint64_t{last} + 76;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
    std::uint64_t available = 0;
    for (const auto& range : ranges) {
        const std::uint64_t begin = range.first, end = std::uint64_t{range.second} + 1;
        auto add = [&](std::uint64_t lo, std::uint64_t hi) {
            if (hi > lo) { spans.emplace_back(lo, hi); available += hi - lo; }
        };
        add(begin, std::min(end, excludedStart));
        add(std::max(begin, excludedEnd), end);
    }
    if (available < required) return false;
    constexpr DWORD blockSize = 24;
    std::array<BYTE, blockSize * AUDIO_SECTOR_SIZE> discard{};
    std::uint64_t remaining = required;
    for (const auto& span : spans) {
        for (auto lba = span.first; lba < span.second && remaining > 0;) {
            if (cancelled()) return false;
            const DWORD count = static_cast<DWORD>(std::min<std::uint64_t>(blockSize,
                std::min(remaining, span.second - lba)));
            if (!readBlock(static_cast<DWORD>(lba), count, discard.data())) return false;
            remaining -= count;
            lba += count;
        }
        if (remaining == 0) return true;
    }
    return false;
}

template<class ReadBlock, class Cancelled>
bool EvictAudioCache(DWORD target, const AudioRanges& ranges, int bufferSizeKB,
    ReadBlock readBlock, Cancelled cancelled) {
    return EvictAudioCacheRange(target, target, ranges, bufferSizeKB, readBlock, cancelled);
}

struct ConsistencyResult {
    bool readable = true;
    bool cacheCleared = false;
    int mismatches = 0;
};

template<class ReadSector, class Evict, class Cancelled>
ConsistencyResult CheckReadConsistency(DWORD lba, int passes,
    ReadSector readSector, Evict evict, Cancelled cancelled) {
    ConsistencyResult result;
    if (passes < 2 || cancelled()) return result;
    std::array<BYTE, AUDIO_SECTOR_SIZE> reference{}, compare{};
    if (!readSector(lba, reference.data())) { result.readable = false; return result; }
    result.cacheCleared = true;
    for (int pass = 1; pass < passes; ++pass) {
        if (cancelled()) { result.cacheCleared = false; return result; }
        // Always attempt eviction, including on Accurate Stream drives.
        const bool cleared = evict(lba);
        result.cacheCleared = result.cacheCleared && cleared;
        if (cancelled()) { result.cacheCleared = false; return result; }
        if (!readSector(lba, compare.data())) { result.readable = false; return result; }
        if (std::memcmp(reference.data(), compare.data(), reference.size()) != 0)
            ++result.mismatches;
    }
    return result;
}
} // namespace DiscRot
