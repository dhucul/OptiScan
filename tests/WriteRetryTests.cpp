#define NOMINMAX
#include "../OpticalDrive.h"
#include "../WriteDiscInternal.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>

// Link-time transport double. No test code opens a drive or calls DeviceIoControl.
namespace {
bool injectFailure = false, failedOnce = false, readinessTimedOut = false;
std::vector<BYTE> recorded;
}

bool ScsiDrive::SendSCSIWithSense(void* rawCdb, BYTE, void* buffer, DWORD bytes,
    BYTE* sk, BYTE* asc, BYTE* ascq, bool, DWORD) {
    if (sk) *sk = 0; if (asc) *asc = 0; if (ascq) *ascq = 0;
    const auto* cdb = static_cast<const BYTE*>(rawCdb);
    if (cdb[0] != 0x2A) throw std::runtime_error("Unexpected command in write retry test");
    const DWORD address = (static_cast<DWORD>(cdb[2]) << 24) |
        (static_cast<DWORD>(cdb[3]) << 16) | (static_cast<DWORD>(cdb[4]) << 8) | cdb[5];
    const int32_t lba = static_cast<int32_t>(address);
    if (lba < 0) return true; // the unchanged 150-sector SAO pregap
    if (injectFailure && !failedOnce) {
        failedOnce = true;
        *sk = 2; *asc = 4;
        return false;
    }
    const auto offset = static_cast<size_t>(lba) * AUDIO_SECTOR_SIZE;
    if (offset + bytes > recorded.size()) throw std::runtime_error("Write exceeded planned image");
    std::copy_n(static_cast<const BYTE*>(buffer), bytes, recorded.begin() + offset);
    return true;
}
std::string ScsiDrive::GetSenseDescription(BYTE, BYTE, BYTE) { return "injected test response"; }
bool WriteDiscInternal::WaitForDriveReady(ScsiDrive&, int seconds) {
    if (seconds == 30 && injectFailure && !readinessTimedOut) {
        readinessTimedOut = true;
        return false;
    }
    return true;
}
void WriteDiscInternal::DeinterleaveSubchannel(const BYTE*, BYTE*) {
    throw std::runtime_error("Unexpected raw subchannel path in SAO test");
}
size_t WriteDiscInternal::FindTrackForSector(
    const std::vector<OpticalDrive::TrackWriteInfo>&, DWORD, bool&) {
    throw std::runtime_error("Unexpected raw subchannel path in SAO test");
}

int RunWriteRetryTests() {
    int failures = 0;
    auto check = [&](bool value, const char* name) {
        std::cout << (value ? "[PASS] " : "[FAIL] ") << name << "\n";
        if (!value) ++failures;
    };
    const auto image = std::filesystem::temp_directory_path() /
        (L"OptiScanRetry-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    constexpr DWORD sectors = 60;
    std::vector<BYTE> expected(static_cast<size_t>(sectors) * AUDIO_SECTOR_SIZE);
    for (DWORD sector = 0; sector < sectors; ++sector)
        std::fill_n(expected.begin() + static_cast<size_t>(sector) * AUDIO_SECTOR_SIZE,
            AUDIO_SECTOR_SIZE, static_cast<BYTE>(sector + 1));
    { std::ofstream out(image, std::ios::binary);
      out.write(reinterpret_cast<const char*>(expected.data()), expected.size()); }
    OpticalDrive drive;
    OpticalDrive::TrackWriteInfo track{};
    track.trackNumber = 1; track.isAudio = true; track.endLBA = sectors - 1;
    for (bool fault : {false, true}) {
        injectFailure = fault; failedOnce = false; readinessTimedOut = false;
        recorded.assign(expected.size(), 0);
        check(drive.WriteAudioSectors(image.wstring(), L"", {track}, sectors, false, false, 0, ""),
            fault ? "Actual SAO write engine completes after WRITE failure and readiness timeout"
                  : "Actual SAO write engine completes the normal sequence");
        check(recorded == expected,
            fault ? "Retried SAO image is byte-identical with no skipped or shifted source sectors"
                  : "Normal SAO image preserves source bytes");
        if (fault) check(failedOnce && readinessTimedOut,
            "Regression exercised the exact not-ready timeout branch from the audit");
    }
    std::filesystem::remove(image);
    return failures;
}
