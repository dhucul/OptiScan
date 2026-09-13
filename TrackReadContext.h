#pragma once
#include "CDStructures.h"
#include <algorithm>

struct TrackOutputSlice { size_t start = 0, count = 0; };

// Supply the existing offset corrector with physically contiguous audio.
// These are temporary read bounds, never changes to the source TOC or CRC math.
inline void AddTrackOffsetContext(DiscInfo& read, const DiscInfo& source,
    size_t firstSourceTrack, size_t lastSourceTrack) {
    if (read.driveOffset == 0 || read.tracks.empty()) return;
    read.pregapMode = PregapMode::Include;
    const int64_t offset = read.driveOffset;
    const uint64_t bytes = static_cast<uint64_t>(offset < 0 ? -offset : offset) * 4;
    const DWORD margin = static_cast<DWORD>((bytes + AUDIO_SECTOR_SIZE - 1) / AUDIO_SECTOR_SIZE);
    auto& first = read.tracks.front();
    if (offset < 0 && firstSourceTrack > 0 && firstSourceTrack < source.tracks.size()) {
        const auto& previous = source.tracks[firstSourceTrack - 1];
        if (first.isAudio && previous.isAudio && previous.endLBA + 1 == first.pregapLBA) {
            const DWORD wanted = first.startLBA > margin ? first.startLBA - margin : 0;
            first.pregapLBA = std::min(first.pregapLBA, std::max(previous.pregapLBA, wanted));
        }
    }
    auto& last = read.tracks.back();
    if (offset > 0 && lastSourceTrack + 1 < source.tracks.size()) {
        const auto& next = source.tracks[lastSourceTrack + 1];
        if (last.isAudio && next.isAudio && last.endLBA + 1 == next.pregapLBA) {
            last.endLBA = static_cast<DWORD>(std::min<uint64_t>(
                static_cast<uint64_t>(last.endLBA) + margin, next.endLBA));
        }
    }
}

inline bool BuildTrackOutputSlices(const DiscInfo& read, const std::vector<TrackInfo>& bodies,
    std::vector<TrackOutputSlice>& slices) {
    slices.clear();
    if (read.tracks.size() != bodies.size()) return false;
    size_t cursor = 0;
    for (size_t i = 0; i < bodies.size(); ++i) {
        const auto& track = read.tracks[i];
        const auto& body = bodies[i];
        const DWORD begin = read.pregapMode == PregapMode::Skip ? track.startLBA : track.pregapLBA;
        if (track.endLBA < begin || body.endLBA < body.startLBA ||
            body.startLBA < begin || body.endLBA > track.endLBA) return false;
        const size_t readCount = static_cast<size_t>(track.endLBA) - begin + 1;
        if (cursor > read.rawSectors.size() || readCount > read.rawSectors.size() - cursor) return false;
        slices.push_back({cursor + body.startLBA - begin,
            static_cast<size_t>(body.endLBA) - body.startLBA + 1});
        cursor += readCount;
    }
    return cursor == read.rawSectors.size();
}

// Once file output/physical comparison is finished, remove only the added
// outside context so later diagnostics see real pregap and track boundaries.
inline bool RemoveTrackOffsetContext(DiscInfo& read, const std::vector<TrackInfo>& bodies) {
    if (read.driveOffset == 0) return true;
    if (read.tracks.empty() || read.tracks.size() != bodies.size() ||
        read.tracks.front().pregapLBA > bodies.front().pregapLBA ||
        read.tracks.back().endLBA < bodies.back().endLBA) return false;
    const size_t before = bodies.front().pregapLBA - read.tracks.front().pregapLBA;
    const size_t after = read.tracks.back().endLBA - bodies.back().endLBA;
    if (before > read.rawSectors.size() || after > read.rawSectors.size() - before) return false;
    read.rawSectors.resize(read.rawSectors.size() - after);
    read.rawSectors.erase(read.rawSectors.begin(), read.rawSectors.begin() + before);
    read.tracks = bodies;
    return true;
}
