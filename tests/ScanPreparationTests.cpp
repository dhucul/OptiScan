#define NOMINMAX
#include "../HardwareSweep.h"
#include "../DiscBalanceAssessment.h"
#include "../DiscRotQuality.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

int RunScanPreparationTests() {
    int failures=0;
    auto check=[&](bool ok,const char* text){std::cout<<(ok?"[PASS] ":"[FAIL] ")<<text<<'\n';if(!ok)++failures;};
    auto window=Diagnostics::HardwareSweepRange({{0,316266}},237199);
    check(window && window->first==237150 && window->second==238274 && window->first-750==236400,
        "Balance reproduces the tested Q-Check grid with a continuous ten-second startup segment");
    auto offset=Diagnostics::HardwareSweepRange({{100,10000}},2400);
    check(offset && (offset->first-100)%75==0 && offset->first-750>=100,
        "A nonzero first audio LBA anchors the same sample grid for every workflow");
    auto gap=Diagnostics::HardwareSweepRange({{0,999},{2000,4999}},2200);
    check(gap && gap->first==2775 && gap->first-750>=2000 && gap->second<=4999,
        "The aligned lead-in and target cannot cross a mixed-mode data gap");
    check(!Diagnostics::HardwareSweepRange({{0,1873}},0) &&
        Diagnostics::HardwareSweepRange({{0,1874}},0)->first==750,
        "Short audio cannot silently shorten startup or fabricate fifteen target seconds");
    auto upper=Diagnostics::HardwareSweepRange({{MAXDWORD-3000,MAXDWORD}},MAXDWORD-1);
    check(upper && upper->second<=MAXDWORD && upper->first>=MAXDWORD-3000+750,
        "Alignment arithmetic stays bounded near the maximum sector address");

    struct RunState {int starts=0,stops=0,polls=0;bool active=false;std::uint64_t clock=0;};
    auto run=[&](RunState& state,int unknownAt=-1,int failAt=-1,bool zeroTarget=false,bool zeroAll=false) {
        return Diagnostics::MeasureHardwareSweep(1500,2624,
            [&]{if(state.active)throw std::runtime_error("Eviction during scan");return true;},
            [&](DWORD first,DWORD last){++state.starts;state.active=true;return first==750 && last==2624;},
            [&](Diagnostics::HardwareSweepSample& sample) {
                const int i=state.polls++;
                if(i==failAt)return false;
                sample.lba=750+i*75;sample.sectors=i==unknownAt?0:75;sample.done=i==24;
                sample.c1=zeroAll?0:(i<10?(i==1?40:1):(zeroTarget?0:2));
                sample.secondStage=zeroAll?0:(i==1?18:(i==24&&!zeroTarget?1:0));
                sample.cu=zeroAll?0:(i==1?3:0);
                return true;
            },
            [&]{++state.stops;state.active=false;return true;},
            [&](WORD& speed){if(!state.active)throw std::runtime_error("Readback outside scan");speed=16*CD_SPEED_1X;return true;},
            []{return false;},[&]{return state.clock+=100;},{},750);
    };
    bool invalidStarted=false;
    auto invalid=Diagnostics::MeasureHardwareSweep(1500,2699,[]{return true;},
        [&](DWORD,DWORD){invalidStarted=true;return true;},
        [](Diagnostics::HardwareSweepSample&){return false;},[]{return true;},
        [](WORD&){return false;},[]{return false;},[]{return std::uint64_t{0};});
    check(!invalid.attempted && !invalidStarted,
        "A non-fifteen-second target is rejected before entering measurement mode");
    RunState state;
    auto pass=run(state);
    check(pass.Qualified() && state.starts==1 && state.stops==1 && state.polls==25 &&
        pass.observations.size()==25 && pass.startup.samples==10 && pass.intervals.size()==15,
        "One uninterrupted session records all startup and target samples with no boundary restart");
    check(pass.startup.c1==49 && pass.startup.secondStage==18 && pass.startup.cu==3 &&
        pass.c1.total==30 && pass.secondStageTotal==1 && pass.cuTotal==0 &&
        pass.c1.measuredSectors==1125 && pass.throughput.sectors==1875 && pass.targetThroughput.sectors==1125,
        "Startup counters and duration stay separate without losing any C1/C2/CU evidence");
    std::ostringstream output;
    Diagnostics::PrintHardwareSweepEvidence(output,pass,16,16,"C2");
    check(output.str().find("Startup raw counts: C1 49, C2 18, CU 3")!=std::string::npos &&
        output.str().find("Total C1 observed: 30")!=std::string::npos &&
        output.str().find("C2 observed total: 1")!=std::string::npos,
        "The report shows both startup observations and the target's independent totals");
    state={};auto partial=run(state,-1,5);
    check(!partial.complete && !partial.Qualified() && partial.startup.secondStage==18 &&
        partial.startup.cu==3 && partial.intervals.empty() && state.stops==1,
        "Failure during startup retains positive errors and cannot produce a clean target measurement");
    state={};auto unknown=run(state,1);
    check(unknown.complete && !unknown.Qualified() && !unknown.startup.Complete() &&
        unknown.startup.secondStage==18 && unknown.c1.total==30,
        "Unknown startup duration prevents qualification while retaining target and startup counts");
    state={};auto targetZero=run(state,-1,-1,true);
    check(targetZero.Qualified() && targetZero.c1.total==0 && targetZero.startup.HasActivity(),
        "Verified activity earlier in the same fresh pass supports an observed zero target without deleting startup warnings");
    state={};auto allZero=run(state,-1,-1,true,true);
    check(allZero.complete && !allZero.Qualified(),
        "A wholly zero startup and target still leave decoder activity unverified");

    Diagnostics::QualityStartupSummary startup;
    startup.Reset(0,150);startup.Record(0,75,4,1,0);startup.Record(75,75,5,0,0);
    QCheckResult full;full.startup=startup;full.totalC1=9;full.totalC2=1;full.totalSectors=150;
    full.samples={{0,4,1,0,0,75},{75,5,0,0,0,75}};
    ComputeTimedC1(full);
    output.str("");output.clear();Diagnostics::PrintQualityStartup(output,startup,"C2",true,true);
    check(startup.Complete() && full.c1.total==9 && full.totalC2==1 &&
        output.str().find("included in the full-pass totals; none are subtracted")!=std::string::npos,
        "Full-disc startup labeling never subtracts first-sector counts or coverage");
    DiscRotAnalysis rot;DiscRot::RecordQualityEvidence(full,true,rot);
    check(rot.qualityStartup.c1==9 && rot.qualityC2Count==1 && rot.c1.total==9 && rot.c1.RateAvailable(),
        "Disc Rot preserves the same full-pass and startup evidence as Q-Check");
    full.totalSectors=750;
    DiscRot::RecordQualityEvidence(full,false,rot);
    check(rot.c1.total==9 && rot.c1.rawPeakCount==5 && rot.c1.measuredSectors==150 &&
        rot.c1RequestedSectors==750 && rot.qualitySamples.size()==2 && !rot.qualityScanComplete &&
        !rot.c1.RateAvailable() && rot.c1.Rating()==ScanQuality::C1Rating::Unrated,
        "A failed Disc Rot phase retains raw C1, peak and coverage while withholding its rating");
    output.str("");output.clear();
    Diagnostics::PrintQualityStartup(output,rot.qualityStartup,"C2",true,true);
    ScanQuality::PrintC1Summary(output,rot.c1,rot.c1RequestedSectors);
    check(output.str().find("Startup raw counts: C1 9")!=std::string::npos &&
        output.str().find("Total C1 observed: 9 - rating unavailable")!=std::string::npos &&
        output.str().find("20.00% of requested audio")!=std::string::npos &&
        output.str().find("Average C1: NOT RATED")!=std::string::npos,
        "Partial Disc Rot reports include startup C1 in the displayed total without printing a measured rate");
    auto unknownFull=full;unknownFull.samples.back().measuredSectors=0;
    DiscRot::RecordQualityEvidence(unknownFull,false,rot);
    check(rot.c1.total==9 && !rot.c1.timingKnown && !rot.c1.RateAvailable() && rot.qualityC2Count==1,
        "Unknown duration in a partial Disc Rot phase preserves C1 and C2 without inventing coverage");
    auto zeroFull=full;zeroFull.totalC1=zeroFull.totalC2=0;
    zeroFull.samples={{0,0,0,0,0,75},{75,0,0,0,0,75}};
    DiscRot::RecordQualityEvidence(zeroFull,true,rot);
    check(rot.qualityScanComplete && rot.c1.total==0 && rot.c1.samples==2 && !rot.c1.RateAvailable() &&
        rot.qualityC2Count==0,
        "A completed all-zero Disc Rot phase retains observations but cannot gain a C1 rating");
    auto unverifiedFull=full;unverifiedFull.c1Unverified=true;
    DiscRot::RecordQualityEvidence(unverifiedFull,true,rot);
    check(rot.c1.total==9 && !rot.c1.RateAvailable(),
        "Disc Rot cannot promote explicitly unverified C1 observations into a rating");
    auto pioneerFull=full;pioneerFull.scanMethod="Pioneer (0x3B/0x3C)";pioneerFull.cuMeasured=false;
    pioneerFull.samples.front().c2=0;pioneerFull.samples.front().pioneerE22=1;
    DiscRot::RecordQualityEvidence(full,true,rot);
    DiscRot::RecordQualityEvidence(pioneerFull,false,rot);
    check(rot.c1.total==9 && !rot.c1.RateAvailable() && rot.qualitySamples.front().pioneerE22==1 &&
        rot.qualityC2Count==0 && rot.qualityCUCount==0 && !rot.qualityCuMeasured,
        "Partial Pioneer C1 and E22 stay recorded without retaining a previous result's C2 or CU");
    DiscRot::RecordQualityEvidence(QCheckResult{},false,rot);
    check(rot.c1.samples==0 && rot.c1.total==0 && rot.qualitySamples.empty() && !rot.qualityCountersRecorded &&
        !rot.qualityScanComplete && rot.c1RequestedSectors==0 && rot.qualityC2Count==0,
        "An empty quality phase cannot inherit an earlier phase's raw observations");
    output.str("");output.clear();Diagnostics::PrintQualityStartup(output,startup,"E22",false,true);
    check(output.str().find("E22 1, CU NOT MEASURED")!=std::string::npos,
        "Pioneer startup E22 remains diagnostic and cannot become a fabricated CU zero");

    std::vector<Diagnostics::BalanceSpeedSample> rows;
    for(int speed:{8,16}) {
        Diagnostics::BalanceSpeedSample row;row.requestedSpeed=row.actualSpeed=speed;
        row.validReads=50;row.readTimeMs=160.0/speed;row.jitterCV=.1;row.stabilityMeasured=true;row.stabilityRatio=1;
        rows.push_back(row);
    }
    auto flagged=Diagnostics::AssessBalance(rows,50,25,false,false,{0,0,18});
    output.str("");output.clear();Diagnostics::PrintBalanceRipRecommendation(output,flagged);
    check(!flagged.recommendationAvailable && output.str().find("Startup C2 raw count: 18")!=std::string::npos &&
        Diagnostics::BalanceExtractionGuidance(flagged).find("startup C2")!=std::string::npos,
        "Moving a C2 spike into startup cannot silently restore a reassuring extraction recommendation");
    return failures;
}
