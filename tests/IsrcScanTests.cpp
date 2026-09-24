#define NOMINMAX
#include "OpticalDrive.h"
#include "GlobalOptions.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
int commands = 0;
int failures = 0;
void Expect(bool condition, const char* description) {
    std::cerr << (condition ? "PASS: " : "FAIL: ") << description << '\n';
    if (!condition) ++failures;
}
}

// No physical drive or user preferences are changed by these tests.
void ScsiDrive::Close() {}
std::string ScsiDrive::GetSenseDescription(BYTE, BYTE, BYTE) { return {}; }
bool ScsiDrive::SendSCSIWithSense(void*, BYTE, void*, DWORD, BYTE*, BYTE*, BYTE*, bool, DWORD, DWORD*) {
    throw std::runtime_error("Unexpected CD-Text command");
}
bool ScsiDrive::SendSCSI(void* command, BYTE, void* output, DWORD size, bool, DWORD) {
    const auto* cdb = static_cast<const BYTE*>(command);
    if (cdb[0] != 0x42 || cdb[3] != 3 || size != 24)
        throw std::runtime_error("Unexpected metadata command");
    ++commands;
    auto* data = static_cast<BYTE*>(output);
    std::memset(data, 0, size);
    data[8] = 0x80;
    std::memcpy(data + 9, "USABC2600001", 12);
    return true;
}

int main() {
    OpticalDrive drive;
    DiscInfo disc;
    disc.tracks.resize(2);
    disc.tracks[0].isAudio = true;
    disc.tracks[0].trackNumber = 1;
    disc.tracks[1].isAudio = false;
    disc.tracks[1].trackNumber = 2;

    auto& disabled = GlobalOptions::DisableIsrcScanningState();
    disabled.store(GlobalOptions::kDefaultDisableIsrcScanning);
    Expect(!drive.ReadISRC(disc) && commands == 0,
        "Default setting skips all ISRC drive commands");

    disabled.store(false);
    Expect(drive.ReadISRC(disc) && commands == 1
        && disc.tracks[0].isrc == "USABC2600001" && disc.tracks[1].isrc.empty(),
        "Disabling the option reads ISRC from audio tracks only");

    disabled.store(true);
    Expect(!drive.ReadISRC(disc) && commands == 1
        && disc.tracks[0].isrc == "USABC2600001",
        "Re-enabling the option skips scans and preserves known metadata");
    return failures ? 1 : 0;
}
