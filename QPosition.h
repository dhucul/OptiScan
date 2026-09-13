#pragma once
#include "Constants.h"
#include <cstddef>
#include <array>

template<class Read>
bool ReadPositionMajority(DWORD lba, int& track, int& index, Read read) {
    struct Position { int track, index; };
    std::array<Position, 3> votes{};
    size_t valid = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        int qt = 0, qi = -1;
        if (read(lba, qt, qi)) votes[valid++] = {qt, qi};
    }
    for (size_t i = 0; i < valid; ++i) {
        int count = 0;
        for (size_t j = 0; j < valid; ++j)
            if (votes[j].track == votes[i].track && votes[j].index == votes[i].index) ++count;
        if (count >= 2) { track = votes[i].track; index = votes[i].index; return true; }
    }
    return false;
}

// CRC, BCD and absolute address must all agree. Repeated stale Q packets
// cannot establish a boundary merely by returning the same track/index.
inline bool DecodePositionQ(const BYTE* q, DWORD requestedLBA, int& track, int& index, bool optionalCrc = false) {
    if ((q[0] & 0x0F) != 1 || q[6] != 0) return false;
    const auto bcd = [](BYTE value) { return (value & 15) <= 9 && (value >> 4) <= 9; };
    if ((q[1] != 0xAA && !bcd(q[1])) || !bcd(q[2]) ||
        !bcd(q[7]) || !bcd(q[8]) || !bcd(q[9])) return false;
    const int seconds = BcdToBin(q[8]), frames = BcdToBin(q[9]);
    if (seconds >= 60 || frames >= 75) return false;
    const uint16_t calculated = SubchannelCRC16(q, 10);
    const uint16_t stored = (static_cast<uint16_t>(q[10]) << 8) | q[11];
    if (!(optionalCrc && stored == 0) && calculated != stored &&
        static_cast<uint16_t>(~calculated) != stored) return false;
    const int64_t address = static_cast<int64_t>(BcdToBin(q[7])) * 4500 + seconds * 75 + frames - 150;
    if (address != static_cast<int32_t>(requestedLBA)) return false;
    const int decodedTrack = q[1] == 0xAA ? 0xAA : BcdToBin(q[1]);
    if (decodedTrack == 0) return false;
    track = decodedTrack; index = BcdToBin(q[2]);
    return true;
}

inline bool DecodeFormattedPositionQ(const BYTE* data, size_t size, DWORD lba, int& track, int& index) {
    // MMC READ CD formatted Q: the packet begins at byte zero; CRC may be
    // omitted as 00/00. Bytes 12..14 and bits 6..0 of byte 15 are padding.
    if (size < 16 || data[12] || data[13] || data[14] || (data[15] & 0x7F)) return false;
    return DecodePositionQ(data, lba, track, index, true);
}
