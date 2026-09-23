#define NOMINMAX
#include "../OpticalDrive.h"
#include "../DiagnosticAssessment.h"
#include "../ProtectionCheck.h"
#include "../InterruptHandler.h"
#include <sstream>
#include <functional>
#include <cstring>
#include <stdexcept>

// Link-time drive double for the actual diagnostic .cpp files. No device is
// opened. Keep transport failures distinct from readable zero-filled data.
namespace {
enum class Mode { Good, NoAudio, HalfAudio, NoRaw, EmptyRaw, HalfRaw, FormattedOnly, RawRW, BadCrc,
    BadQ, FailSeeks, PartialSeeks, PairFail, OriginFail, CancelControl, CancelQProbe };
Mode mode = Mode::Good;
int audioReads = 0, rawReads = 0, seekCalls = 0;
std::vector<DWORD> boundaryReads;
void Reset(Mode next) {
    mode = next; audioReads = rawReads = seekCalls = 0; boundaryReads.clear();
    g_interrupt.SetInterrupted(false);
}
std::string Capture(const std::function<void()>& run) {
    std::ostringstream text;
    auto* previous = std::cout.rdbuf(text.rdbuf());
    try { run(); } catch (...) { std::cout.rdbuf(previous); throw; }
    std::cout.rdbuf(previous);
    return text.str();
}
}

void ScsiDrive::SetSpeed(int speed, int) { m_currentSpeed = speed == 0 ? CD_SPEED_MAX : static_cast<WORD>(speed * CD_SPEED_1X); }
bool ScsiDrive::GetActualSpeed(WORD& read, WORD& write) { read = m_currentSpeed; write = 0; return true; }
int ScsiDrive::GetLowestHonoredSpeed(bool apply) { if (apply) SetSpeed(1); return 1; }
bool ScsiDrive::SpinDown() { return true; }
bool ScsiDrive::CheckC2Support() { return true; }
bool ScsiDrive::TestOverread(bool) { return false; }
bool ScsiDrive::GetMediaProfile(WORD& profile, std::string& name) { profile = 0x0009; name = "CD-R"; return true; }
bool ScsiDrive::ReadSectorAudioOnly(DWORD, BYTE* audio) {
    ++audioReads;
    if (mode == Mode::NoAudio || mode == Mode::OriginFail || (mode == Mode::HalfAudio && audioReads % 2 == 0)) return false;
    std::memset(audio, 0, AUDIO_SECTOR_SIZE);
    return true;
}
bool ScsiDrive::ReadSector(DWORD lba, BYTE* audio, BYTE* sub) {
    ++rawReads;
    if (mode == Mode::NoRaw || mode == Mode::FormattedOnly || mode == Mode::CancelQProbe ||
        (mode == Mode::HalfRaw && rawReads % 2 == 0)) return false;
    std::memset(audio, 0, AUDIO_SECTOR_SIZE);
    std::memset(sub, 0, SUBCHANNEL_SIZE);
    if (mode == Mode::EmptyRaw) return true;
    BYTE q[12] = { 0x01, 0x01, 0x01 };
    DWORD frame = lba + 150;
    q[7] = BinToBcd(static_cast<BYTE>(frame / 4500));
    q[8] = BinToBcd(static_cast<BYTE>((frame / 75) % 60));
    q[9] = BinToBcd(static_cast<BYTE>(frame % 75));
    const auto crc = static_cast<uint16_t>(~SubchannelCRC16(q, 10));
    q[10] = static_cast<BYTE>(crc >> 8); q[11] = static_cast<BYTE>(crc);
    if (mode == Mode::BadCrc) q[11] ^= 0x01;
    for (int i=0; i<96; ++i) if (q[i/8] & (1 << (7-i%8))) sub[i] |= 0x40;
    if (mode == Mode::RawRW) for (int i=0; i<96; ++i) sub[i] |= 0x09;
    return true;
}
bool ScsiDrive::ReadSectorWithC2(DWORD lba, BYTE*, BYTE*, int& c2) {
    boundaryReads.push_back(lba); c2 = 0; return mode != Mode::NoAudio;
}
bool ScsiDrive::ReadSectorQSingle(DWORD, int& track, int& index) {
    if (mode == Mode::CancelQProbe) { g_interrupt.SetInterrupted(true); return false; }
    track=1; index=1;
    return mode == Mode::FormattedOnly || mode == Mode::Good;
}
bool ScsiDrive::ReadSectorQAdaptive(DWORD, int& track, int& index, DWORD, DWORD) {
    track=1; index=1; return mode != Mode::BadQ;
}
bool ScsiDrive::ReadSectorQControl(DWORD, int& control) {
    control=0;
    if (mode == Mode::CancelControl) g_interrupt.SetInterrupted(true);
    return true;
}
bool ScsiDrive::SeekToLBA(DWORD) {
    ++seekCalls;
    if (mode == Mode::PairFail && seekCalls <= 10) return seekCalls % 2 == 1;
    if (mode == Mode::FailSeeks) return seekCalls % 2 == 1;
    if (mode == Mode::PartialSeeks) return seekCalls != 2;
    return true;
}
bool OpticalDrive::DefeatDriveCache(DWORD, DWORD) { return true; }
void OpticalDrive::EnsureCapabilitiesDetected() { throw std::runtime_error("Unexpected multi-pass workflow in menu diagnostic tests"); }
bool ScsiDrive::SendSCSI(void*, BYTE, void*, DWORD, bool, DWORD) {
    throw std::runtime_error("Unexpected raw SCSI command in menu diagnostic tests");
}
DWORD OpticalDrive::CalculateTotalAudioSectors(const DiscInfo& disc) const {
    DWORD count=0;
    for (const auto& t : disc.tracks) if (t.isAudio) count += t.endLBA - ((t.trackNumber==1) ? 0 : t.pregapLBA) + 1;
    return count;
}

int RunMenuDiagnosticTests() {
    int failures=0;
    auto check=[&](bool ok,const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; if(!ok) ++failures; };
    OpticalDrive drive;
    DiscInfo disc;
    TrackInfo track; track.trackNumber=1; track.startLBA=0; track.pregapLBA=0; track.endLBA=999; track.isAudio=true;
    disc.tracks.push_back(track); disc.leadOutLBA=1000;
    bool ok=false; std::string text;
    AudioAnalysisResult audio;
    Reset(Mode::NoAudio);
    text=Capture([&]{ok=drive.AnalyzeAudioContent(disc,audio,4);});
    check(!ok && !audio.complete && audio.analyzedSectors==0 && audio.readFailures==audio.sampledSectors &&
        text.find("NOT MEASURED")!=std::string::npos && text.find("No digital clipping detected")==std::string::npos,
        "14: all failed reads remain unmeasured instead of clean audio");
    Reset(Mode::HalfAudio);
    text=Capture([&]{ok=drive.AnalyzeAudioContent(disc,audio,4);});
    check(!ok && audio.analyzedSectors==50 && audio.readFailures==50 && audio.silentSectors==50 &&
        text.find("50 (100.0%)")!=std::string::npos,
        "14: audio percentages use 50 readable samples, excluding 50 failed reads");
    Reset(Mode::Good);
    Capture([&]{ok=drive.AnalyzeAudioContent(disc,audio,4);});
    check(ok && audio.complete && audio.analyzedSectors==100 && audio.readFailures==0,
        "14: a reused result becomes complete only after fully readable sampling");
    Reset(Mode::Good);
    text=Capture([&]{ok=drive.CheckLeadAreas(disc,4);});
    check(ok && boundaryReads.size()==300 && *std::max_element(boundaryReads.begin(),boundaryReads.end())<disc.leadOutLBA &&
        text.find("NOT VERIFIED")!=std::string::npos && text.find("Lead areas are intact")==std::string::npos,
        "16: successful boundary proxies leave actual lead areas explicitly unverified");
    Reset(Mode::NoAudio);
    text=Capture([&]{ok=drive.CheckLeadAreas(disc,4);});
    check(!ok && text.find("INCOMPLETE")!=std::string::npos, "16: unreadable boundary proxies fail coverage");
    int errors=0;
    Reset(Mode::BadQ);
    text=Capture([&]{ok=drive.VerifySubchannelIntegrity(disc,errors,4);});
    check(!ok && errors==200 && text.find("INCOMPLETE")!=std::string::npos,
        "17: the 200-error early abort cannot return successful completion");
    Reset(Mode::Good);
    Capture([&]{ok=drive.VerifySubchannelIntegrity(disc,errors,4);});
    check(ok && errors==0, "17: full integrity coverage still completes");
    Reset(Mode::CancelControl);
    Capture([&]{ok=drive.VerifySubchannelIntegrity(disc,errors,4);});
    check(!ok, "17: cancellation during the late Q-control check reaches the caller");
    for (Mode scenario : {Mode::NoRaw,Mode::EmptyRaw,Mode::FormattedOnly,Mode::HalfRaw,Mode::BadCrc}) {
        Reset(scenario); SubchannelBurnResult result;
        text=Capture([&]{ok=drive.VerifySubchannelBurnStatus(disc,result,4);});
        check(!ok && !result.complete && result.verdict.find("EMPTY -")==std::string::npos &&
            text.find("Subchannel extraction can be skipped")==std::string::npos &&
            text.find("Do not skip subchannel extraction")!=std::string::npos,
            "18: failed, zero-filled, formatted-only and partial raw reads cannot prove absent R-W");
        check(result.formattedQVerified==(scenario==Mode::FormattedOnly),
            "18: formatted Q support is retained separately from raw coverage");
    }
    Reset(Mode::Good); SubchannelBurnResult burn;
    text=Capture([&]{ok=drive.VerifySubchannelBurnStatus(disc,burn,4);});
    check(ok && burn.complete && burn.qCrcValidPercent==100 && !burn.subchannelBurned &&
        text.find("sampling result")!=std::string::npos,
        "18: clean raw samples support a qualified basic-subchannel observation");
    Reset(Mode::RawRW);
    text=Capture([&]{ok=drive.VerifySubchannelBurnStatus(disc,burn,4);});
    check(ok && burn.complete && burn.subchannelBurned && burn.rwDataPresent>0 &&
        text.find("R-W content was observed; preserve raw subchannels")!=std::string::npos,
        "18: a complete positive R-W observation still recommends preservation");
    Reset(Mode::CancelQProbe);
    Capture([&]{ok=drive.VerifySubchannelBurnStatus(disc,burn,4);});
    check(!ok && !burn.complete, "18: cancelling the formatted fallback never publishes a completed result");
    for (Mode scenario : {Mode::FailSeeks,Mode::PartialSeeks,Mode::OriginFail,Mode::Good}) {
        Reset(scenario); std::vector<SeekTimeResult> seeks;
        text=Capture([&]{ok=drive.RunSeekTimeAnalysis(disc,seeks);});
        if (scenario==Mode::FailSeeks || scenario==Mode::OriginFail)
            check(!ok && seeks.size()==110 && std::none_of(seeks.begin(),seeks.end(),[](const SeekTimeResult& r){ return r.timingAvailable; }) && text.find("INCOMPLETE")!=std::string::npos && text.find("(normal)")==std::string::npos,
                "24: failed origins or destinations cannot supply seek timing statistics");
        else if (scenario==Mode::PartialSeeks)
            check(!ok && seeks.size()==110 && seeks.front().successfulAttempts==4 && seeks.front().failedAttempts==1 &&
                text.find("(normal)")==std::string::npos,
                "24: mixed attempts retain failures and calculate medians from successful seeks only");
        else
            check(ok && seeks.size()==110 && seeks.front().successfulAttempts==5 && seeks.front().timingAvailable,
                "24: fully successful seeks still complete with measured medians");
    }
    Reset(Mode::PairFail); std::vector<SeekTimeResult> pairFailure;
    text=Capture([&]{ok=drive.RunSeekTimeAnalysis(disc,pairFailure);});
    check(!ok && pairFailure.size()==110 && !pairFailure.front().timingAvailable &&
        pairFailure.front().failedAttempts==5 && pairFailure.front().abnormal &&
        std::count_if(pairFailure.begin(),pairFailure.end(),[](const SeekTimeResult& r){ return r.timingAvailable; })==109,
        "24: an entirely failed pair remains in structured results alongside 109 measured pairs");
    check(text.find("None - consistent mechanical performance")==std::string::npos &&
        text.find("109 / 110")!=std::string::npos && text.find("Unmeasured pairs: 1")!=std::string::npos,
        "24: failed pairs remain visible in the final mechanical summary and coverage");
    Reset(Mode::Good);
    Capture([&]{ok=drive.RunSeekTimeAnalysis(disc,pairFailure);});
    check(ok && pairFailure.size()==110 && std::all_of(pairFailure.begin(),pairFailure.end(),
        [](const SeekTimeResult& r){ return r.timingAvailable && r.failedAttempts==0; }),
        "24: a reused result clears failed-pair metadata after a new fully measured scan");
    ProtectionCheckResult protection;
    protection.indicators={{"Intentional Errors","",true,2},{"Data Track Present","",true,0},{"Pre-Emphasis Anomaly","",true,0}};
    FinalizeProtectionAssessment(protection);
    check(!protection.protectionLikely && protection.detectedCount==1,
        "19: one strong and two informational observations are not corroborated protection");
    protection.indicators[1].severity=1;
    FinalizeProtectionAssessment(protection);
    check(!protection.protectionLikely, "19: one strong, one weak and one informational remains inconclusive");
    protection.indicators[2].severity=1;
    FinalizeProtectionAssessment(protection);
    check(protection.protectionLikely, "19: one strong and two actual warnings meet the intended threshold");
    protection.indicators={{"a","",true,2},{"b","",true,2}};
    FinalizeProtectionAssessment(protection);
    check(protection.protectionLikely, "19: two strong indicators still corroborate protection");
    protection.indicators={{"a","",true,0},{"b","",true,0}};
    FinalizeProtectionAssessment(protection);
    check(!protection.protectionLikely && protection.detectedCount==0,
        "19: informational-only observations clear a previous positive verdict");
    check(!Diagnostics::HasDistinctBalanceSpeeds({16,16,16,16,16,16},{25,25,25,25,25,25},25),
        "26: six requests at one actual speed cannot form a measured speed sweep");
    check(!Diagnostics::HasDistinctBalanceSpeeds({0,0,0,0,0,0},{50,50,50,50,50,50},25),
        "26: unknown actual speeds do not become independent measurements");
    check(!Diagnostics::HasDistinctBalanceSpeeds({4,8},{50,24},25) &&
        Diagnostics::HasDistinctBalanceSpeeds({4,8},{50,25},25),
        "26: distinct speed measurements must meet sample coverage");
    check(Diagnostics::BalanceCoverageCap({25,25,25,25,25,25},50,5,5)==0 &&
        Diagnostics::BalanceCoverageCap({25,25,25,25,25,25},50,0,5)==0,
        "26: 50 percent failed coverage cannot return full marks even at a collapsed comparison");
    check(Diagnostics::BalanceCoverageCap({50,45},50,0,1)==60 &&
        Diagnostics::BalanceCoverageCap({50,50},50,0,1)==100,
        "26: coverage limits retain severity while preserving fully measured clean coverage");
    check(Diagnostics::BalanceCoverageCap({0,50,50},50,0,2)==0,
        "26: unreadable low-speed rows cannot disappear when choosing a later baseline");
    for (int operation=0; operation<5; ++operation) {
        Reset(Mode::Good); drive.GetDriveRef().SetSpeed(8);
        Capture([&] {
            if (operation==0) drive.AnalyzeAudioContent(disc,audio,4);
            else if (operation==1) drive.CheckLeadAreas(disc,4);
            else if (operation==2) drive.VerifySubchannelIntegrity(disc,errors,4);
            else if (operation==3) drive.VerifySubchannelBurnStatus(disc,burn,4);
            else { std::vector<SeekTimeResult> seeks; drive.RunSeekTimeAnalysis(disc,seeks); }
        });
        check(drive.GetDriveRef().GetCurrentSpeed()==8*CD_SPEED_1X,
            "Diagnostic workflow restores the previous drive speed");
    }
    Reset(Mode::Good);
    return failures;
}
