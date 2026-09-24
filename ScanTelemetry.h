#pragma once
#include "Constants.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>

namespace Diagnostics {
inline std::uint64_t ScanNowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct HardwareSpeedEvidence {
    int readbacks = 0;
    bool missing = false;
    WORD minimumKB = 0, maximumKB = 0;
    std::uint64_t sumKB = 0;
    void Record(bool ok, WORD kb) {
        if (!ok || kb == 0 || kb == CD_SPEED_MAX) { missing = true; return; }
        if (readbacks == 0) minimumKB = maximumKB = kb;
        else { minimumKB = (std::min)(minimumKB,kb); maximumKB = (std::max)(maximumKB,kb); }
        sumKB += kb;
        ++readbacks;
    }
    bool Stable() const {
        return !missing && readbacks >= 2 && maximumKB-minimumKB <= (std::max)(1,minimumKB/20);
    }
    int ActualSpeed() const {
        return Stable() ? static_cast<int>((sumKB/readbacks+CD_SPEED_1X/2)/CD_SPEED_1X) : 0;
    }
};

template<class Drive>
void CaptureScanSpeed(Drive& drive, HardwareSpeedEvidence& speed) {
    WORD read=0, write=0;
    const bool ok=drive.GetActualSpeed(read,write);
    speed.Record(ok,read);
}

// Cancellation may arrive while the synchronous MODE SENSE command runs,
// including after the final counter sample. Check both sides of that command.
template<class Drive, class Cancelled>
bool CaptureScanSpeedChecked(Drive& drive, HardwareSpeedEvidence& speed, Cancelled cancelled) {
    if (cancelled()) return false;
    CaptureScanSpeed(drive,speed);
    return !cancelled();
}

// Count only newly accepted coverage. Wall time includes polling, retries and
// reporting overhead, but excludes preparation before Begin and cleanup after
// Finish. This measures delivered scan throughput, never independent spindle RPM.
struct ScanThroughput {
    bool started=false, finished=false, coverageKnown=true, clockValid=true;
    std::uint64_t startMs=0, endMs=0, sectors=0;
    std::deque<std::pair<std::uint64_t,std::uint64_t>> recent;
    void Begin(std::uint64_t now) {
        *this=ScanThroughput{};
        started=true;startMs=endMs=now;recent.emplace_back(now,0);
    }
    void Observe(DWORD covered, std::uint64_t now) {
        if (!started || finished) return;
        if (now<endMs) {clockValid=false;return;}
        endMs=now;
        if (covered==0) coverageKnown=false;
        sectors+=covered;
        recent.emplace_back(now,sectors);
        while (recent.size()>2 && now-recent[1].first>=2000) recent.pop_front();
    }
    void Finish(std::uint64_t now) {
        if (!started || finished) return;
        if (now<endMs) clockValid=false;
        else endMs=now;
        finished=true;
    }
    double ElapsedSeconds() const { return started && clockValid ? (endMs-startMs)/1000.0 : 0.0; }
    double AverageX() const {
        return coverageKnown && sectors>0 && ElapsedSeconds()>0
            ? (sectors/75.0)/ElapsedSeconds() : 0.0;
    }
    double CurrentX() const {
        if (!coverageKnown || !clockValid || recent.empty() || endMs<=recent.front().first) return 0.0;
        return ((sectors-recent.front().second)/75.0)*1000.0/(endMs-recent.front().first);
    }
};

inline std::string ScanSpeedText(double speed) {
    if (!(speed>0)) return "unavailable";
    std::ostringstream out;out<<std::fixed<<std::setprecision(1)<<speed;
    auto value=out.str();
    if (value.size()>2 && value.ends_with(".0")) value.resize(value.size()-2);
    return value+"x";
}

inline void PrintScanTelemetry(std::ostream& out, const HardwareSpeedEvidence& speed,
    const ScanThroughput& throughput, const char* indent="  ") {
    const auto flags=out.flags();const auto precision=out.precision();out<<std::dec;
    out<<indent<<"Drive-reported speed during scan: ";
    if (speed.Stable()) out<<"~"<<speed.ActualSpeed()<<"x\n";
    else out<<"UNVERIFIED (missing or changing readback)\n";
    out<<indent<<"Measured scan throughput (average): "<<ScanSpeedText(throughput.AverageX())<<'\n';
    out<<indent<<"Scan elapsed time: "<<std::fixed<<std::setprecision(3)<<throughput.ElapsedSeconds()<<" seconds\n";
    out<<indent<<"Throughput = measured audio / elapsed time; includes command overhead, not spindle speed.\n";
    out.flags(flags);out.precision(precision);
}

enum class QualitySampleDecision { Ignore, Accept, Invalid };
struct QualitySampleSequence {
    DWORD first=0, last=0, previous=0;
    std::uint64_t previousEnd=0;
    bool havePrevious=false;
    QualitySampleSequence(DWORD begin,DWORD end):first(begin),last(end){}
    QualitySampleDecision Observe(DWORD lba,DWORD sectors,int c1,int c2,int cu,bool valid) {
        if (!valid) return QualitySampleDecision::Ignore;
        if (lba<first || lba>last || c1<0 || c2<0 || cu<0 ||
            (sectors>0 && std::uint64_t{lba}+sectors>std::uint64_t{last}+1))
            return QualitySampleDecision::Invalid;
        if (havePrevious && lba==previous) return QualitySampleDecision::Ignore;
        if (havePrevious && (lba<previous || lba<previousEnd)) return QualitySampleDecision::Invalid;
        havePrevious=true;previous=lba;previousEnd=std::uint64_t{lba}+sectors;
        // Unknown duration with valid positive counters is still evidence.
        return QualitySampleDecision::Accept;
    }
};

template<class Disc>
std::string ScanDiscIdentity(const Disc& disc) {
    // A layout key identifies comparable TOCs, not the identity of audio data.
    std::uint64_t key=14695981039346656037ULL;
    auto add=[&](std::uint64_t value) {for(int i=0;i<8;++i) {key^=(value>>(i*8))&255;key*=1099511628211ULL;}};
    add(disc.leadOutLBA);add(disc.tracks.size());
    for(const auto& track:disc.tracks) {add(track.trackNumber);add(track.startLBA);add(track.endLBA);add(track.isAudio);}
    std::ostringstream out;out<<disc.tracks.size()<<" tracks; lead-out LBA "<<disc.leadOutLBA
        <<"; layout key "<<std::hex<<std::setw(16)<<std::setfill('0')<<key;
    return out.str();
}
} // namespace Diagnostics
