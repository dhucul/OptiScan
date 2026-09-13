#pragma once
#include "CDStructures.h"
#include <chrono>

namespace Pregaps {
struct Boundary { DWORD start = 0; bool verified = false; };

template<class Read>
void SetFirstTrackBoundary(TrackInfo& track, bool trustedToc, Read read) {
    track.pregapLBA = 0;
    track.index01LBA = track.startLBA;
    track.pregapVerified = trustedToc;
    int qt = 0, qi = -1;
    if (trustedToc && read(track.startLBA, qt, qi) && (qt != track.trackNumber || qi != 1))
        track.pregapVerified = false; // retain audio, but expose contradictory evidence
}

// TOC-less recovery can start at INDEX 00. Metadata packets within that
// interval are normal; missing evidence at the transition is still unknown.
template<class Read, class Stop>
Boundary FindIndex01(DWORD index00, DWORD end, int track, Read read, Stop stop) {
    Boundary unknown{index00, false};
    unsigned missing = 0;
    for (DWORD position = index00; position < end;) {
        ++position;
        if (stop()) return unknown;
        int qt = 0, qi = -1;
        if (!read(position, qt, qi)) { if (++missing > 8) return unknown; continue; }
        if (qt != track) return unknown;
        if (qi == 1) return missing ? unknown : Boundary{position, true};
        if (qi != 0) return unknown;
        missing = 0;
    }
    return unknown;
}

// Read must provide majority-voted, address-checked Q positions. Walking back
// from INDEX 01 supports even one-frame gaps and has no fixed gap-length cap.
// Non-position Q packets (e.g. MCN/ISRC) can occur inside a gap. Bridge short
// holes only within an established interval; a hole at its left edge leaves
// the boundary unknown rather than guessing which missing frame starts it.
template<class Read, class Stop>
Boundary FindBoundary(DWORD index01, DWORD previousIndex01, int track,
    int previousTrack, Read read, Stop stop) {
    Boundary result{index01, false};
    if (index01 <= previousIndex01 || stop()) return result;
    int qt = 0, qi = -1;
    // The trusted TOC supplies INDEX 01. Position Q, when available there,
    // must agree; a metadata packet need not contain position information.
    if (read(index01, qt, qi) && (qt != track || qi != 1)) return result;
    DWORD beginning = index01;
    unsigned missing = 0;
    for (DWORD position = index01 - 1;; --position) {
        if (stop()) return result;
        if (!read(position, qt, qi)) {
            if (++missing > 8 || position == previousIndex01) return result;
            continue;
        }
        if (qt == track && qi == 0) {
            beginning = position;
            missing = 0;
        } else if (qt == previousTrack && qi >= 1) {
            if (missing) return result; // the transition is inside an unreadable span
            return {beginning, true};
        } else {
            return result;
        }
        if (position == previousIndex01) return result;
    }
}

inline bool AllVerified(const DiscInfo& disc) {
    for (const auto& track : disc.tracks)
        if (track.isAudio && (disc.selectedSession == 0 || track.session == disc.selectedSession) &&
            !track.pregapVerified) return false;
    return true;
}
}
