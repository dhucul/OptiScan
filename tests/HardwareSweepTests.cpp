#define NOMINMAX
#include "../HardwareSweep.h"
#include "../DiscBalanceAssessment.h"
#include <deque>
#include <unordered_set>
#include <sstream>
#include <iostream>
#include <stdexcept>

namespace {
struct SweepDrive {
    static constexpr DWORD first=1000,last=2124;
    static constexpr int bufferKB=4096;
    std::deque<DWORD> fifo;
    std::unordered_set<DWORD> cached;
    std::vector<std::string> events;
    int speedX=16, pollCount=0, speedCount=0, stopCount=0;
    int failPollAt=-1, missingSpeedAt=-1, changeSpeedAt=-1;
    int unknownDurationAt=-1;
    bool allDurationsUnknown=false;
    int cancelAt=0; // 1 eviction, 2 start, 3 poll
    bool active=false,cancelled=false,zeroCounters=false,startFails=false,stopFails=false;
    bool repeatPosition=false,evictionFails=false;
    std::uint64_t clock=0;
    bool Read(DWORD lba) {
        if(cached.count(lba)) return false;
        cached.insert(lba);fifo.push_back(lba);
        const size_t capacity=bufferKB*1024/AUDIO_SECTOR_SIZE;
        while(fifo.size()>capacity) {cached.erase(fifo.front());fifo.pop_front();}
        return true;
    }
    void ResetPass(int speed) {speedX=speed;pollCount=speedCount=stopCount=0;events.clear();active=false;cancelled=false;clock=0;}
    Diagnostics::HardwareSweepPass Run(int reportedBuffer=bufferKB) {
        return Diagnostics::MeasureHardwareSweep(first,last,
            [&] {
                events.push_back("evict");
                return DiscRot::EvictAudioCacheRange(first,last,{{0,6000}},reportedBuffer,
                    [&](DWORD start,DWORD count,BYTE*) {
                        if(active) throw std::runtime_error("Eviction while counters active");
                        if(evictionFails) return false;
                        for(DWORD i=0;i<count;++i) {
                            if(start+i>=first-75 && start+i<=last+75) throw std::runtime_error("Eviction touched sampled window");
                            Read(start+i);
                        }
                        if(cancelAt==1) cancelled=true;
                        return true;
                    },[&]{return cancelled;});
            },
            [&](DWORD start,DWORD end) {
                events.push_back("start");active=true;
                if(start!=first || end!=last) throw std::runtime_error("Different sample range");
                if(cancelAt==2) cancelled=true;
                return !startFails;
            },
            [&](Diagnostics::HardwareSweepSample& sample) {
                events.push_back("poll");
                if(!active) throw std::runtime_error("Poll outside active scan");
                const int interval=pollCount++;
                if(interval==failPollAt) return false;
                sample.lba=first+(repeatPosition ? 0 : interval*75);
                sample.sectors=75;
                bool fresh=false;
                for(DWORD i=0;i<75;++i) fresh=Read(sample.lba+i)||fresh;
                sample.c1=fresh&&!zeroCounters ? 4 : 0;
                sample.secondStage=fresh&&!zeroCounters&&interval%3==0 ? 1 : 0;
                if(interval==unknownDurationAt || allDurationsUnknown) {
                    sample.sectors=0;sample.c1=9;sample.secondStage=23;sample.cu=7;
                }
                sample.done=interval==14 && !repeatPosition;
                if(cancelAt==3) cancelled=true;
                return true;
            },
            [&] {events.push_back("stop");++stopCount;if(stopFails)return false;active=false;speedX=40;return true;},
            [&](WORD& kb) {
                events.push_back("speed");
                if(!active) throw std::runtime_error("Speed captured after stop");
                const int query=speedCount++;
                if(query==missingSpeedAt)return false;
                if(query==changeSpeedAt)speedX=8;
                kb=static_cast<WORD>(speedX*CD_SPEED_1X);return true;
            },
            [&]{return cancelled;},
            [&]{clock+=repeatPosition?10001:1;return clock;});
    }
};
}

int RunHardwareSweepTests() {
    int failures=0;
    auto check=[&](bool ok,const char* label){std::cout<<(ok?"[PASS] ":"[FAIL] ")<<label<<'\n';if(!ok)++failures;};
    SweepDrive drive;
    drive.ResetPass(16);
    auto pass=drive.Run();
    check(pass.Qualified() && pass.c1.total==60 && pass.secondStageTotal==5 &&
        pass.ActualSpeed()==16 && pass.c1.measuredSectors==1125 && !drive.active,
        "Hardware sweep records fresh 15-second observations and hardware-phase speed");
    check(drive.events.front()=="evict" && drive.events[1]=="start" && drive.events[2]=="speed" &&
        drive.events.back()=="stop" && drive.speedCount==16 && drive.stopCount==1 && drive.speedX==40 && pass.ActualSpeed()==16,
        "Eviction precedes counter initialization and speed evidence precedes stop/reset");
    drive.ResetPass(32);
    auto cached=drive.Run(0);
    check(cached.c1.total==0 && cached.secondStageTotal==0 && !cached.Qualified() && !cached.cacheCleared,
        "Re-reading a cached region cannot convert idle zero counters into a rated clean pass");
    std::ostringstream log;
    Diagnostics::PrintHardwareSweepEvidence(log,cached,32,32,"C2");
    check(log.str().find("EXCELLENT")==std::string::npos && log.str().find("NOT RATED")!=std::string::npos &&
        log.str().find("cache eviction")!=std::string::npos,
        "Unverified cached zeros are retained without an EXCELLENT label");
    for(int speed : {4,8,16,24,32,40,40,32,24,16,8,4}) {
        drive.ResetPass(speed);
        auto fresh=drive.Run();
        check(fresh.Qualified() && fresh.c1.total==60 && fresh.secondStageTotal==5 && fresh.ActualSpeed()==speed,
            "Cache-cleared repeated-region counts are independent of forward/reverse sweep order");
    }
    for(int unknownAt : {0,10,14}) {
        drive.ResetPass(16);drive.unknownDurationAt=unknownAt;
        auto raw=drive.Run();
        const auto expectedC2=5-(unknownAt%3==0?1:0)+23;
        check(raw.complete && raw.intervals.size()==15 && raw.c1.total==65 &&
            raw.secondStageTotal==expectedC2 && raw.cuTotal==7 && raw.intervals[unknownAt].sectors==0 &&
            !raw.Qualified() && !raw.c1.timingKnown && raw.limitation.find("duration")!=std::string::npos,
            "Unknown-duration evidence is preserved at the first, middle and final sample without assigning a rate");
        std::ostringstream rawReport;
        Diagnostics::PrintHardwareSweepEvidence(rawReport,raw,16,16,"C2");
        check(rawReport.str().find("CU observed total: 7")!=std::string::npos &&
            rawReport.str().find("C2 observed total: "+std::to_string(expectedC2))!=std::string::npos &&
            rawReport.str().find("EXCELLENT")==std::string::npos && rawReport.str().find("NOT RATED")!=std::string::npos,
            "The report retains unknown-duration C2/CU counts and clearly withholds the rate");
    }
    drive.unknownDurationAt=-1;drive.allDurationsUnknown=true;drive.ResetPass(16);
    auto allRaw=drive.Run();
    check(allRaw.intervals.size()==15 && allRaw.c1.total==135 && allRaw.secondStageTotal==345 &&
        allRaw.cuTotal==105 && !allRaw.Qualified() && !allRaw.c1.timingKnown,
        "A wholly raw counter series loses no startup observations and never acquires a guessed duration");
    drive.allDurationsUnknown=false;
    drive.ResetPass(8);
    pass=drive.Run();log.str("");log.clear();
    Diagnostics::PrintHardwareSweepEvidence(log,pass,32,32,"C2");
    check(log.str().find("Requested: 32x; hardware-phase speed: ~8x")!=std::string::npos &&
        log.str().find("different/unverified speeds")!=std::string::npos && log.str().find("average 0.33/sec")!=std::string::npos,
        "Reporting uses hardware-phase speed and formats five C2 counts over 15 seconds clearly");
    drive.ResetPass(32);drive.zeroCounters=true;
    auto zero=drive.Run();log.str("");log.clear();
    Diagnostics::PrintHardwareSweepEvidence(log,zero,32,32,"C2");
    check(zero.complete && zero.cacheCleared && zero.speed.Stable() && !zero.Qualified() &&
        zero.limitation.find("zero-only")!=std::string::npos && log.str().find("EXCELLENT")==std::string::npos,
        "Even fresh zero-only counters leave decoder activity unverified rather than rated EXCELLENT");
    drive.zeroCounters=false;drive.ResetPass(32);drive.missingSpeedAt=5;
    auto missing=drive.Run();
    check(missing.complete && !missing.Qualified() && missing.c1.total==60 && missing.ActualSpeed()==0,
        "A missing in-pass speed readback invalidates comparison without discarding positive counts");
    drive.missingSpeedAt=-1;drive.ResetPass(32);drive.changeSpeedAt=8;
    auto changed=drive.Run();
    check(changed.complete && !changed.Qualified() && changed.limitation.find("speed")!=std::string::npos,
        "Mid-pass speed fallback cannot masquerade as a stable full-speed observation");
    drive.changeSpeedAt=-1;drive.ResetPass(16);drive.evictionFails=true;
    auto evictionFailed=drive.Run();
    check(evictionFailed.complete && !evictionFailed.Qualified() && !evictionFailed.cacheCleared,
        "Failed cache eviction leaves the full observed pass explicitly unverified");
    drive.evictionFails=false;drive.ResetPass(16);drive.failPollAt=5;
    auto failed=drive.Run();
    check(!failed.complete && !failed.Qualified() && failed.c1.total==20 && drive.stopCount==1 && !drive.active,
        "Polling failure retains positive partial evidence and always stops the session");
    drive.failPollAt=-1;drive.ResetPass(16);drive.repeatPosition=true;
    auto stalled=drive.Run();
    check(!stalled.complete && stalled.limitation=="scan stalled" && drive.pollCount<10 && drive.stopCount==1,
        "Repeated positions cannot keep a hardware pass alive indefinitely");
    drive.repeatPosition=false;
    for(int point : {1,2,3}) {
        drive.ResetPass(16);drive.cancelAt=point;
        auto cancelled=drive.Run();
        check(cancelled.cancelled && !cancelled.Qualified() && !drive.active &&
            drive.stopCount==(point==1 ? 0 : 1),
            "Cancellation during eviction, startup or polling preserves scan lifecycle boundaries");
    }
    drive.cancelAt=0;drive.ResetPass(16);drive.startFails=true;
    auto startFailed=drive.Run();
    check(!startFailed.complete && drive.pollCount==0 && drive.stopCount==1 && !drive.active,
        "Partially entered scan mode is cleaned up after a failed start");
    drive.startFails=false;drive.ResetPass(16);drive.stopFails=true;
    auto stopFailed=drive.Run();
    check(stopFailed.cleanupFailed && !stopFailed.Qualified() && drive.stopCount>=2 && drive.stopCount<=4,
        "Unconfirmed cleanup cannot publish rated observations or permit a following pass");
    const auto ranges=DiscRot::NormalizeAudioRanges({{0,1999},{4000,7999}});
    auto selected=Diagnostics::HardwareSweepRange(ranges,3000);
    check(selected && selected->first==4800 && selected->second==5924,
        "Hardware sweep windows remain in contiguous audio across mixed-mode gaps");
    check(!Diagnostics::HardwareSweepRange({{0,999}},750),
        "Short audio cannot fabricate a complete 15-second hardware window");
    int reads=0;
    check(!DiscRot::EvictAudioCacheRange(1000,2124,{{0,2124}},4096,
        [&](DWORD,DWORD,BYTE*){++reads;return true;},[]{return false;}) && reads==0,
        "Insufficient audio outside the entire sampled window leaves cache eviction unverified");
    std::vector<Diagnostics::BalanceSpeedSample> rows;
    for(int speed : {8,16}) {
        Diagnostics::BalanceSpeedSample r;
        r.requestedSpeed=r.actualSpeed=speed;r.validReads=50;r.readTimeMs=160.0/speed;
        r.stabilityRatio=1;r.stabilityMeasured=true;
        r.c1Rate=speed;r.hardwareSamples=15;r.hardwareActualSpeed=speed;r.hardwareVerified=true;
        rows.push_back(r);
    }
    auto matching=Diagnostics::AssessBalance(rows,50,25,true,false);
    check(matching.usingHwEcc, "Qualified hardware counts can contribute when timing and hardware speeds match");
    rows[1].hardwareActualSpeed=8;
    auto mismatch=Diagnostics::AssessBalance(rows,50,25,true,false);
    check(!mismatch.usingHwEcc,
        "Hardware data at a different actual speed cannot be attached to the timing sweep's speed row");
    rows[1].hardwareActualSpeed=16;rows[1].hardwareVerified=false;
    check(!Diagnostics::AssessBalance(rows,50,25,true,false).usingHwEcc,
        "Missing freshness/speed evidence cannot influence hardware-based scoring or recommendations");
    return failures;
}
