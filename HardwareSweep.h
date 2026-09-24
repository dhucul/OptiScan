#pragma once
#include "DiscRotReadConsistency.h"
#include "QualityScanSession.h"
#include "ScanResults.h"
#include <optional>
#include <limits>
#include <iomanip>
#include <ostream>
#include <map>

namespace Diagnostics {
inline constexpr int kHardwareSweepSamples = 15;

// Select the same bounded, contiguous audio window for every speed. Never
// extend cache-eviction reads into that window or across mixed-mode gaps.
inline std::optional<std::pair<DWORD,DWORD>> HardwareSweepRange(
    const DiscRot::AudioRanges& ranges, DWORD preferredStart) {
    constexpr std::uint64_t sectors = kHardwareSweepSamples * 75;
    std::optional<std::pair<DWORD,DWORD>> best;
    std::uint64_t bestDistance = UINT64_MAX;
    for (const auto& range : ranges) {
        if (range.second < range.first || std::uint64_t{range.second}-range.first+1 < sectors) continue;
        const DWORD latest = static_cast<DWORD>(std::uint64_t{range.second}+1-sectors);
        const DWORD start = std::clamp(preferredStart, range.first, latest);
        const std::uint64_t distance = start > preferredStart ? start-preferredStart : preferredStart-start;
        if (!best || distance < bestDistance) {
            best = {{start, static_cast<DWORD>(std::uint64_t{start}+sectors-1)}};
            bestDistance = distance;
        }
    }
    return best;
}

struct HardwareSweepSample {
    DWORD lba = 0, sectors = 0;
    int c1 = 0, secondStage = 0, cu = 0;
    bool valid = true, done = false;
};

struct HardwareSweepPass {
    bool attempted = false, complete = false, cancelled = false, cleanupFailed = false;
    bool cacheCleared = false;
    HardwareSpeedEvidence speed;
    ScanThroughput throughput;
    std::vector<HardwareSweepSample> observations;
    std::vector<std::uint64_t> elapsedMs;
    std::vector<ScanQuality::C1Interval> intervals;
    ScanQuality::C1Statistics c1;
    long long secondStageTotal = 0, cuTotal = 0;
    std::string limitation = "not measured";
    bool Qualified() const { return complete && c1.RateAvailable(); }
    bool HasCounterActivity() const { return c1.total > 0 || secondStageTotal > 0 || cuTotal > 0; }
    int ActualSpeed() const { return speed.ActualSpeed(); }
};

// Evict BEFORE entering scan mode: initialization resets counters accumulated
// during eviction. Speed evidence is captured after start and after each poll,
// never after stop or from the earlier timing sweep. Failed cleanup forbids
// subsequent drive commands; the caller closes the handle.
template<class Evict, class Start, class Poll, class Stop, class ReadSpeed, class Cancelled, class Now>
HardwareSweepPass MeasureHardwareSweep(DWORD first, DWORD last, Evict evict,
    Start start, Poll poll, Stop stop, ReadSpeed readSpeed, Cancelled cancelled, Now nowMs,
    std::function<void(const HardwareSweepPass&)> progressUpdate = {}) {
    HardwareSweepPass result;
    if (cancelled()) { result.cancelled = true; result.limitation = "cancelled"; return result; }
    result.cacheCleared = evict();
    if (cancelled()) { result.cancelled = true; result.limitation = "cancelled during cache eviction"; return result; }
    result.attempted = true;
    QualityScanSession session(stop);
    if (!start(first,last)) {
        result.cleanupFailed = !session.Stop();
        result.limitation = result.cleanupFailed ? "scan cleanup failed" : "scan could not start";
        return result;
    }
    auto captureSpeed = [&] { WORD kb=0; const bool ok=readSpeed(kb); result.speed.Record(ok,kb); };
    DWORD progress=DWORD(-1);
    QualitySampleSequence sequence(first,last);
    result.throughput.Begin(nowMs());
    auto lastProgress = nowMs();
    bool failed=false;
    if (!cancelled()) captureSpeed();
    while (result.intervals.size() < kHardwareSweepSamples) {
        if (cancelled()) { result.cancelled=true; break; }
        HardwareSweepSample sample;
        if (!poll(sample)) { failed=true; result.limitation="hardware poll failed"; break; }
        if (cancelled()) { result.cancelled=true; break; }
        captureSpeed();
        const auto now=nowMs();
        if (progress==DWORD(-1) || sample.lba>progress) { progress=sample.lba;lastProgress=now; }
        else if (now-lastProgress >= 30000) { failed=true;result.limitation="scan stalled";break; }
        const auto decision=sequence.Observe(sample.lba,sample.sectors,sample.c1,
            sample.secondStage,sample.cu,sample.valid);
        if (decision==QualitySampleDecision::Invalid) {
            failed=true;result.limitation="invalid sample range, order or counter";break;
        }
        if (decision==QualitySampleDecision::Ignore) {if(sample.done) break;continue;}
        result.throughput.Observe(sample.sectors,now);
        result.observations.push_back(sample);
        result.elapsedMs.push_back(now-result.throughput.startMs);
        result.intervals.push_back({sample.lba,sample.sectors,sample.c1});
        result.secondStageTotal += sample.secondStage;
        result.cuTotal += sample.cu;
        if (progressUpdate) progressUpdate(result);
        if (sample.done) break;
    }
    result.throughput.Finish(nowMs());
    result.cleanupFailed = !session.Stop();
    result.cancelled = result.cancelled || cancelled();
    result.complete = !failed && !result.cancelled && !result.cleanupFailed &&
        result.intervals.size() >= kHardwareSweepSamples;
    result.c1 = ScanQuality::SummarizeC1(result.intervals, false);
    // Zero-only counters can be genuine, but cannot establish decoder activity.
    // Keep raw zeros visible without awarding an EXCELLENT measurement band.
    const bool counterActivity=result.HasCounterActivity();
    const bool trusted=result.complete && result.cacheCleared && result.speed.Stable() && counterActivity;
    result.c1 = ScanQuality::SummarizeC1(result.intervals, trusted);
    if (result.cleanupFailed) result.limitation="scan cleanup failed";
    else if (result.cancelled) result.limitation="cancelled";
    else if (!failed) {
        if (!result.complete) result.limitation="incomplete sample coverage";
        else if (!result.cacheCleared) result.limitation="cache eviction could not be established";
        else if (!result.speed.Stable()) result.limitation="hardware-phase speed missing or changed";
        else if (!counterActivity) result.limitation="zero-only counters; decoder activity unverified";
        else if (!result.c1.RateAvailable()) result.limitation="counter duration or ordering unverified";
        else result.limitation.clear();
    }
    return result;
}

inline void PrintHardwareSweepEvidence(std::ostream& out, const HardwareSweepPass& pass,
    int requestedSpeed, int timingSpeed, const char* secondStage, const char* indent="    ") {
    const auto flags=out.flags(); const auto precision=out.precision();
    out<<indent<<"Requested: "<<requestedSpeed<<"x; hardware-phase speed: ";
    if (pass.speed.Stable()) out<<"~"<<pass.ActualSpeed()<<"x (drive-reported during this pass)\n";
    else out<<"UNVERIFIED\n";
    out<<indent<<"Cache eviction: "<<(pass.cacheCleared ? "completed before scan start" : "UNVERIFIED")<<'\n';
    if (!pass.Qualified()) out<<indent<<"NOT RATED - "<<pass.limitation<<'\n';
    else if (pass.ActualSpeed()!=timingSpeed)
        out<<indent<<"Excluded from speed comparison: hardware and timing phases ran at different/unverified speeds.\n";
    PrintScanTelemetry(out,pass.speed,pass.throughput,indent);
    ScanQuality::PrintC1Summary(out,pass.c1,0,indent);
    out<<indent<<secondStage<<" observed total: ";
    if (pass.intervals.empty()) out<<"unavailable (no observations)";
    else out<<pass.secondStageTotal;
    if (pass.Qualified()) out<<"; average "<<std::fixed<<std::setprecision(2)
        <<pass.secondStageTotal/pass.c1.MeasuredSeconds()<<"/sec of measured audio\n";
    else out<<"; rate unavailable (unverified measurement)\n";
    if (pass.cuTotal>0) out<<indent<<"CU observed total: "<<pass.cuTotal<<'\n';
    out.flags(flags);out.precision(precision);
}

struct MeasuredSpeedGroup {
    int speed = 0; // Zero keeps missing/unstable readbacks separate from measured speeds.
    std::vector<size_t> rows;
};

inline std::vector<MeasuredSpeedGroup> GroupMeasuredSpeeds(const std::vector<int>& speeds) {
    std::map<int,std::vector<size_t>> measured;
    std::vector<size_t> unknown;
    for (size_t i=0;i<speeds.size();++i) {
        if (speeds[i]>0) measured[speeds[i]].push_back(i);
        else unknown.push_back(i);
    }
    std::vector<MeasuredSpeedGroup> groups;
    for (const auto& [speed,rows] : measured) groups.push_back({speed,rows});
    if (!unknown.empty()) groups.push_back({0,unknown});
    return groups;
}

inline void PrintMeasuredSpeedHeading(std::ostream& out, const MeasuredSpeedGroup& group,
    const char* description) {
    const auto flags=out.flags();
    out<<std::dec<<"\n  ";
    if (group.speed>0) {
        out<<"~"<<group.speed<<"x "<<description<<" - "
            <<(group.rows.size()>1 ? "repeated measurements" : "single measurement");
    }
    else out<<description<<" - actual speed UNVERIFIED";
    out<<" ("<<group.rows.size()<<" pass"<<(group.rows.size()==1 ? "" : "es")<<")\n";
    out.flags(flags);
}

struct HardwareSweepReportRow {
    const HardwareSweepPass& pass;
    int requestedSpeed;
    int timingSpeed;
    bool timingCompared = true;
    bool primaryCompared = true;
    bool variationFlag = false;
};

// Reporting only: group captures by their own hardware-phase readback, retain
// acquisition order within each group, and never pool away per-pass warnings.
inline void PrintHardwareSweepGroups(std::ostream& out,
    const std::vector<HardwareSweepReportRow>& rows, const char* secondStage) {
    const auto flags=out.flags();const auto precision=out.precision();
    out<<std::dec;
    std::vector<int> speeds;
    std::vector<size_t> attempted,notRun;
    for (size_t i=0;i<rows.size();++i) {
        if (rows[i].pass.attempted) {attempted.push_back(i);speeds.push_back(rows[i].pass.ActualSpeed());}
        else notRun.push_back(i);
    }
    for (const auto& group : GroupMeasuredSpeeds(speeds)) {
        PrintMeasuredSpeedHeading(out,group,"hardware observations");
        if (group.speed>0 && group.rows.size()>1) {
            out<<"    These are repeat reads at the same approximate reported speed.\n"
                <<"    Differences within this group do not establish a speed effect.\n";
            bool excellent=false,good=false,positive=false,zero=false;
            out<<"    C1 by pass: ";
            for (size_t i=0;i<group.rows.size();++i) {
                const auto& pass=rows[attempted[group.rows[i]]].pass;
                if (i>0) out<<"; ";
                if (pass.Qualified()) {
                    const auto band=pass.c1.Rating();
                    excellent=excellent || band==ScanQuality::C1Rating::Excellent;
                    good=good || band==ScanQuality::C1Rating::Good;
                    out<<std::fixed<<std::setprecision(2)<<pass.c1.average<<"/sec ["
                        <<ScanQuality::C1RatingName(band)<<"]";
                }
                else out<<"NOT RATED";
            }
            out<<"\n    "<<secondStage<<" raw totals by pass: ";
            for (size_t i=0;i<group.rows.size();++i) {
                const auto& pass=rows[attempted[group.rows[i]]].pass;
                if (i>0) out<<"; ";
                if (pass.intervals.empty()) out<<"unavailable";
                else {
                    out<<pass.secondStageTotal;
                    if (!pass.Qualified()) out<<" (unrated)";
                    positive=positive || pass.secondStageTotal>0;
                    zero=zero || pass.secondStageTotal==0;
                }
            }
            out<<'\n';
            if (positive && zero)
                out<<"    A zero count in one pass does not cancel the positive observation in another.\n";
            if (excellent && good) {
                out<<"    C1 label explanation: EXCELLENT is below "<<std::fixed<<std::setprecision(2)
                    <<ScanQuality::kC1ExcellentLimit<<"/sec; GOOD is "<<ScanQuality::kC1ExcellentLimit
                    <<" to below "<<ScanQuality::kC1ElevatedLimit<<"/sec.\n"
                    <<"    The EXCELLENT/GOOD change crosses that numeric boundary. It does not, by itself,\n"
                    <<"    show that the disc deteriorated or that a higher requested speed made it worse.\n";
            }
            out<<"    Counts, coverage, limitations and C1 bands below belong to each individual pass.\n";
        }
        for (size_t i=0;i<group.rows.size();++i) {
            const auto& row=rows[attempted[group.rows[i]]];
            out<<"\n    Pass "<<i+1<<":\n";
            if (!row.timingCompared)
                out<<"      Excluded from speed comparison: timing speed or coverage unverified.\n";
            PrintHardwareSweepEvidence(out,row.pass,row.requestedSpeed,row.timingSpeed,secondStage,"      ");
            if (row.timingCompared && row.pass.Qualified() && row.pass.ActualSpeed()==row.timingSpeed) {
                if (row.variationFlag) out<<"      Measurement variation flagged; cause unconfirmed.\n";
                else if (!row.primaryCompared) out<<"      Wider verified range only.\n";
            }
        }
    }
    if (!notRun.empty()) {
        out<<"\n  Hardware settings without a recorded pass:\n";
        for (size_t i : notRun)
            out<<"    Requested "<<rows[i].requestedSpeed<<"x: NOT MEASURED - "<<rows[i].pass.limitation<<'\n';
    }
    out.flags(flags);out.precision(precision);
}
} // namespace Diagnostics
