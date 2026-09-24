#define NOMINMAX
#include "../OpticalDrive.h"
#include "../PioneerVendor.h"
#include "../InterruptHandler.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <fstream>

namespace {
struct DriveState {
    int pass=0, polls=0, readbacks=0, stops=0, closes=0, vendorCommandsAfterClose=0;
    int cancelPass=0, cancelReadback=0, failStopPass=0;
    DWORD sectors=150;
    bool active=false, invalidSample=false, failStop=false, cancelOnStop=false, primaryC2=false;
    bool failEviction=false, recheckC2=false, recheckCU=false;
} state;
[[noreturn]] void Unexpected() {throw std::runtime_error("Unexpected hardware command in Q-Check test");}
}
// Transport doubles never open a physical device. The actual Q-Check workflow
// and reporting code run against a deterministic audio disc.
bool ScsiDrive::Open(wchar_t) {m_handle=reinterpret_cast<HANDLE>(1);return true;}
void ScsiDrive::Close() {if(IsOpen())++state.closes;m_handle=INVALID_HANDLE_VALUE;state.active=false;}
bool ScsiDrive::PreventMediumRemoval(bool) {return true;}
bool ScsiDrive::GetMediaStatus(DriveHealthCheck& info) {info.mediaPresent=info.mediaReady=true;return true;}
bool ScsiDrive::WaitForDriveReady(int) {return true;}
bool ScsiDrive::GetModePage2A(std::vector<BYTE>& page) {
    if(state.active)Unexpected();page.assign(16,0);page[0]=0x2A;page[1]=14;page[12]=2;return true;
}
bool ScsiDrive::ReadSectorsAudioOnly(DWORD,DWORD,BYTE*) {
    if(state.active)Unexpected();return !state.failEviction;
}
bool ScsiDrive::SupportsQCheck() {return false;}
bool ScsiDrive::SupportsPioneerScan() {return false;}
bool ScsiDrive::SupportsLiteOnScan() {m_liteonScanMethod=LiteOnScanMethod::MeasuredIntervals;return true;}
void ScsiDrive::SetSpeed(int multiplier,int) {
    if (!IsOpen()) return;
    if (state.active) Unexpected();
    m_currentSpeed=multiplier<=0 ? CD_SPEED_MAX : static_cast<WORD>(multiplier*CD_SPEED_1X);
}
bool ScsiDrive::GetActualSpeed(WORD& read,WORD& write) {
    if (!IsOpen() || !state.active) Unexpected();
    ++state.readbacks;read=10*CD_SPEED_1X;write=0;
    if (state.pass==state.cancelPass && state.readbacks==state.cancelReadback) g_interrupt.SetInterrupted(true);
    return true;
}
bool ScsiDrive::LiteOnScanStart(DWORD first,DWORD last) {
    if (!IsOpen() || state.active || first!=0 || last!=state.sectors-1) Unexpected();
    ++state.pass;state.polls=state.readbacks=0;state.active=true;return true;
}
bool ScsiDrive::LiteOnScanPoll(int& c1,int& c2,int& cu,DWORD& lba,bool& done,DWORD* covered,bool* valid) {
    if (!IsOpen() || !state.active) Unexpected();
    lba=state.invalidSample && state.polls>0 ? 50 : state.polls*75;
    ++state.polls;
    c1=4;c2=(state.primaryC2 && state.pass==1) || (state.recheckC2 && state.pass==2) ? 1 : 0;
    cu=state.recheckCU && state.pass==2 ? 1 : 0;
    done=state.polls==static_cast<int>(state.sectors/75);*covered=75;*valid=true;return true;
}
bool ScsiDrive::LiteOnScanStop() {
    if (!IsOpen()) {++state.vendorCommandsAfterClose;return false;}
    ++state.stops;
    if(state.cancelOnStop)g_interrupt.SetInterrupted(true);
    if(state.failStop || state.failStopPass==state.pass)return false;
    state.active=false;return true;
}
bool ScsiDrive::PlextorQCheckStart(DWORD,DWORD) {Unexpected();}
bool ScsiDrive::PlextorQCheckPoll(int&,int&,int&,DWORD&,bool&) {Unexpected();}
bool ScsiDrive::PlextorQCheckStop() {Unexpected();}
bool ScsiDrive::PioneerScanStart(DWORD,DWORD) {Unexpected();}
bool ScsiDrive::PioneerScanPoll(int&,int&,int&,DWORD&,bool&,bool*,DWORD*) {Unexpected();}
bool ScsiDrive::PioneerScanStop() {Unexpected();}

bool PioneerVendor::IsPioneerDrive() {return false;}
PioneerPureReadOffGuard::PioneerPureReadOffGuard(ScsiDrive& drive,bool active):m_pioneer(drive) {if(active)Unexpected();}
PioneerPureReadOffGuard::~PioneerPureReadOffGuard()=default;
PioneerPerformanceModeGuard::PioneerPerformanceModeGuard(ScsiDrive& drive,bool active):m_pioneer(drive) {if(active)Unexpected();}
PioneerPerformanceModeGuard::~PioneerPerformanceModeGuard()=default;
PioneerCdInspectionGuard::PioneerCdInspectionGuard(ScsiDrive& drive,bool active):m_pioneer(drive) {if(active)Unexpected();}
PioneerCdInspectionGuard::~PioneerCdInspectionGuard()=default;
bool PioneerVendor::CdCheckStartWithSense(uint32_t,uint32_t,BYTE&,BYTE&,BYTE&) {Unexpected();}
bool PioneerVendor::CdCheckRead(PioneerCdCheckResult&) {Unexpected();}
bool PioneerVendor::CdCheckStop() {Unexpected();}
bool PioneerVendor::GetCdPhysicalTrackParameters(uint32_t,double&,double&) {Unexpected();}
PioneerCdCheckGrade GradePioneerCdCheckSample(const PioneerCdCheckResult&) {Unexpected();}
namespace Accessibility { bool IsEnabled() {return true;} }

int main() {
    int failures=0;
    auto check=[&](bool ok,const char* text) {std::cout<<(ok?"[PASS] ":"[FAIL] ")<<text<<'\n';if(!ok)++failures;};
    DiscInfo disc;disc.leadOutLBA=150;TrackInfo track;track.trackNumber=1;track.endLBA=149;disc.tracks.push_back(track);
    auto run=[&](QCheckResult& result,std::string& output) {
        disc.leadOutLBA=state.sectors;disc.tracks.front().endLBA=state.sectors-1;
        OpticalDrive drive;drive.Open(L'D');
        std::ostringstream log;auto* original=std::cout.rdbuf(log.rdbuf());
        bool success=false;
        try {success=drive.RunQCheckScan(disc,result,10);}
        catch (...) {std::cout.rdbuf(original);throw;}
        std::cout.rdbuf(original);output=log.str();return success;
    };
    auto savedLog=[&](const QCheckResult& result) {
        wchar_t folder[MAX_PATH],filename[MAX_PATH];
        if(!GetTempPathW(MAX_PATH,folder) || !GetTempFileNameW(folder,L"oqc",0,filename))
            throw std::runtime_error("Could not create Q-Check test report");
        const std::filesystem::path path(filename);
        OpticalDrive drive;
        const bool saved=drive.SaveQCheckLog(result,path.wstring());
        std::ifstream file(path);
        std::ostringstream contents;contents<<file.rdbuf();file.close();
        std::filesystem::remove(path);
        if(!saved)throw std::runtime_error("Could not save Q-Check test report");
        return contents.str();
    };
    QCheckResult result;std::string output;
    state={};g_interrupt.SetInterrupted(false);
    check(run(result,output) && result.totalC1==8 && output.find("CD QUALITY SCAN REPORT")!=std::string::npos,
        "The production Q-Check workflow still completes and reports a normal pass");
    check(result.startup.Complete() && result.startup.c1==8 && result.totalC1==8 &&
        output.find("included in the full-pass totals; none are subtracted")!=std::string::npos,
        "Q-Check keeps the complete first two seconds in both startup detail and full-disc totals");
    state={};state.invalidSample=true;state.failStop=true;g_interrupt.SetInterrupted(false);
    check(!run(result,output) && state.stops==2 && state.closes==1 && state.vendorCommandsAfterClose==0,
        "An invalid sample and failed cleanup close the drive once with no later vendor commands");
    check(output.find("CD QUALITY SCAN REPORT")==std::string::npos && result.samples.size()==1,
        "Invalid sample failure retains earlier observations without publishing a completed quality report");
    state={};state.cancelPass=1;state.cancelReadback=3;g_interrupt.SetInterrupted(false);
    check(!run(result,output) && state.stops==1 && !state.active && result.samples.size()==1 &&
        output.find("CD QUALITY SCAN REPORT")==std::string::npos,
        "Cancellation inside the final primary speed readback cannot publish success");
    state={};state.cancelOnStop=true;g_interrupt.SetInterrupted(false);
    check(!run(result,output) && output.find("CD QUALITY SCAN REPORT")==std::string::npos,
        "Cancellation during final cleanup is checked before publishing the result");
    state={};state.primaryC2=true;state.cancelPass=2;state.cancelReadback=3;g_interrupt.SetInterrupted(false);
    check(!run(result,output) && result.totalC2==2 && !result.c2RecheckCompleted && state.stops==2 &&
        output.find("CD QUALITY SCAN REPORT")==std::string::npos,
        "Cancellation inside the final verification readback retains primary C2 and withholds completion");
    state={};state.primaryC2=true;state.failStopPass=2;g_interrupt.SetInterrupted(false);
    check(!run(result,output) && state.closes==1 && state.stops==3 &&
        state.vendorCommandsAfterClose==0 && !result.c2RecheckCompleted && result.totalC2==2,
        "A failed verification stop closes the drive without publishing a clean recheck");
    state={};state.cancelPass=1;state.cancelReadback=3;state.failStop=true;g_interrupt.SetInterrupted(false);
    check(!run(result,output) && state.closes==1 && state.stops==2 && state.vendorCommandsAfterClose==0,
        "Final-readback cancellation combined with cleanup failure still closes safely");
    state={};g_interrupt.SetInterrupted(false);
    check(run(result,output) && result.totalC2==0 && result.totalC1==8 && !result.c2RecheckAttempted,
        "A fresh run clears cancellation, partial observations and verification state");

    state={};state.primaryC2=true;g_interrupt.SetInterrupted(false);
    check(run(result,output) && result.c2RecheckCompleted && !result.recheckStartupCacheCleared &&
        result.totalC2==2 && result.c2RecheckTotal==0 &&
        ClassifyQCheckC2Stability(result)==QCheckC2Stability::RecheckUnverified,
        "A completed zero recheck on a disc too short for cache eviction retains unverified freshness");
    check(output.find("RECHECK FRESHNESS UNVERIFIED")!=std::string::npos &&
        output.find("verification pass was clean")==std::string::npos &&
        output.find("flagged as intermittent")==std::string::npos,
        "Live Q-Check reporting cannot describe an unverified zero recheck as clean or intermittent");
    auto exported=savedLog(result);
    check(exported.find("Verification Pass:     COMPLETE")!=std::string::npos &&
        exported.find("RECHECK FRESHNESS UNVERIFIED")!=std::string::npos &&
        exported.find("zero counters cannot establish a clean recheck")!=std::string::npos &&
        exported.find("INTERMITTENT READ INSTABILITY")==std::string::npos,
        "The saved report separates completed acquisition from unverified recheck freshness");

    state={};state.primaryC2=true;state.sectors=1500;state.failEviction=true;
    check(run(result,output) && result.c2RecheckCompleted && !result.recheckStartupCacheCleared &&
        ClassifyQCheckC2Stability(result)==QCheckC2Stability::RecheckUnverified,
        "Failed cache-eviction reads cannot establish a clean recheck even with sufficient audio");
    state={};state.primaryC2=true;state.sectors=1500;
    check(run(result,output) && result.c2RecheckCompleted && result.recheckStartupCacheCleared &&
        ClassifyQCheckC2Stability(result)==QCheckC2Stability::Intermittent &&
        output.find("flagged as intermittent")!=std::string::npos &&
        savedLog(result).find("INTERMITTENT READ INSTABILITY")!=std::string::npos,
        "Full coverage with successful cache eviction still classifies a zero recheck as intermittent");

    state={};state.primaryC2=true;state.recheckC2=true;
    check(run(result,output) && !result.recheckStartupCacheCleared && result.c2RecheckTotal==2 &&
        ClassifyQCheckC2Stability(result)==QCheckC2Stability::Reproducible &&
        savedLog(result).find("REPRODUCIBLE C2 ACTIVITY")!=std::string::npos,
        "Positive C2 on an unverified-cache recheck remains observed evidence in the saved report");
    state={};state.primaryC2=true;state.recheckC2=true;state.recheckCU=true;
    check(run(result,output) && !result.recheckStartupCacheCleared && result.c2RecheckTotalCU==2 &&
        ClassifyQCheckC2Stability(result)==QCheckC2Stability::Unrecoverable &&
        output.find("Verification pass ESCALATED")!=std::string::npos &&
        savedLog(result).find("UNRECOVERABLE ACTIVITY ON VERIFICATION PASS")!=std::string::npos,
        "Positive CU keeps highest severity regardless of cache confidence");
    return failures==0?0:1;
}
