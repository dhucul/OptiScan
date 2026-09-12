#pragma once
#include <windows.h>
#include <chrono>
#include <cstdint>
#include <istream>
#include "InterruptHandler.h"

namespace WorkflowChecks {
template<class Action>
bool RunMediaAction(Action action) {
    if (g_interrupt.IsInterrupted()) return false;
    return action();
}
template<class Votes>
bool HasUniqueQuorum(const Votes& votes, int quorum) {
    int highest = 0, winners = 0;
    for (const auto& value : votes) {
        if (value.second > highest) { highest = value.second; winners = 1; }
        else if (value.second == highest) ++winners;
    }
    return highest > 0 && highest >= quorum && winners == 1;
}
inline bool RewindWriteSource(std::istream& source, uint64_t sector, uint64_t stride) {
    source.clear();
    source.seekg(static_cast<std::streamoff>(sector * stride));
    return source.good();
}
inline bool MediaStateKnown(bool commandOk, BYTE sense, BYTE asc) {
    return commandOk || (sense == 0x02 && (asc == 0x3A || asc == 0x04));
}
inline bool DiscSessionComplete(const BYTE* data, size_t size) {
    return size >= 3 && ((static_cast<unsigned>(data[0]) << 8) | data[1]) >= 1 &&
        (data[2] & 3) == 2 && ((data[2] >> 2) & 3) == 3;
}
inline bool SimulationRequested() {
    char value[8]{};
    const DWORD length = GetEnvironmentVariableA("OPTISCAN_SIMULATE_WRITE", value, sizeof(value));
    return length > 0 && length < sizeof(value) && value[0] != '0';
}
class ScanProgressWatch {
    bool observed = false;
    DWORD highest = 0;
    std::chrono::steady_clock::time_point advanced;
public:
    explicit ScanProgressWatch(std::chrono::steady_clock::time_point start) : advanced(start) {}
    bool Stalled(DWORD position, bool done, std::chrono::steady_clock::time_point now) {
        if (done) return false;
        if (!observed || position > highest) {
            observed = true;
            highest = position;
            advanced = now;
        }
        return now - advanced >= std::chrono::seconds(30);
    }
};
}
