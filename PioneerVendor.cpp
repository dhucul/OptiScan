// ============================================================================
// PioneerVendor.cpp - Pioneer vendor feature implementation
// ============================================================================
#define NOMINMAX
#include "PioneerVendor.h"
#include "ScsiDrive.h"
#include "Constants.h"
#include "ConsoleColors.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <thread>

namespace {
    // Big-endian helpers.
    inline void StoreBE24(BYTE* p, uint32_t v) {
        p[0] = static_cast<BYTE>((v >> 16) & 0xFF);
        p[1] = static_cast<BYTE>((v >> 8) & 0xFF);
        p[2] = static_cast<BYTE>(v & 0xFF);
    }
    inline void StoreBE32(BYTE* p, uint32_t v) {
        p[0] = static_cast<BYTE>((v >> 24) & 0xFF);
        p[1] = static_cast<BYTE>((v >> 16) & 0xFF);
        p[2] = static_cast<BYTE>((v >> 8) & 0xFF);
        p[3] = static_cast<BYTE>(v & 0xFF);
    }
    inline uint32_t LoadBE32(const BYTE* p) {
        return (static_cast<uint32_t>(p[0]) << 24)
             | (static_cast<uint32_t>(p[1]) << 16)
             | (static_cast<uint32_t>(p[2]) << 8)
             |  static_cast<uint32_t>(p[3]);
    }
    inline uint16_t LoadBE16(const BYTE* p) {
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    }
}

bool PioneerVendor::IsPioneerDrive() {
    if (m_isPioneerCached >= 0) return m_isPioneerCached == 1;
    m_isPioneerCached = m_drive.IsPioneer() ? 1 : 0;
    return m_isPioneerCached == 1;
}

bool PioneerVendor::ReadBuffer(BYTE bufId, uint32_t offset, BYTE* dst,
    DWORD length, BYTE mode) {
    BYTE cdb[10] = {};
    cdb[0] = 0x3C;
    cdb[1] = mode;
    cdb[2] = bufId;
    StoreBE24(&cdb[3], offset);
    StoreBE24(&cdb[6], length);
    return m_drive.SendSCSI(cdb, 10, dst, length, /*dataIn=*/true);
}

bool PioneerVendor::WriteBuffer(BYTE bufId, uint32_t offset, const BYTE* src,
    DWORD length, BYTE mode) {
    BYTE cdb[10] = {};
    cdb[0] = 0x3B;
    cdb[1] = mode;
    cdb[2] = bufId;
    StoreBE24(&cdb[3], offset);
    StoreBE24(&cdb[6], length);
    // SendSCSI takes a non-const buffer; safe because dataIn=false treats it read-only.
    return m_drive.SendSCSI(cdb, 10, const_cast<BYTE*>(src), length, /*dataIn=*/false);
}

bool PioneerVendor::WriteFeatureCommand(BYTE cmdId, BYTE arg2, BYTE arg3,
    BYTE arg4, BYTE arg5, BYTE arg6) {
    BYTE payload[256] = {};
    payload[0] = cmdId;
    payload[1] = 0;
    payload[2] = arg2;
    payload[3] = arg3;
    payload[4] = arg4;
    payload[5] = arg5;
    payload[6] = arg6;
    return WriteBuffer(PioneerBufId::FeatureSet, 0, payload, sizeof(payload), /*mode=*/1);
}

bool PioneerVendor::ReadCapabilities(PioneerCapabilities& caps) {
    caps = {};
    if (!IsPioneerDrive()) return false;

    if (!ReadBuffer(PioneerBufId::Capabilities, 0, caps.raw, sizeof(caps.raw))) {
        return false;
    }
	if (std::all_of(std::begin(caps.raw), std::end(caps.raw),
		[](BYTE value) { return value == 0; })) return false;

    const BYTE* r = caps.raw;
    caps.valid = true;
    caps.isSupportedDrive          = (r[43] == 1);
    caps.advancedQuietCurrent      = r[2];
    caps.quietFallback             = r[3];
    caps.pureReadSupport           = (r[9] != 0);
    caps.recordingModeSupport      = (r[10] == 1);
    caps.bdRecordingMode           = r[11];
    caps.dvdRecordingMode          = r[13];
    caps.peakPowerReducerSupport   = (r[16] == 1);
    caps.peakPowerReducerOn        = (r[17] == 1);
    caps.smoothTraySupport         = (r[20] != 0xFF);
    caps.smoothTrayOn              = (r[20] == 1);
    caps.driveStatusSupport        = (r[22] != 0xFF);
    caps.driveStatus               = r[23];
    caps.ledOffSupport             = (r[24] != 0xFF);
    caps.ledOffOn                  = (r[24] == 1);
    caps.bdrHighSpeedSupport       = (r[26] != 0xFF);
    caps.bdrHighSpeedOn            = (r[26] == 1);
    caps.realTimePureReadOn        = (r[28] != 0);
    caps.realTimePureReadSupport   = (r[29] != 0);
    caps.highSpeedDataReadSupport  = (r[41] != 0xFF);
    caps.advancedQuietSupport      = (r[45] != 0);
    caps.cdCheckSupport            = (r[44] == 1);
    caps.discStatusSupport         = (r[46] == 1);
    caps.usbBusPowerSupport        = (r[47] == 1);
    caps.forceEjectSupport         = (r[48] == 1);
    caps.pureReadVersion           = r[49];
    caps.customEcoSupport          = (r[50] == 1);
    caps.selectTrackInspectionSupport = (r[52] == 1);
    caps.driveTypeCode             = r[53];
    caps.switchCdRomSpeedTableSupport = (r[54] == 1);
    caps.fragileCdSupport          = (r[55] != 0);
    caps.fragileCdOn               = (r[56] != 0);

    m_caps = caps;
    m_capsRead = true;
    return true;
}

const PioneerCapabilities& PioneerVendor::Capabilities() {
    if (!m_capsRead) {
        PioneerCapabilities tmp;
        ReadCapabilities(tmp);
        // Mark probed even on failure so we don't retry on every method call.
        m_capsRead = true;
    }
    return m_caps;
}

// ── Identification ──────────────────────────────────────────────────────────
bool PioneerVendor::GetHardwareVersion(uint32_t& versionHex, std::string& versionStr) {
    versionHex = 0;
    versionStr.clear();
    if (!IsPioneerDrive()) return false;

    BYTE buf[48] = {};
    if (!ReadBuffer(PioneerBufId::HardwareVersion, 0, buf, sizeof(buf))) return false;

    // ASCII hex at offset 20, typically 4 chars.
    char ascii[5] = {};
    std::memcpy(ascii, buf + 20, 4);
    versionStr.assign(ascii, 4);

    unsigned v = 0;
    if (sscanf_s(ascii, "%4x", &v) == 1) {
        versionHex = v;
    }
    return true;
}

bool PioneerVendor::GetSerialNumber(std::string& serial) {
    serial.clear();
    if (!IsPioneerDrive()) return false;

    // GET CONFIGURATION (0x46) starting at feature 0x0108 (Logical Unit
    // Serial Number). Ask for only that descriptor so offset 12 is unambiguous.
    BYTE cdb[10] = {};
    cdb[0] = 0x46;
    cdb[1] = 0x02;          // RT=2 (specified feature only)
    cdb[2] = 0x01;          // feature high
    cdb[3] = 0x08;          // feature low
    BYTE buf[32] = {};
    cdb[7] = 0;
    cdb[8] = sizeof(buf);
    if (!m_drive.SendSCSI(cdb, 10, buf, sizeof(buf), true)) return false;
	if ((static_cast<WORD>(buf[8]) << 8 | buf[9]) != 0x0108 || buf[11] < 12)
		return false;

    // Per Pioneer utility: copy 12 bytes starting at offset 12.
    char s[13] = {};
    std::memcpy(s, buf + 12, 12);
    // Trim trailing spaces/nulls.
    int len = 12;
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == 0)) --len;
    serial.assign(s, len);
    return !serial.empty();
}

// ── PureRead ────────────────────────────────────────────────────────────────
bool PioneerVendor::GetPureReadMode(PureReadMode& mode, bool& realTimeEnabled) {
    mode = PureReadMode::Off;
    realTimeEnabled = false;

    PioneerCapabilities caps;
    if (!ReadCapabilities(caps)) return false;
    if (!caps.pureReadSupport) return false;

    const BYTE* r = caps.raw;
    if (r[4] == 0) mode = PureReadMode::Off;
    else if (r[5] == 254) mode = PureReadMode::Perfect;
    else mode = PureReadMode::Master;

    // Pioneer BD Drive Utility reads byte 28 as the Real-Time PureRead state.
    realTimeEnabled = (r[28] != 0);
    return true;
}

bool PioneerVendor::SetPureReadMode(PureReadMode mode, bool realTimeEnabled,
    bool eepSave) {
    if (!IsPioneerDrive()) return false;
    const auto& caps = Capabilities();
    if (!caps.pureReadSupport) return false;

    BYTE payload[256] = {};
    payload[0] = PioneerCmdId::PureRead;
    payload[1] = 0x80;
    switch (mode) {
    case PureReadMode::Off:
        payload[2] = 0;
        break;
    case PureReadMode::Master:
        payload[2] = 1;
        payload[3] = 8;
        payload[4] = 64;
        break;
    case PureReadMode::Perfect:
        payload[2] = 1;
        payload[3] = 254;
        payload[4] = 1;
        break;
    }
    payload[5] = eepSave ? 1 : 0;
    payload[6] = (realTimeEnabled && caps.realTimePureReadSupport) ? 1 : 0;
    return WriteBuffer(PioneerBufId::FeatureSet, 0, payload, sizeof(payload), 1);
}

PioneerPureReadModeGuard::PioneerPureReadModeGuard(ScsiDrive& drive,
    bool active, PureReadMode requestedMode, bool requestRealTime)
    : m_pioneer(drive), m_active(active), m_requestedMode(requestedMode) {
    if (!m_active) return;

    if (!m_pioneer.GetPureReadMode(m_previousMode, m_previousRealTime))
        return;
    m_haveSnapshot = true;

    const auto& caps = m_pioneer.Capabilities();
    m_requestedRealTime = requestRealTime && caps.valid &&
        caps.realTimePureReadSupport;
    Reapply();
}

bool PioneerPureReadModeGuard::Reapply() {
    m_engaged = false;
    if (!m_active || !m_haveSnapshot) return false;

    PureReadMode currentMode = PureReadMode::Off;
    bool currentRealTime = false;
    if (m_pioneer.GetPureReadMode(currentMode, currentRealTime) &&
        currentMode == m_requestedMode &&
        currentRealTime == m_requestedRealTime) {
        if (currentMode != m_previousMode ||
            currentRealTime != m_previousRealTime) {
            m_restore = true;
        }
        m_engaged = true;
        return true;
    }

    if (!m_pioneer.SetPureReadMode(m_requestedMode, m_requestedRealTime,
        /*eepSave=*/false)) {
        return false;
    }
    m_restore = true;

    if (m_pioneer.GetPureReadMode(currentMode, currentRealTime) &&
        currentMode == m_requestedMode &&
        currentRealTime == m_requestedRealTime) {
        m_engaged = true;
    }
    return m_engaged;
}

bool PioneerPureReadModeGuard::Restore() {
    m_engaged = false;
    if (!m_restore) return true;

    if (!m_pioneer.SetPureReadMode(m_previousMode, m_previousRealTime,
        /*eepSave=*/false)) {
        return false;
    }

    PureReadMode restoredMode = PureReadMode::Off;
    bool restoredRealTime = false;
    if (!m_pioneer.GetPureReadMode(restoredMode, restoredRealTime) ||
        restoredMode != m_previousMode ||
        restoredRealTime != m_previousRealTime) {
        return false;
    }

    m_restore = false;
    return true;
}

PioneerPureReadModeGuard::~PioneerPureReadModeGuard() {
    if (!Restore()) {
        Console::Warning("Pioneer PureRead state could not be restored; "
            "power-cycle the drive before relying on another read.\n");
    }
}

PioneerPureReadOffGuard::PioneerPureReadOffGuard(ScsiDrive& drive, bool active)
    : m_pioneer(drive), m_active(active) {
    if (!m_active) return;

    PioneerCapabilities capabilities;
    if (!m_pioneer.ReadCapabilities(capabilities))
        return;
    if (!capabilities.pureReadSupport) {
        m_engaged = true;
        return;
    }

    if (!m_pioneer.GetPureReadMode(m_previousMode, m_previousRealTime))
        return;

    // Nothing to change (and nothing to restore) if both the PureRead mode and
    // the Real-Time PureRead flag are already off. This is also what makes the
    // guard nest correctly: an inner guard reads back Off/off and no-ops.
    if (m_previousMode == PureReadMode::Off && !m_previousRealTime) {
        m_engaged = true;
        return;
    }

    // Force both error-hiding features off for the scan. Only arm the restore
    // if the write actually took, so a failed Set doesn't leave us "restoring"
    // to a state we never left.
    if (m_pioneer.SetPureReadMode(PureReadMode::Off, /*realTimeEnabled=*/false,
                                  /*eepSave=*/false)) {
        m_restore = true;
        PureReadMode currentMode = PureReadMode::Master;
        bool currentRealTime = true;
        if (m_pioneer.GetPureReadMode(currentMode, currentRealTime) &&
            currentMode == PureReadMode::Off && !currentRealTime) {
            m_engaged = true;
        }
    }
}

bool PioneerPureReadOffGuard::Restore() {
    m_engaged = false;
    if (!m_restore) return true;

    if (!m_pioneer.SetPureReadMode(m_previousMode, m_previousRealTime,
        /*eepSave=*/false)) {
        return false;
    }

    PureReadMode restoredMode = PureReadMode::Off;
    bool restoredRealTime = false;
    if (!m_pioneer.GetPureReadMode(restoredMode, restoredRealTime) ||
        restoredMode != m_previousMode ||
        restoredRealTime != m_previousRealTime) {
        return false;
    }

    m_restore = false;
    return true;
}

PioneerPureReadOffGuard::~PioneerPureReadOffGuard() {
    if (!Restore()) {
        Console::Warning("Pioneer PureRead state could not be restored after the diagnostic; "
            "power-cycle the drive before relying on another read.\n");
    }
}

PioneerPerformanceModeGuard::PioneerPerformanceModeGuard(ScsiDrive& drive, bool active)
    : m_pioneer(drive), m_active(active) {
    if (!m_active) return;

    const auto& caps = m_pioneer.Capabilities();
    // Match ApplyAudioExtractionPreset's gate: advancedQuietSupport (byte 45)
    // is the official "speed mode is changeable" flag. Some drives expose
    // a valid current-mode byte without honoring writes, so byte 45 is the
    // authoritative check.
    if (!caps.valid || !caps.advancedQuietSupport)
        return;

    // Only attempt restore if the current-mode read-back is a known enum
    // value. 0xFF means "not reported" and anything > 3 is an undocumented
    // mode we shouldn't pretend to recognize.
    BYTE current = caps.advancedQuietCurrent;
    if (current > 3)
        return;

    m_previousMode = static_cast<PioneerSpeedMode>(current);
    if (m_previousMode == PioneerSpeedMode::Performance)
        return;  // Already in performance mode

    if (m_pioneer.SetSpeedMode(PioneerSpeedMode::Performance, /*eepSave=*/false)) {
        m_restore = true;
    }
}

PioneerPerformanceModeGuard::~PioneerPerformanceModeGuard() {
    if (m_restore) {
        m_pioneer.SetSpeedMode(m_previousMode, /*eepSave=*/false);
    }
}

PioneerCdInspectionGuard::PioneerCdInspectionGuard(ScsiDrive& drive, bool active)
    : m_pioneer(drive), m_active(active) {
    if (!m_active) return;

    // The Pioneer utility enters prepare state before inspecting the loaded
    // audio disc, then enters inspection state immediately before measurement.
    // Arm cleanup before the writes so every partial transition gets mode 2.
    m_restore = true;
    m_pioneer.SetCdInspectionMode(0);
    m_inspectionModeActive = m_pioneer.SetCdInspectionMode(1);
}

PioneerCdInspectionGuard::~PioneerCdInspectionGuard() {
    if (m_restore)
        m_pioneer.SetCdInspectionMode(2);
}

bool PioneerVendor::GetRealTimePureReadStatus(PioneerRtPureReadStatus& status) {
    status = {};
    if (!IsPioneerDrive()) return false;
    const auto& caps = Capabilities();
    if (!caps.realTimePureReadSupport) return false;

    BYTE buf[32] = {};
    if (!ReadBuffer(PioneerBufId::RealTimePureRead, 0, buf, sizeof(buf))) return false;

    status.valid = true;
    status.errorSectors = LoadBE32(buf + 0);
    status.playSectors  = LoadBE32(buf + 4);
    status.currentLBA   = LoadBE32(buf + 8);
    return true;
}

bool PioneerVendor::ClearRealTimePureReadStatus() {
    if (!IsPioneerDrive()) return false;
    const auto& caps = Capabilities();
    if (!caps.realTimePureReadSupport) return false;

    BYTE empty = 0;
    BYTE cdb[10] = {};
    cdb[0] = 0x3B;
    cdb[1] = 0x02;
    cdb[2] = PioneerBufId::RealTimePureRead;
    return m_drive.SendSCSI(cdb, 10, &empty, 0, /*dataIn=*/false);
}

// ── Real-Time PureRead rip diagnostics ──────────────────────────────────────
PioneerPureReadSummary AssessPioneerPureRead(uint32_t errorSectors,
    uint32_t transferredSectors, uint32_t lastLBA) {
    PioneerPureReadSummary summary;
    summary.valid = true;
    summary.errorSectors = errorSectors;
    summary.transferredSectors = transferredSectors;
    summary.lastLBA = lastLBA;

    // The Pioneer utility maps a zero transfer count to level 0. For a rip
    // report that would look like a clean measurement when no measurement was
    // actually collected, so retain it explicitly as No Data instead.
    if (transferredSectors == 0) {
        summary.assessment = PioneerPureReadAssessment::NoData;
        summary.indicatorLevel = -1;
        return summary;
    }

    summary.errorRatio = static_cast<double>(errorSectors)
        / static_cast<double>(transferredSectors);
    if (errorSectors == 0) {
        summary.indicatorLevel = 0;
        summary.assessment = PioneerPureReadAssessment::Perfect;
    }
    else if (summary.errorRatio < 0.000125) {
        summary.indicatorLevel = 1;
        summary.assessment = PioneerPureReadAssessment::Better;
    }
    else if (summary.errorRatio < 0.00025) {
        summary.indicatorLevel = 2;
        summary.assessment = PioneerPureReadAssessment::Good;
    }
    else if (summary.errorRatio < 0.000525) {
        summary.indicatorLevel = 3;
        summary.assessment = PioneerPureReadAssessment::NotGood;
    }
    else if (summary.errorRatio < 0.001155) {
        summary.indicatorLevel = 4;
        summary.assessment = PioneerPureReadAssessment::NotGood;
    }
    else if (summary.errorRatio < 0.0026565) {
        summary.indicatorLevel = 5;
        summary.assessment = PioneerPureReadAssessment::NotGood;
    }
    else if (summary.errorRatio < 0.0079695) {
        summary.indicatorLevel = 6;
        summary.assessment = PioneerPureReadAssessment::Bad;
    }
    else if (summary.errorRatio < 0.031878) {
        summary.indicatorLevel = 7;
        summary.assessment = PioneerPureReadAssessment::Bad;
    }
    else {
        summary.indicatorLevel = 8;
        summary.assessment = PioneerPureReadAssessment::Fatal;
    }
    return summary;
}

const char* PioneerPureReadAssessmentName(PioneerPureReadAssessment assessment) {
    switch (assessment) {
    case PioneerPureReadAssessment::Perfect: return "Perfect";
    case PioneerPureReadAssessment::Better:  return "Better";
    case PioneerPureReadAssessment::Good:    return "Good";
    case PioneerPureReadAssessment::NotGood: return "Not Good";
    case PioneerPureReadAssessment::Bad:     return "Bad";
    case PioneerPureReadAssessment::Fatal:   return "Fatal";
    default:                                 return "No Data";
    }
}

std::string FormatPioneerPureReadMsf(uint32_t lba) {
    const uint32_t minutes = lba / (75u * 60u);
    const uint32_t seconds = (lba / 75u) % 60u;
    const uint32_t frames = lba % 75u;
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << minutes << ':'
        << std::setw(2) << seconds << ':' << std::setw(2) << frames;
    return out.str();
}

bool PioneerPureReadSession::Begin() {
    if (m_started || m_finished) return false;

    PureReadMode mode = PureReadMode::Off;
    bool realTimeEnabled = false;
    if (!m_pioneer.GetPureReadMode(mode, realTimeEnabled)
        || mode == PureReadMode::Off || !realTimeEnabled) {
        return false;
    }

    PioneerRtPureReadStatus before;
    const bool haveBefore = m_pioneer.GetRealTimePureReadStatus(before);
    const bool cleared = m_pioneer.ClearRealTimePureReadStatus();

    if (cleared) {
        // Use the post-clear values when the firmware exposes them immediately;
        // otherwise retain the pre-clear snapshot. If the firmware really did
        // reset, Finish() detects the lower final counters and safely uses them
        // directly; if it did not, subtracting this snapshot excludes stale data.
        PioneerRtPureReadStatus afterClear;
        if (m_pioneer.GetRealTimePureReadStatus(afterClear)) {
            m_baseErrorSectors = afterClear.errorSectors;
            m_baseTransferredSectors = afterClear.playSectors;
        }
        else if (haveBefore) {
            m_baseErrorSectors = before.errorSectors;
            m_baseTransferredSectors = before.playSectors;
        }
    }
    else if (haveBefore) {
        // Clearing is optional for correctness: subtract the cumulative values
        // observed immediately before the rip.
        m_baseErrorSectors = before.errorSectors;
        m_baseTransferredSectors = before.playSectors;
    }
    else {
        return false;
    }

    m_started = true;
    return true;
}

bool PioneerPureReadSession::Finish(PioneerPureReadSummary& summary) {
    if (m_finished) {
        summary = m_cachedSummary;
        return summary.valid;
    }
    if (!m_started) return false;

    PioneerRtPureReadStatus finalStatus;
    constexpr int kStatusRetries = 3;
    bool haveFinalStatus = false;
    for (int attempt = 0; attempt < kStatusRetries; ++attempt) {
        if (m_pioneer.GetRealTimePureReadStatus(finalStatus)) {
            haveFinalStatus = true;
            break;
        }
        if (attempt + 1 < kStatusRetries)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Leave the session unfinished on failure so a later Finish() call can
    // retry instead of permanently returning a cached invalid summary.
    if (!haveFinalStatus) return false;

    // A lower final value means the firmware reset the counter after Begin;
    // use the final value rather than turning it into an unsigned underflow.
    const uint32_t errorDelta = (finalStatus.errorSectors >= m_baseErrorSectors)
        ? finalStatus.errorSectors - m_baseErrorSectors
        : finalStatus.errorSectors;
    const uint32_t transferredDelta = (finalStatus.playSectors >= m_baseTransferredSectors)
        ? finalStatus.playSectors - m_baseTransferredSectors
        : finalStatus.playSectors;

    m_cachedSummary = AssessPioneerPureRead(errorDelta, transferredDelta,
        finalStatus.currentLBA);
    m_finished = true;
    summary = m_cachedSummary;
    return true;
}

void PrintPioneerPureReadSummary(const PioneerPureReadSummary& summary) {
    if (!summary.valid) return;
	const std::ios::fmtflags savedFlags = std::cout.flags();
	const std::streamsize savedPrecision = std::cout.precision();

    Console::Heading("\n=== Pioneer Real-Time PureRead Summary ===\n");
    std::cout << "  Error sectors:       " << summary.errorSectors << "\n";
    std::cout << "  Transferred sectors: " << summary.transferredSectors << "\n";
    if (summary.transferredSectors > 0) {
        std::cout << "  Error ratio:         " << std::fixed << std::setprecision(6)
            << summary.errorRatio << "  (" << std::setprecision(4)
            << summary.errorRatio * 100.0 << "%)\n" << std::defaultfloat;
        std::cout << "  Indicator level:     " << summary.indicatorLevel << "/8\n";
    }
    else {
        std::cout << "  Error ratio:         unavailable (no transfer count)\n";
        std::cout << "  Indicator level:     unavailable\n";
    }
    std::cout << "  Assessment:          "
        << PioneerPureReadAssessmentName(summary.assessment) << "\n";
    std::cout << "  Last transfer:       LBA " << summary.lastLBA << "  ("
        << FormatPioneerPureReadMsf(summary.lastLBA) << ")\n";
	std::cout.flags(savedFlags);
	std::cout.precision(savedPrecision);

    if (summary.transferredSectors == 0) {
        Console::Warning("  PureRead returned no transferred-sector count; no clean/error verdict was assigned.\n");
    }
    else if (summary.errorSectors == 0) {
        Console::Success("  PureRead reported no error-sector events during this read session.\n");
    }
    else {
        Console::Warning("  PureRead handled one or more firmware-detected error sectors during this read session.\n");
    }
    Console::Info("  Diagnostic only: this is not verified C2/CU data or proof that the output is incorrect.\n");
}

bool SavePioneerPureReadSummary(const std::wstring& filename,
    const PioneerPureReadSummary& summary, const std::string& workflow,
    const std::string& readOutcome) {
    if (!summary.valid) return false;

    std::ofstream log(std::filesystem::path(filename), std::ios::out | std::ios::trunc);
    if (!log) return false;

    log << "# Pioneer Real-Time PureRead Rip Summary\n";
    log << "Workflow: " << workflow << "\n";
    log << "Read phase: " << readOutcome << "\n";
    log << "PureRead error sectors: " << summary.errorSectors << "\n";
    log << "Transferred sectors: " << summary.transferredSectors << "\n";
    if (summary.transferredSectors > 0) {
        log << "Error ratio: " << std::fixed << std::setprecision(8)
            << summary.errorRatio << "\n";
        log << "Error percentage: " << std::setprecision(6)
            << summary.errorRatio * 100.0 << "%\n";
        log << "Indicator level: " << summary.indicatorLevel << "/8\n";
    }
    else {
        log << "Error ratio: unavailable\n";
        log << "Indicator level: unavailable\n";
    }
    log << "Assessment: " << PioneerPureReadAssessmentName(summary.assessment) << "\n";
    log << "Last transfer LBA: " << summary.lastLBA << "\n";
    log << "Last transfer MSF: " << FormatPioneerPureReadMsf(summary.lastLBA) << "\n";
    log << "\n# This is a Pioneer firmware diagnostic. It is not verified C2/CU data\n";
    log << "# and does not by itself prove that the extracted audio is incorrect.\n";
    log << "# Use secure rereads, physical comparison, or AccurateRip for integrity.\n";
    return static_cast<bool>(log);
}

// ── Quiet / Performance ─────────────────────────────────────────────────────
bool PioneerVendor::SetSpeedMode(PioneerSpeedMode mode, bool eepSave,
    int readMultiplier, int writeMultiplier) {
    if (!IsPioneerDrive()) return false;
    BYTE modeByte = static_cast<BYTE>(mode);
    // SetCdSpeedPioneer applies the 0x80 / 0xC0 prefix internally.
    return m_drive.SetCdSpeedPioneer(readMultiplier, modeByte, eepSave, writeMultiplier);
}

// ── Boolean toggles ─────────────────────────────────────────────────────────
bool PioneerVendor::SetPeakPowerReducer(bool on, bool eepSave) {
    if (!Capabilities().peakPowerReducerSupport) return false;
    return WriteFeatureCommand(PioneerCmdId::PeakPowerReducer, on ? 1 : 0, eepSave ? 1 : 0);
}

bool PioneerVendor::SetSmoothTray(bool on, bool eepSave) {
    if (!Capabilities().smoothTraySupport) return false;
    return WriteFeatureCommand(PioneerCmdId::TraySmooth, on ? 1 : 0, eepSave ? 1 : 0);
}

bool PioneerVendor::SetLedOff(bool on, bool eepSave) {
    if (!Capabilities().ledOffSupport) return false;
    return WriteFeatureCommand(PioneerCmdId::LedOff, on ? 1 : 0, eepSave ? 1 : 0);
}

bool PioneerVendor::SetBdrHighSpeedRecording(bool on, bool eepSave) {
    if (!Capabilities().bdrHighSpeedSupport) return false;
    return WriteFeatureCommand(PioneerCmdId::BdrHighSpeed, on ? 1 : 0, eepSave ? 1 : 0);
}

bool PioneerVendor::SetHighSpeedDataRead(bool on, bool eepSave) {
    if (!Capabilities().highSpeedDataReadSupport) return false;
    return WriteFeatureCommand(PioneerCmdId::HighSpeedRead, on ? 1 : 0, eepSave ? 1 : 0);
}

bool PioneerVendor::SetFragileCdMode(bool on, bool eepSave) {
    if (!Capabilities().fragileCdSupport) return false;
    // Per utility: w[2]=eepSave, w[3]=off-flag (1 means disable).
    BYTE off = on ? 0 : 1;
    return WriteFeatureCommand(PioneerCmdId::RentalCdMode, eepSave ? 1 : 0, off);
}

// ── Recording mode ──────────────────────────────────────────────────────────
bool PioneerVendor::SetRecordingMode(PioneerRecordingMode bdMode,
    PioneerRecordingMode dvdMode, bool eepSave) {
    if (!Capabilities().recordingModeSupport) return false;
    return WriteFeatureCommand(PioneerCmdId::RecordingMode,
        static_cast<BYTE>(bdMode), static_cast<BYTE>(dvdMode), eepSave ? 1 : 0);
}

// ── Utility commands via 0xE6 ───────────────────────────────────────────────
bool PioneerVendor::ForceEject() {
    if (!Capabilities().forceEjectSupport) return false;
    BYTE empty = 0;
    // WRITE BUFFER mode=2, id=0xE6, offset=0x20000, length=0.
    BYTE cdb[10] = {};
    cdb[0] = 0x3B;
    cdb[1] = 0x02;
    cdb[2] = PioneerBufId::Utility;
    StoreBE24(&cdb[3], 0x20000);
    // length stays 0.
    return m_drive.SendSCSI(cdb, 10, &empty, 0, /*dataIn=*/false);
}

bool PioneerVendor::RunBusPowerCheck(uint32_t& reading) {
    reading = 0;
    if (!Capabilities().usbBusPowerSupport) return false;

    BYTE dummy = 0;
    BYTE cdbW[10] = {};
    cdbW[0] = 0x3B;
    cdbW[1] = 0x02;
    cdbW[2] = PioneerBufId::Utility;
    StoreBE24(&cdbW[3], 0x10000);
    if (!m_drive.SendSCSI(cdbW, 10, &dummy, 0, /*dataIn=*/false)) return false;

    BYTE buf[8] = {};
    BYTE cdbR[10] = {};
    cdbR[0] = 0x3C;
    cdbR[1] = 0x02;
    cdbR[2] = PioneerBufId::Utility;
    StoreBE24(&cdbR[3], 0x10000);
    StoreBE24(&cdbR[6], sizeof(buf));
    if (!m_drive.SendSCSI(cdbR, 10, buf, sizeof(buf), /*dataIn=*/true)) return false;

    reading = LoadBE32(buf + 4);
    return true;
}

// ── CD Check ────────────────────────────────────────────────────────────────
// The capability-flag gate is intentionally omitted: byte 44 of the 0xF4 block
// disagrees with reality on some Pioneer firmwares (e.g. BDR-S13U reports the
// flag as 0 even though the 0xE6+0x300000 protocol is implemented). The drive
// is the authoritative oracle — it will reject the SCSI command with a sense
// error if the feature is genuinely unsupported.
bool PioneerVendor::SetCdInspectionMode(BYTE mode) {
    if (!IsPioneerDrive() || mode > 2) return false;

    BYTE payload[32] = {};
    payload[0] = 0x91;
    payload[1] = 0x60;
    payload[2] = mode;
    return WriteBuffer(PioneerBufId::McDirectAlt, 0, payload,
        static_cast<DWORD>(sizeof(payload)), 2);
}

bool PioneerVendor::CdCheckStart(uint32_t startLBA, uint32_t unitSize) {
    BYTE sk = 0, asc = 0, ascq = 0;
    return CdCheckStartWithSense(startLBA, unitSize, sk, asc, ascq);
}

bool PioneerVendor::CdCheckStartWithSense(uint32_t startLBA, uint32_t unitSize,
    BYTE& senseKey, BYTE& asc, BYTE& ascq) {
    senseKey = asc = ascq = 0;
    if (!IsPioneerDrive()) return false;
    BYTE payload[32] = {};
    payload[0] = 0xFF;
    payload[1] = 0x02;
    payload[2] = 0;     // start
    StoreBE32(&payload[3], startLBA);
    StoreBE32(&payload[11], unitSize);
    BYTE cdb[10] = {};
    cdb[0] = 0x3B;
    cdb[1] = 0x02;
    cdb[2] = PioneerBufId::Utility;
    StoreBE24(&cdb[3], 0x300000);
    StoreBE24(&cdb[6], sizeof(payload));
    return m_drive.SendSCSIWithSense(cdb, 10, payload, sizeof(payload),
        &senseKey, &asc, &ascq, /*dataIn=*/false);
}

bool PioneerVendor::CdCheckStop() {
    if (!IsPioneerDrive()) return false;
    BYTE payload[32] = {};
    payload[0] = 0xFF;
    payload[1] = 0x02;
    payload[2] = 1;     // stop
    BYTE cdb[10] = {};
    cdb[0] = 0x3B;
    cdb[1] = 0x02;
    cdb[2] = PioneerBufId::Utility;
    StoreBE24(&cdb[3], 0x300000);
    StoreBE24(&cdb[6], sizeof(payload));
    return m_drive.SendSCSI(cdb, 10, payload, sizeof(payload), /*dataIn=*/false);
}

bool PioneerVendor::CdCheckRead(PioneerCdCheckResult& result) {
    result = {};
    if (!IsPioneerDrive()) return false;
    BYTE buf[64] = {};
    BYTE cdb[10] = {};
    cdb[0] = 0x3C;
    cdb[1] = 0x02;
    cdb[2] = PioneerBufId::Utility;
    StoreBE24(&cdb[3], 0x300000);
    StoreBE24(&cdb[6], sizeof(buf));
    if (!m_drive.SendSCSI(cdb, 10, buf, sizeof(buf), /*dataIn=*/true)) return false;

    result.valid = true;
    result.c1Uncorrectable = LoadBE16(buf + 4);
    result.c2Uncorrectable = LoadBE16(buf + 14);
    result.endAddress = LoadBE32(buf + 18);
    uint32_t validity = LoadBE32(buf + 22);
    result.dataValid = (validity != 0xFFFFFFFFu);
    result.tePeak = LoadBE16(buf + 60);
    result.teIntegrationMax = LoadBE16(buf + 62);
    result.teDataValid = (result.tePeak != 0xFFFF && result.teIntegrationMax != 0xFFFF);
    return true;
}

bool PioneerVendor::GetCdPhysicalTrackParameters(uint32_t leadoutInnerAddress,
    double& linearVelocity, double& trackPitch) {
    linearVelocity = 0.0;
    trackPitch = 0.0;
    if (!IsPioneerDrive()) return false;

    BYTE payload[32] = {};
    payload[0] = 0x50;
    payload[1] = 0x54;
    StoreBE32(payload + 5, leadoutInnerAddress);
    payload[12] = 1;
    if (!WriteBuffer(PioneerBufId::McDirect, 0, payload,
        static_cast<DWORD>(sizeof(payload)), 2)) {
        return false;
    }

    BYTE response[32] = {};
    if (!ReadBuffer(PioneerBufId::McDirect, 0, response,
        static_cast<DWORD>(sizeof(response)), 2)) {
        return false;
    }

    linearVelocity = static_cast<double>(LoadBE32(response + 3)) * 6.103515625e-05;
    trackPitch = static_cast<double>(LoadBE32(response + 7)) * 0.0001220703125;
    return linearVelocity > 0.0 && trackPitch > 0.0;
}

PioneerCdCheckGrade GradePioneerCdCheckSample(const PioneerCdCheckResult& result) {
    // Pioneer BD Drive Utility's CInspectionResult::SetData thresholds.
    if (!result.valid || !result.dataValid)
        return PioneerCdCheckGrade::D;
    if (result.teDataValid && result.teIntegrationMax > 1140 && result.tePeak >= 45)
        return PioneerCdCheckGrade::D;
    if (result.c2Uncorrectable > 15)
        return PioneerCdCheckGrade::D;
    if (result.c2Uncorrectable != 0)
        return PioneerCdCheckGrade::C;
    if (result.c1Uncorrectable > 25)
        return PioneerCdCheckGrade::B;
    return PioneerCdCheckGrade::A;
}

const char* PioneerCdCheckGradeName(PioneerCdCheckGrade grade) {
    switch (grade) {
    case PioneerCdCheckGrade::A: return "A";
    case PioneerCdCheckGrade::B: return "B";
    case PioneerCdCheckGrade::C: return "C";
    case PioneerCdCheckGrade::D: return "D";
    }
    return "?";
}

// ── Media code / Media ID / write protection ────────────────────────────────
namespace {
// Disc-type labels keyed by the low byte of the Pioneer media code.
struct DiscTypeEntry { BYTE code; const char* label; };
constexpr DiscTypeEntry kDiscTypeTable[] = {
    { 0x00, "CD-ROM" },
    { 0x20, "CD-R" },
    { 0x10, "CD-RW" },
    { 0x40, "DVD-ROM Single" },
    { 0x44, "DVD-ROM Dual" },
    { 0x46, "DVD-ROM Dual" },
    { 0x50, "DVD-RW" },
    { 0x60, "DVD-R Single" },
    { 0x68, "DVD-R 3.95GB Single" },
    { 0x51, "DVD+RW" },
    { 0x61, "DVD+R Single" },
    { 0x66, "DVD-R Dual" },
    { 0x67, "DVD+R Dual" },
    { 0x70, "DVD-RAM" },
    { 0x80, "BD-ROM Single" },
    { 0x86, "BD-ROM Dual" },
    { 0x8A, "BD-ROM Triple" },
    { 0x90, "BD-RE Single" },
    { 0x96, "BD-RE Dual" },
    { 0x9A, "BD-RE Triple" },
    { 0xA0, "BD-R Single" },
    { 0xA6, "BD-R Dual" },
    { 0xAA, "BD-R Triple" },
    { 0xAE, "BD-R Quadruple" },
};

PioneerMediaFamily FamilyFromLowByte(BYTE lo) {
    switch (lo) {
    case 0x00: return PioneerMediaFamily::CDROM;
    case 0x20: case 0x10: return PioneerMediaFamily::CDR;
    case 0x40: case 0x44: case 0x46: return PioneerMediaFamily::DVDROM;
    case 0x50: case 0x60: case 0x68: case 0x66: return PioneerMediaFamily::DVDDashR;
    case 0x51: case 0x61: case 0x67: return PioneerMediaFamily::DVDPlusR;
    case 0x70: return PioneerMediaFamily::DVDRAM;
    case 0x80: case 0x86: case 0x8A: return PioneerMediaFamily::BDROM;
    case 0x90: case 0x96: case 0x9A:
    case 0xA0: case 0xA6: case 0xAA: case 0xAE:
        return PioneerMediaFamily::BDR;
    default: return PioneerMediaFamily::Unknown;
    }
}

// Replace zeros with underscores; then trim trailing underscores to spaces.
std::string CleanAsciiMediaId(const BYTE* p, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        BYTE b = p[i];
        if (b == 0) out.push_back('_');
        else if (b < 0x20 || b > 0x7E) break;  // stop at non-printable
        else out.push_back(static_cast<char>(b));
    }
    // Convert trailing underscores to spaces, then rtrim.
    while (!out.empty() && out.back() == '_') out.pop_back();
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return out;
}

// READ DISC STRUCTURE (0xAD) helper.
bool DoReadDiscStructure(ScsiDrive& drive, BYTE mediaType, BYTE format,
    BYTE* dst, uint16_t allocLen) {
    BYTE cdb[12] = {};
    cdb[0] = 0xAD;
    cdb[1] = mediaType & 0x0F;     // MediaType nibble
    cdb[7] = format;
    cdb[8] = static_cast<BYTE>((allocLen >> 8) & 0xFF);
    cdb[9] = static_cast<BYTE>(allocLen & 0xFF);
    return drive.SendSCSI(cdb, 12, dst, allocLen, /*dataIn=*/true);
}
}  // namespace

bool PioneerVendor::GetMediaInfo(PioneerMediaInfo& info) {
    info = {};
    if (!IsPioneerDrive()) return false;

    // Step 1: WRITE BUFFER 0xE1 with command 0x91 0x40 to request media code.
    BYTE wpayload[32] = {};
    wpayload[0] = 0x91;
    wpayload[1] = 0x40;
    if (!WriteBuffer(PioneerBufId::McDirectAlt, 0, wpayload, sizeof(wpayload), /*mode=*/2))
        return false;

    // Step 2: READ BUFFER 0xE1 to retrieve the response.
    BYTE rbuf[32] = {};
    if (!ReadBuffer(PioneerBufId::McDirectAlt, 0, rbuf, sizeof(rbuf), /*mode=*/2))
        return false;

    info.valid = true;
    info.mediaCode = static_cast<uint16_t>((rbuf[10] << 8) | rbuf[6]);
    info.lowByte = static_cast<BYTE>(info.mediaCode & 0xFF);
    info.family = FamilyFromLowByte(info.lowByte);

    const char* label = "Unknown Disc";
    for (const auto& e : kDiscTypeTable) {
        if (e.code == info.lowByte) { label = e.label; break; }
    }
    info.discTypeLabel = label;
    return true;
}

bool PioneerVendor::GetMediaId(const PioneerMediaInfo& info, std::string& id) {
    id.clear();
    if (!info.valid) return false;

    BYTE buf[2048] = {};

    switch (info.family) {
    case PioneerMediaFamily::BDR: {
        // READ DISC STRUCTURE mediaType=1, format=0; ASCII at offset 104, length 9.
        if (!DoReadDiscStructure(m_drive, 1, 0x00, buf, 256)) return false;
        id = CleanAsciiMediaId(buf + 104, 9);
        return !id.empty();
    }
    case PioneerMediaFamily::DVDDashR: {
        // mediaType=0, format=14; ASCII from offsets around 21, skipping bytes 6 and 7.
        if (!DoReadDiscStructure(m_drive, 0, 0x0E, buf, 256)) return false;
        // Build from offset 21 skipping the two reserved bytes at +6,+7 relative
        // to offset 21 (i.e. absolute indexes 27 and 28).
        std::string raw;
        raw.reserve(16);
        for (int i = 0; i < 16 && (21 + i) < 256; i++) {
            int abs = 21 + i;
            if (abs == 27 || abs == 28) continue;
            BYTE b = buf[abs];
            if (b < 0x20 || b > 0x7E) {
                if (b == 0) raw.push_back('_');
                else break;
            } else {
                raw.push_back(static_cast<char>(b));
            }
        }
        id = CleanAsciiMediaId(reinterpret_cast<const BYTE*>(raw.data()), raw.size());
        return !id.empty();
    }
    case PioneerMediaFamily::DVDPlusR: {
        // mediaType=0, format=0; ASCII from offset 23, length 11.
        if (!DoReadDiscStructure(m_drive, 0, 0x00, buf, 256)) return false;
        id = CleanAsciiMediaId(buf + 23, 11);
        return !id.empty();
    }
    case PioneerMediaFamily::CDR: {
        // READ TOC (0x43) format=4 (ATIP). Per MMC, the ATIP descriptor
        // starts at response offset 4. The Lead-In Start ATIP Time is at
        // bytes 8/9/10 (MIN/SEC/FRAME) and the Lead-Out Start ATIP Time at
        // bytes 12/13/14. All time fields are BCD-encoded.
        BYTE cdb[10] = {};
        cdb[0] = 0x43;
        cdb[2] = 0x04;          // Format = 4 (ATIP)
        cdb[8] = 32;
        BYTE atip[32] = {};
        if (!m_drive.SendSCSI(cdb, 10, atip, sizeof(atip), /*dataIn=*/true))
            return false;
        char tmp[32] = {};
        std::snprintf(tmp, sizeof(tmp), "%02u:%02u:%02u-%02u:%02u:%02u",
            BcdToBin(atip[8]),  BcdToBin(atip[9]),  BcdToBin(atip[10]),
            BcdToBin(atip[12]), BcdToBin(atip[13]), BcdToBin(atip[14]));
        id.assign(tmp);
        return true;
    }
    case PioneerMediaFamily::CDROM:
    case PioneerMediaFamily::BDROM:
    case PioneerMediaFamily::DVDROM:
    case PioneerMediaFamily::DVDRAM:
    case PioneerMediaFamily::Unknown:
    default: {
        // Default DVD-ish handling: mediaType=0, format=0, ASCII from offset
        // 601, up to 16 bytes or carriage return.
        if (!DoReadDiscStructure(m_drive, 0, 0x00, buf, 2048)) return false;
        std::string raw;
        raw.reserve(16);
        for (int i = 0; i < 16 && (601 + i) < 2048; i++) {
            BYTE b = buf[601 + i];
            if (b == '\r' || b == '\n') break;
            if (b == 0) raw.push_back('_');
            else if (b < 0x20 || b > 0x7E) break;
            else raw.push_back(static_cast<char>(b));
        }
        id = CleanAsciiMediaId(reinterpret_cast<const BYTE*>(raw.data()), raw.size());
        return !id.empty();
    }
    }
}

bool PioneerVendor::GetMediaId(std::string& id) {
    PioneerMediaInfo info;
    if (!GetMediaInfo(info)) return false;
    return GetMediaId(info, id);
}

bool PioneerVendor::IsWriteProtected(bool& writeProtected) {
    writeProtected = false;
    if (!IsPioneerDrive()) return false;

    PioneerMediaInfo info;
    if (!GetMediaInfo(info)) return false;

    // BD-like media (high nibble 0x90 or 0xA0) -> mediaType=1; else 0.
    BYTE hi = static_cast<BYTE>(info.lowByte & 0xF0);
    BYTE mediaType = (hi == 0x90 || hi == 0xA0) ? 1 : 0;

    BYTE buf[8] = {};
    if (!DoReadDiscStructure(m_drive, mediaType, 0xC0, buf, sizeof(buf)))
        return false;

    writeProtected = ((buf[4] & 0x0F) != 0);
    return true;
}

// ── Status ──────────────────────────────────────────────────────────────────
int PioneerVendor::GetDriveStatusCode() {
    const auto& caps = Capabilities();
    if (!caps.driveStatusSupport) return -1;
    return caps.driveStatus;
}

bool PioneerVendor::GetDiscStatus(int& status, std::string& description) {
    status = -1;
    description.clear();
    if (!Capabilities().discStatusSupport) return false;

    // READ DISC INFORMATION (0x51), 34-byte response is sufficient for byte 2.
    BYTE cdb[10] = {};
    cdb[0] = 0x51;
    cdb[8] = 32;
    BYTE buf[32] = {};
    if (!m_drive.SendSCSI(cdb, 10, buf, sizeof(buf), true)) return false;

    int s = buf[2] & 0x03;
    status = s;
    switch (s) {
    case 0: description = "Blank Disc"; break;
    case 1: description = "Writable Media"; break;
    case 2: description = "Finalized Disc or writable depending on format"; break;
    default: description = "Cannot Write"; break;
    }
    return true;
}

// ── Audio preset ────────────────────────────────────────────────────────────
bool PioneerVendor::ApplyAudioExtractionPreset(bool persist, PureReadMode pureReadMode) {
    if (!IsPioneerDrive()) return false;
    const auto& caps = Capabilities();
    if (!caps.valid) return false;

    bool any = false;

    if (caps.pureReadSupport) {
        // Master mode interpolates after retry exhaustion — best for music CDs
        // where a silent dropout is worse than a near-perfect interpolation.
        // Perfect mode instead reports a read error and never fabricates the
        // missing samples — strictest, for callers that want unrecoverable
        // sectors surfaced rather than masked.
        if (SetPureReadMode(pureReadMode,
            caps.realTimePureReadSupport, persist)) {
            any = true;
        }
    }

    if (caps.advancedQuietSupport) {
        // Quiet mode reduces spin-up surges and head excursions, which helps
        // marginal discs read cleanly.
        if (SetSpeedMode(PioneerSpeedMode::Quiet, persist)) {
            any = true;
        }
    }

    if (caps.fragileCdSupport) {
        // Fragile / Rental CD mode slows rotation on audio CDs.
        if (SetFragileCdMode(true, persist)) {
            any = true;
        }
    }

    return any;
}

// Inverse of ApplyAudioExtractionPreset. A rip enables Quiet speed + Fragile/
// Rental-CD slow-rotation (and PureRead) for read quality; those read-optimized
// modes are session-only but survive a media swap, so a burn started on the same
// drive right after a rip inherits them and the firmware refuses to stream the
// write (BDR-S13U: COMMAND SEQUENCE ERROR, KEY=05 ASC=2C ASCQ=00). Restoring the
// default spin/speed state here lets the raw SAO write proceed normally.
bool PioneerVendor::ClearAudioExtractionPreset() {
    if (!IsPioneerDrive()) return false;
    const auto& caps = Capabilities();
    if (!caps.valid) return false;

    bool any = false;

    if (caps.fragileCdSupport) {
        if (SetFragileCdMode(false, /*eepSave=*/false)) any = true;
    }

    if (caps.advancedQuietSupport) {
        if (SetSpeedMode(PioneerSpeedMode::Default, /*eepSave=*/false)) any = true;
    }

    if (caps.pureReadSupport) {
        if (SetPureReadMode(PureReadMode::Off, /*realTimeEnabled=*/false,
            /*eepSave=*/false)) any = true;
    }

    return any;
}

// ── Pretty-print ────────────────────────────────────────────────────────────
void PioneerVendor::PrintCapabilitiesReport() {
    if (!IsPioneerDrive()) return;
    const auto& caps = Capabilities();
    if (!caps.valid) {
        std::cout << "\n--- Pioneer Vendor Features ---\n";
        std::cout << "  (capability block 0xF4 unavailable)\n";
        return;
    }

    auto yn = [](bool v) -> const char* { return v ? "YES" : "NO"; };

    std::cout << "\n--- Pioneer Vendor Features ---\n";
    std::cout << "  Supported drive flag:  " << yn(caps.isSupportedDrive) << "\n";

    uint32_t hwHex = 0; std::string hwStr;
    if (GetHardwareVersion(hwHex, hwStr) && !hwStr.empty()) {
        std::cout << "  Hardware Version:      " << hwStr
            << " (0x" << std::hex << hwHex << std::dec << ")\n";
    }
    std::string serial;
    if (GetSerialNumber(serial) && !serial.empty()) {
        std::cout << "  Drive Serial:          " << serial << "\n";
    }
    std::cout << "  Drive Type Code:       " << static_cast<int>(caps.driveTypeCode) << "\n";

    std::cout << "  PureRead:              " << yn(caps.pureReadSupport);
    if (caps.pureReadSupport) {
        const char* ver = "PureRead";
        switch (caps.pureReadVersion) {
        case 2: ver = "PureRead2"; break;
        case 3: ver = "PureRead3+"; break;
        case 4: ver = "PureRead4+"; break;
        default: break;
        }
        PureReadMode m = PureReadMode::Off; bool rt = false;
        GetPureReadMode(m, rt);
        const char* mname = m == PureReadMode::Off ? "Off"
            : m == PureReadMode::Master ? "Master" : "Perfect";
        std::cout << "  (" << ver << ", mode=" << mname << ")";
    }
    std::cout << "\n";
    std::cout << "  Real-Time PureRead:    " << yn(caps.realTimePureReadSupport) << "\n";
    if (caps.realTimePureReadSupport)
        std::cout << "  Real-Time PR enabled:  " << yn(caps.realTimePureReadOn) << "\n";
    std::cout << "  Advanced Quiet:        " << yn(caps.advancedQuietSupport);
    if (caps.advancedQuietSupport && caps.advancedQuietCurrent != 0xFF)
        std::cout << "  (current=" << static_cast<int>(caps.advancedQuietCurrent) << ")";
    std::cout << "\n";
    std::cout << "  Peak Power Reducer:    " << yn(caps.peakPowerReducerSupport)
        << "  (on=" << yn(caps.peakPowerReducerOn) << ")\n";
    std::cout << "  LED Off:               " << yn(caps.ledOffSupport)
        << "  (on=" << yn(caps.ledOffOn) << ")\n";
    std::cout << "  Smooth Tray Loading:   " << yn(caps.smoothTraySupport)
        << "  (on=" << yn(caps.smoothTrayOn) << ")\n";
    std::cout << "  BD-R High-Speed Rec:   " << yn(caps.bdrHighSpeedSupport)
        << "  (on=" << yn(caps.bdrHighSpeedOn) << ")\n";
    std::cout << "  High-Speed Data Read:  " << yn(caps.highSpeedDataReadSupport) << "\n";
    std::cout << "  CD Check (BLER/C2):    " << yn(caps.cdCheckSupport) << "\n";
    std::cout << "  Select-Track Check:    " << yn(caps.selectTrackInspectionSupport) << "\n";
    std::cout << "  Disc Status report:    " << yn(caps.discStatusSupport) << "\n";
    std::cout << "  Drive Status report:   " << yn(caps.driveStatusSupport);
    if (caps.driveStatusSupport)
        std::cout << "  (code=" << static_cast<int>(caps.driveStatus)
            << (caps.driveStatus == 1 ? ", speed-limited" : "") << ")";
    std::cout << "\n";
    std::cout << "  USB Bus-Power Check:   " << yn(caps.usbBusPowerSupport) << "\n";
    std::cout << "  Force Eject:           " << yn(caps.forceEjectSupport) << "\n";
    std::cout << "  Custom Eco:            " << yn(caps.customEcoSupport) << "\n";
    std::cout << "  Fragile CD Mode:       " << yn(caps.fragileCdSupport)
        << "  (on=" << yn(caps.fragileCdOn) << ")\n";
    std::cout << "  Recording-Mode Select: " << yn(caps.recordingModeSupport) << "\n";

    if (caps.discStatusSupport) {
        int s = -1; std::string desc;
        if (GetDiscStatus(s, desc) && s >= 0) {
            std::cout << "  Current Disc Status:   " << desc << "\n";
        }
    }

    // Media code / media ID / write-protect for the currently inserted disc.
    PioneerMediaInfo media;
    if (GetMediaInfo(media) && media.valid) {
        std::cout << "  Inserted Media:        " << media.discTypeLabel
            << "  (code=0x" << std::hex << std::setw(4) << std::setfill('0')
            << media.mediaCode << std::dec << std::setfill(' ') << ")\n";
        std::string mid;
        if (GetMediaId(media, mid) && !mid.empty()) {
            std::cout << "  Media ID:              " << mid << "\n";
        }
        bool wp = false;
        if (IsWriteProtected(wp)) {
            std::cout << "  Write Protected:       " << (wp ? "YES" : "NO") << "\n";
        }
    }
}
