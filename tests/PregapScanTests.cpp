#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

// Exercise the complete production ReadTOC/ReadFullTOC with a synthetic TOC
// and Q results. No physical drive is opened and no write command is linked.
namespace {
enum class Fault { None, FalseCoarse, Separated, NoQ, ShortEndMissing,
    ShortBeforeMissing, InteriorMissing, CancelCoarse, CancelFine,
    CancelFirstRefinement, RetrySeparated, LongWindowMissing, FirstQMissing,
    FirstSeparated, FirstFalseCoarse, ShortWrongBefore };
Fault fault;
std::vector<DWORD> starts, gaps;
std::map<DWORD, int> votedAt;
int singles, voted, hiddenCalls, readsAfterCancel, checks, failures, speed;

void Expect(const std::string& name, bool pass) {
    ++checks;
    if (!pass) ++failures;
    std::cerr << (pass ? "[PASS] " : "[FAIL] ") << name << '\n';
}

void Reset(std::vector<DWORD> positions, std::vector<DWORD> pregaps, Fault inject = Fault::None) {
    starts = std::move(positions); gaps = std::move(pregaps); fault = inject;
    singles = voted = hiddenCalls = readsAfterCancel = 0;
    votedAt.clear(); speed = 0;
    g_interrupt.SetInterrupted(false);
}

bool Position(DWORD lba, int& track, int& index, bool fine) {
    if (g_interrupt.IsInterrupted()) ++readsAfterCancel;
    if (fine) { ++voted; ++votedAt[lba]; } else ++singles;
    const DWORD window = starts[1] > 450 ? starts[1] - 450 : 0;
    if (fault == Fault::NoQ) return false;
    if (fault == Fault::FirstQMissing && lba <= 1) return false;
    if (fault == Fault::FirstSeparated && lba < 225) {
        if (lba != 5 && lba != 7 && lba != 9) return false;
        track = 1; index = 1; return true;
    }
    if (fault == Fault::LongWindowMissing && lba == window) return false;
    if (fault == Fault::FirstFalseCoarse && lba < starts[0]) {
        track = 1; index = lba >= starts[0] - 145 ? 0 : 1;
        if (!fine && lba == starts[0] - 435) index = 0;
        return true;
    }
    if (fault == Fault::CancelCoarse && !fine) g_interrupt.SetInterrupted(true);
    if (fault == Fault::CancelFine && fine && lba >= window) g_interrupt.SetInterrupted(true);
    if (fault == Fault::FalseCoarse && !fine && lba == window + 15) {
        track = 2; index = 0; return true;
    }
    if (fault == Fault::RetrySeparated && !fine && lba >= window && speed != 2) return false;
    if (fault == Fault::Separated || (fault == Fault::RetrySeparated && speed == 2)) {
        if (lba == window + 16 || lba == window + 18) return false;
        if (lba == window + 15 || lba == window + 17 || lba == window + 19) {
            track = 2; index = 0; return true;
        }
    }
    if (fault == Fault::ShortEndMissing && lba == starts[1]) return false;
    if (fault == Fault::ShortBeforeMissing && lba == starts[1] - gaps[1] - 1) return false;
    if (fault == Fault::ShortWrongBefore && lba == starts[1] - gaps[1] - 1) {
        track = 3; index = 1; return true;
    }
    if (fault == Fault::InteriorMissing && lba == starts[1] - gaps[1] + 7) return false;
    if (fault == Fault::CancelFirstRefinement && lba < starts[0]) {
        const DWORD boundary = starts[0] - 145;
        track = 1; index = lba >= boundary ? 0 : 1;
        if (fine && lba == boundary - 1 && votedAt[lba] == 2) {
            g_interrupt.SetInterrupted(true); index = 0;
        }
        return true;
    }
    for (size_t i = starts.size(); i-- > 0;) {
        if (lba >= starts[i]) { track = static_cast<int>(i + 1); index = 1; return true; }
        if (lba >= starts[i] - gaps[i]) { track = static_cast<int>(i + 1); index = 0; return true; }
    }
    track = 1; index = 0; return true;
}

void GapCase(DWORD gap, Fault inject = Fault::None, DWORD expected = MAXDWORD) {
    Reset({0, 3000}, {0, gap}, inject);
    DiscInfo disc; OpticalDrive drive;
    const bool ok = drive.ReadTOC(disc);
    const DWORD actual = disc.tracks[1].startLBA - disc.tracks[1].pregapLBA;
    Expect("gap " + std::to_string(gap) + ", fault " + std::to_string(static_cast<int>(inject)),
        ok && actual == (expected == MAXDWORD ? gap : expected));
    Expect("previous track ends before detected gap", disc.tracks[0].endLBA == disc.tracks[1].pregapLBA - 1);
}
}

void ScsiDrive::Close() {}
void ScsiDrive::SetSpeed(int read, int) { speed = read; }
bool ScsiDrive::ReadSectorQSingle(DWORD lba, int& track, int& index) { return Position(lba, track, index, false); }
bool ScsiDrive::ReadSectorQ(DWORD lba, int& track, int& index) { return Position(lba, track, index, true); }
bool ScsiDrive::ReadSectorAudioOnly(DWORD, BYTE* data) {
    std::fill_n(data, AUDIO_SECTOR_SIZE, BYTE{0});
    return fault != Fault::RetrySeparated;
}
bool OpticalDrive::DetectHiddenTrack(DiscInfo&) { ++hiddenCalls; return false; }

bool ScsiDrive::SendSCSI(void* command, BYTE, void* output, DWORD size, bool, DWORD) {
    auto* cdb = static_cast<const BYTE*>(command);
    if (cdb[0] != 0x43) throw std::runtime_error("Unexpected command in scanner test");
    if (cdb[2] != 0) return false; // Only a standard format-0 TOC is needed.
    auto* data = static_cast<BYTE*>(output);
    std::fill_n(data, size, BYTE{0});
    const auto length = 2 + (starts.size() + 1) * 8;
    data[0] = static_cast<BYTE>(length >> 8); data[1] = static_cast<BYTE>(length);
    data[2] = 1; data[3] = static_cast<BYTE>(starts.size());
    for (size_t i = 0; i <= starts.size(); ++i) {
        auto* entry = data + 4 + i * 8;
        entry[1] = 0x10; entry[2] = i == starts.size() ? 0xAA : static_cast<BYTE>(i + 1);
        const DWORD lba = i == starts.size() ? starts.back() + 3000 : starts[i];
        entry[4] = static_cast<BYTE>(lba >> 24); entry[5] = static_cast<BYTE>(lba >> 16);
        entry[6] = static_cast<BYTE>(lba >> 8); entry[7] = static_cast<BYTE>(lba);
    }
    return true;
}

int main(int argc, char**) {
    std::ostringstream display;
    auto* previousOutput = std::cout.rdbuf(display.rdbuf());
    const std::vector<DWORD> supplied{0,145,118,143,140,135,208,172,212,205,183,200,0,107,132,120,212,120,213,198,235};
    std::vector<DWORD> positions;
    for (size_t i = 0; i < supplied.size(); ++i) positions.push_back(static_cast<DWORD>(i * 3000));
    Reset(positions, supplied);
    DiscInfo disc; OpticalDrive drive;
    Expect("supplied 21-track scan succeeds", drive.ReadTOC(disc));
    for (size_t i = 0; i < supplied.size(); ++i) {
        Expect("supplied track " + std::to_string(i + 1), disc.tracks[i].index01LBA - disc.tracks[i].pregapLBA == supplied[i]);
    }
    std::ofstream("pregap-supplied-display.txt") << display.str();
    std::ofstream("pregap-supplied-reads.txt") << singles << ' ' << voted << '\n';
    std::cerr << "Supplied disc: " << singles << " single reads, " << voted << " voted reads\n";
    Expect("original track display", display.str().find("  Track  2: 1:70 (145 frames)\n") != std::string::npos);
    Expect("original first track display", display.str().find("  Track  1: 0:00 (  0 frames) pregap\n") != std::string::npos);
    if (argc > 1) { std::cout.rdbuf(previousOutput); return failures ? 1 : 0; }
    // A normal scan must retain the coarse/fine read budget. In particular,
    // do not replace the scanner with voted reads of every gap sector.
    Expect("normal disc retains coarse/fine read budget", singles == 413 && voted == 1042);

    for (DWORD gap : {0u,1u,2u,3u,150u,151u,435u,449u,450u,451u,465u,600u,900u,1500u,2999u}) GapCase(gap);
    GapCase(150, Fault::FalseCoarse);
    GapCase(0, Fault::FalseCoarse);
    GapCase(0, Fault::Separated);
    GapCase(0, Fault::RetrySeparated);
    GapCase(150, Fault::InteriorMissing);
    GapCase(600, Fault::LongWindowMissing);
    GapCase(1500, Fault::LongWindowMissing);
    GapCase(1, Fault::ShortEndMissing, 0);
    GapCase(2, Fault::ShortBeforeMissing, 0);
    GapCase(2, Fault::ShortWrongBefore, 0);

    Reset({0,3000}, {0,150}, Fault::NoQ);
    disc = {}; Expect("unreadable Q scan completes", drive.ReadTOC(disc));
    Expect("trusted track 1 TOC remains zero when Q fails", disc.tracks[0].index01LBA == 0);
    for (const auto missing : {Fault::FirstQMissing, Fault::FirstSeparated}) {
        Reset({0,3000}, {0,150}, missing);
        disc = {}; Expect("incomplete first-track Q scan completes", drive.ReadTOC(disc));
        Expect("missing first-track Q evidence never shifts the TOC origin", disc.tracks[0].index01LBA == 0);
    }
    Reset({750,3000}, {750,150}, Fault::FirstFalseCoarse);
    disc = {}; Expect("first-track false coarse hit scan completes", drive.ReadTOC(disc));
    Expect("first-track false coarse hit falls back to real gap", votedAt[750 - 145] > 0);

    Reset({0,3000}, {0,150});
    g_interrupt.SetInterrupted(true);
    disc = {}; Expect("already-cancelled scan returns failure", !drive.ReadTOC(disc));
    Expect("already-cancelled scan issues no Q reads", singles == 0 && voted == 0);
    for (const auto cancel : {Fault::CancelCoarse, Fault::CancelFine, Fault::CancelFirstRefinement}) {
        Reset(cancel == Fault::CancelFirstRefinement ? std::vector<DWORD>{750,3000} : std::vector<DWORD>{0,3000},
              cancel == Fault::CancelFirstRefinement ? std::vector<DWORD>{750,150} : std::vector<DWORD>{0,150}, cancel);
        disc = {};
        Expect("cancellation returns failure " + std::to_string(static_cast<int>(cancel)), !drive.ReadTOC(disc));
        Expect("cancelled scan does not inspect hidden audio", hiddenCalls == 0);
        Expect("cancelled scan issues no further Q reads", readsAfterCancel == 0);
        Expect("cancelled scan restores drive speed", speed == 0);
    }
    g_interrupt.SetInterrupted(false);
    std::cout.rdbuf(previousOutput);
    std::cerr << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
