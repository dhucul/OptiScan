#define NOMINMAX
#include "../ScsiDrive.h"
#include "../ScanQualityRating.h"
#include "../ScanResults.h"
#include "../QualityScanSession.h"
#include <deque>
#include <iostream>
#include <map>
#include <utility>

// These transport doubles never open a drive or issue DeviceIoControl.
struct FakeDrive {
    bool oldSupported=true, failRead=false, failInit=false, failPoll=false, failStop=false;
    bool blankResponse=false, zeroCounters=false, stuckPosition=false;
    bool distinctCounterBytes=false;
    unsigned position=0, oldStops=0, newCommands=0;
    std::deque<unsigned> positions;
    std::vector<std::pair<unsigned,unsigned>> reads;
};
static std::map<const ScsiDrive*,FakeDrive> fake;
void ScsiDrive::Close() {
    LiteOnScanStop();
    m_liteonScanActive=false;
    m_liteonScanMethod=LiteOnScanMethod::Unknown;
    m_liteonScanProbed=-1;
}
bool ScsiDrive::SeekToLBA(DWORD lba) { fake[this].position=lba; return true; }
bool ScsiDrive::ReadCdAudio(DWORD lba,DWORD count,BYTE,BYTE*,DWORD) {
    fake[this].reads.push_back({lba,count});
    return !fake[this].failRead;
}
bool ScsiDrive::SendSCSIWithSense(void* raw,BYTE,void* data,DWORD size,
    BYTE* sk,BYTE* asc,BYTE* ascq,bool,DWORD) {
    if(sk)*sk=0; if(asc)*asc=0; if(ascq)*ascq=0;
    auto& state=fake[this];
    const auto* cdb=static_cast<BYTE*>(raw);
    auto* out=static_cast<BYTE*>(data);
    if(out) std::fill_n(out,size,BYTE(0));
    if(cdb[0]==0xDF) {
        if(cdb[1]==0xA3 && cdb[2]==1) {
            ++state.oldStops;
            return !state.failStop;
        }
        if(!state.oldSupported || (state.failInit && cdb[1]==0xA0)) {
            if(sk)*sk=5;
            return false;
        }
        if(cdb[1]==0x82 && cdb[2]==5 && !state.zeroCounters) { out[1]=2; out[3]=1; out[4]=3; }
        if(cdb[1]==0x82 && cdb[2]==5 && state.distinctCounterBytes) {
            out[0]=0x01;out[1]=0x23;out[2]=0x04;out[3]=0x56;out[4]=0x78;
        }
        return true;
    }
    if(cdb[0]==0xF3) {
        ++state.newCommands;
        if(state.failPoll) return false;
        if(state.blankResponse) return true;
        const unsigned pos=state.positions.empty()?state.position:state.positions.front();
        if(!state.positions.empty()) state.positions.pop_front();
        if(pos==UINT32_MAX) return true; // all-zero terminal response
        const unsigned msf=pos+150;
        out[1]=static_cast<BYTE>(msf/4500);
        out[2]=static_cast<BYTE>((msf/75)%60);
        out[3]=static_cast<BYTE>(msf%75);
        out[5]=state.zeroCounters?0:4; out[7]=state.zeroCounters?0:2;
        if(!state.stuckPosition) state.position=pos+75;
        return true;
    }
    throw std::runtime_error("Unexpected command in LiteOn transport test");
}
int main() {
    int failed=0;
    auto check=[&](bool ok,const char* label) {
        std::cout<<(ok?"[PASS] ":"[FAIL] ")<<label<<"\n"; if(!ok) ++failed;
    };
    ScsiDrive measured;
    check(measured.SupportsLiteOnScan() && measured.LiteOnScanMeasuresCu() &&
        fake[&measured].newCommands==0,
        "Actual probe chooses measured intervals before the alternate protocol");
    fake[&measured].reads.clear();
    check(measured.LiteOnScanStart(0,104),"Measured scan starts with exact requested range");
    int c1=0,c2=0,cu=0; DWORD lba=0,sectors=0; bool done=false,valid=false;
    std::vector<ScanQuality::C1Interval> samples;
    check(measured.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) &&
        valid && !done && lba==0 && sectors==75 && cu==3,
        "First host-driven sample carries measured coverage and actual CU");
    samples.push_back({lba,sectors,c1});
    check(measured.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) &&
        valid && done && lba==75 && sectors==30,
        "Final partial interval ends exactly at the requested last sector");
    samples.push_back({lba,sectors,c1});
    check(ScanQuality::SummarizeC1(samples).RateAvailable() &&
        ScanQuality::SummarizeC1(samples).measuredSectors==105,
        "Actual driver samples support measured C1 rates and correct coverage");
    bool bounded=true;
    for(auto [start,count]:fake[&measured].reads) bounded &= count<=16 && start+count<=105;
    check(bounded,"No host read extends beyond the requested range");
    const auto reads=fake[&measured].reads.size();
    check(measured.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) && done && !valid &&
        sectors==0 && fake[&measured].reads.size()==reads,
        "Polling after completion cannot duplicate the final sample");
    measured.LiteOnScanStop();
    check(!measured.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) && !valid,
        "Stopped sessions cannot return stale observations");
    measured.LiteOnScanStart(0,74);
    fake[&measured].distinctCounterBytes=true;
    check(measured.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) && valid &&
        c1==0x0123 && c2==0x0456 && cu==0x78,
        "DF 82 05 preserves both bytes of C2 separately from C1 and CU (QPxTool field map)");
    fake[&measured].distinctCounterBytes=false;
    measured.LiteOnScanStop();

    ScsiDrive counter; fake[&counter].oldSupported=false;
    check(counter.SupportsLiteOnScan() && !counter.LiteOnScanMeasuresCu(),
        "Alternate protocol is available only after measured protocol fails and has no CU");
    check(measured.LiteOnScanMeasuresCu() && measured.SupportsLiteOnScan(),
        "Probing another drive cannot change a cached drive's protocol");
    check(counter.LiteOnScanStart(0,200),"Counter-only fallback starts");
    fake[&counter].positions={0,75,75,150,225};
    bool all=true; unsigned accepted=0;
    for(int i=0;i<5;++i) {
        all &= counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid);
        if(valid) { ++accepted; all &= lba<=200 && sectors==0 && cu==0; }
    }
    check(all && accepted==3 && done && !valid,
        "Duplicate polls and out-of-range end markers are not recorded as samples");
    counter.LiteOnScanStop();

    counter.LiteOnScanStart(0,244854);
    fake[&counter].positions={170625,170700,170626};
    counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid);
    counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid);
    check(!counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) && !done && !valid,
        "A backward position invalidates the pass instead of inflating totals or claiming completion");
    counter.LiteOnScanStop();

    counter.LiteOnScanStart(0,244854);
    fake[&counter].positions={244800,244875};
    counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid);
    check(counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) &&
        done && !valid && lba==244854,
        "The user's 244875 marker finishes without exporting an out-of-range observation");
    counter.LiteOnScanStop();

    counter.LiteOnScanStart(0,244854);
    fake[&counter].positions={300,UINT32_MAX};
    counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid);
    check(!counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) && !done,
        "An early terminal response cannot produce a clean completed recheck");
    counter.LiteOnScanStop();
    counter.LiteOnScanStart(0,1000);
    fake[&counter].failPoll=true;
    check(!counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) && !done && !valid,
        "Transport failure is distinct from successful completion");
    fake[&counter].failPoll=false;
    counter.LiteOnScanStop();

    measured.LiteOnScanStart(0,74);
    fake[&measured].failRead=true;
    check(measured.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) &&
        valid && sectors==0 && c1==2 && cu==3,
        "Failed head reads retain observed counts without fabricating measured duration");
    fake[&measured].failRead=false;
    measured.LiteOnScanStop();

    ScsiDrive initFailure; fake[&initFailure].failInit=true;
    check(initFailure.SupportsLiteOnScan() && !initFailure.LiteOnScanMeasuresCu() &&
        fake[&initFailure].oldStops>0,
        "Partially initialized measured protocol is stopped before fallback");
    measured.LiteOnScanStart(0,74);
    fake[&measured].failStop=true;
    check(!measured.LiteOnScanStop() && !measured.LiteOnScanStart(150,224),
        "A failed stop cannot silently start a new overlapping scan");
    fake[&measured].failStop=false;
    check(measured.LiteOnScanStop() && measured.LiteOnScanStart(150,224),
        "A successful cleanup allows the next scan to start with fresh state");
    measured.LiteOnScanStop();
    ScsiDrive blank;
    fake[&blank].oldSupported=false; fake[&blank].blankResponse=true;
    check(!blank.SupportsLiteOnScan() && !blank.LiteOnScanStart(0,1000),
        "All-zero placeholder responses cannot establish scan support or start a scan");
    fake[&blank].blankResponse=false; fake[&blank].stuckPosition=true;
    check(!blank.SupportsLiteOnScan(),
        "Repeated nonzero position packets cannot establish a functioning scanner");
    fake[&blank].stuckPosition=false; fake[&blank].zeroCounters=true;
    check(blank.SupportsLiteOnScan() && !blank.LiteOnScanMeasuresCu(),
        "A failed probe remains retryable and advancing zero-error responses establish support");

    counter.LiteOnScanStart(0,244854);
    fake[&counter].positions={300,375,450,525,300000};
    for(int i=0;i<4;++i) counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid);
    check(!counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) && !done && !valid,
        "A large jump beyond the requested end cannot complete a partial pass");
    counter.LiteOnScanStop();
    counter.LiteOnScanStart(0,244854);
    fake[&counter].positions={300,244854};
    counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid);
    check(!counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) && !done,
        "A large jump exactly onto the endpoint is also rejected");
    counter.LiteOnScanStop();
    counter.LiteOnScanStart(0,244854);
    fake[&counter].positions={300000};
    check(!counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) && !done,
        "An end marker before any in-range observation cannot complete a scan");
    counter.LiteOnScanStop();
    counter.LiteOnScanStart(0,244854);
    fake[&counter].positions={244800,UINT32_MAX};
    counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid);
    check(counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid) && done && !valid,
        "A terminal marker after reaching the final interval finishes without an extra sample");
    counter.LiteOnScanStop();
    counter.LiteOnScanStart(0,150);
    fake[&counter].positions={0,75,150};
    bool exactEnd=true;
    for(int i=0;i<3;++i) exactEnd &= counter.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid);
    check(exactEnd && done && valid && lba==150,
        "An ordinary 75-sector advance onto the exact endpoint remains valid");
    counter.LiteOnScanStop();

    measured.LiteOnScanStart(0,299);
    fake[&measured].failRead=true; fake[&measured].zeroCounters=true;
    QCheckResult coverageProof; coverageProof.totalC2=9; coverageProof.graphSectors=300;
    for(int i=0;i<4;++i) {
        measured.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid);
        if(valid) { QCheckSample s; s.lba=lba; s.measuredSectors=sectors;
            s.c1=c1; s.c2=c2; s.cu=cu; coverageProof.c2RecheckSamples.push_back(s); }
    }
    coverageProof.c2RecheckCompleted=done;
    check(!HasCompleteQCheckCoverage(coverageProof.c2RecheckSamples,0,300) &&
        ClassifyQCheckC2Stability(coverageProof)==QCheckC2Stability::RecheckIncomplete,
        "A finished loop where every head read failed cannot classify zero counters as a clean recheck");
    measured.LiteOnScanStop();
    fake[&measured].failRead=false;
    measured.LiteOnScanStart(0,299);
    coverageProof.c2RecheckSamples.clear();
    for(int i=0;i<4;++i) {
        measured.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid);
        if(valid) { QCheckSample s; s.lba=lba; s.measuredSectors=sectors;
            coverageProof.c2RecheckSamples.push_back(s); }
    }
    coverageProof.recheckStartupCacheCleared=true;
    check(HasCompleteQCheckCoverage(coverageProof.c2RecheckSamples,0,300) &&
        ClassifyQCheckC2Stability(coverageProof)==QCheckC2Stability::Intermittent,
        "A fully read zero-error range supplies the coverage required for a clean recheck");
    measured.LiteOnScanStop();
    coverageProof.c2RecheckSamples[1].measuredSectors=0;
    check(ClassifyQCheckC2Stability(coverageProof)==QCheckC2Stability::RecheckIncomplete,
        "One unverified interval is enough to withhold a clean recheck");
    coverageProof.c2RecheckTotal=2;
    check(ClassifyQCheckC2Stability(coverageProof)==QCheckC2Stability::Reproducible,
        "Positive C2 evidence survives incomplete coverage");
    coverageProof.c2RecheckTotalCU=1;
    check(ClassifyQCheckC2Stability(coverageProof)==QCheckC2Stability::Unrecoverable,
        "Positive CU evidence survives incomplete coverage");

    measured.LiteOnScanStart(0,74);
    {
        QualityScanSession session([&]() { return measured.LiteOnScanStop(); });
        fake[&measured].failStop=true;
        const auto stopsBefore=fake[&measured].oldStops;
        check(!session.Stop() && fake[&measured].oldStops==stopsBefore+2,
            "Workflow cleanup reports failure after bounded attempts instead of marking itself stopped");
        fake[&measured].failStop=false;
        check(session.Stop() && fake[&measured].oldStops==stopsBefore+3,
            "A failed workflow stop remains retryable when transport recovers");
        check(session.Stop() && fake[&measured].oldStops==stopsBefore+3,
            "Confirmed cleanup is idempotent and sends no extra stop commands");
    }
    measured.LiteOnScanStart(0,74);
    const auto cancelStops=fake[&measured].oldStops;
    { QualityScanSession cancelled([&]() { return measured.LiteOnScanStop(); }); }
    check(fake[&measured].oldStops==cancelStops+1 &&
        !measured.LiteOnScanPoll(c1,c2,cu,lba,done,&sectors,&valid),
        "Leaving a cancelled workflow stops its active session");
    ScsiDrive failedCleanup;
    fake[&failedCleanup].failInit=true; fake[&failedCleanup].failStop=true;
    check(!failedCleanup.SupportsLiteOnScan() && fake[&failedCleanup].newCommands==0 &&
        !failedCleanup.LiteOnScanStart(0,74),
        "Failed probe cleanup closes the session and prevents unsafe protocol fallback");
    std::cout<<(failed?"LiteOn tests failed.\n":"All LiteOn transport tests passed.\n");
    return failed?1:0;
}
