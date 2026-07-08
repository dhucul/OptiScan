// ============================================================================
// PioneerVendor.h - Pioneer optical-drive vendor feature interface
//
// Wraps the Pioneer vendor capability block (READ BUFFER mode=2 id=0xF4) and
// the WRITE BUFFER 0xFA command family that the Pioneer BD Drive Utility
// exposes. Audio-extraction-relevant features (PureRead, Fragile/Rental CD
// mode, Quiet/Performance mode, CD Check) are first-class methods; less
// common toggles (LED Off, Smooth Tray, BD-R high speed, Custom Eco) are
// exposed for completeness.
//
// All feature methods short-circuit to false when the relevant support bit
// in the capability block is unset, so callers can call unconditionally on
// any Pioneer drive.
// ============================================================================
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

class ScsiDrive;

// Pioneer vendor buffer IDs (used with READ BUFFER 0x3C / WRITE BUFFER 0x3B).
namespace PioneerBufId {
    constexpr BYTE McDirect       = 0xE0;  // MC direct command/result
    constexpr BYTE McDirectAlt    = 0xE1;  // MC direct command variant (media code)
    constexpr BYTE Utility        = 0xE6;  // Bus power, force eject, CD-check measurement
    constexpr BYTE HardwareVersion = 0xF1; // Hardware version block
    constexpr BYTE Capabilities   = 0xF4;  // Main feature/status/capability block (256 B)
    constexpr BYTE RealTimePureRead = 0xF5; // Real-Time PureRead status (32 B)
    constexpr BYTE FeatureSet     = 0xFA;  // Main feature-setting command block (256 B)
}

// 0xFA command IDs (byte 0 of payload).
namespace PioneerCmdId {
    constexpr BYTE PureRead        = 1;
    constexpr BYTE HighSpeedRead   = 8;
    constexpr BYTE RecordingMode   = 9;
    constexpr BYTE PeakPowerReducer = 10;
    constexpr BYTE TraySmooth      = 11;
    constexpr BYTE LedOff          = 12;
    constexpr BYTE BdrHighSpeed    = 13;
    constexpr BYTE LedMode         = 14;
    constexpr BYTE CustomEcoSave   = 17;
    constexpr BYTE RentalCdMode    = 18;
}

enum class PureReadMode {
    Off,
    Master,    // Interpolate after retries — best for audio extraction
    Perfect    // Return error after retries — strictest
};

enum class PioneerSpeedMode {
    Default     = 0,
    Performance = 1,  // Performance priority
    Quiet       = 2,  // Quietness priority
    Persistent  = 3   // Stay in low-speed spin
};

enum class PioneerRecordingMode {
    Off                       = 0,
    DiscRecordSurfaceQuality  = 1,
    WriteQuality              = 2,
    OverSpeedProtection       = 3,
    DiscRecordSurfaceQualityHS = 4
};

// Parsed view of the 256-byte 0xF4 capability block.
struct PioneerCapabilities {
    bool valid = false;
    bool isSupportedDrive = false;     // byte 43 == 1 (the master support flag)

    // Quiet / speed
    bool advancedQuietSupport = false; // byte 45
    BYTE advancedQuietCurrent = 0xFF;  // byte 2  (0xFF = unsupported)
    BYTE quietFallback = 0;            // byte 3

    // PureRead
    bool pureReadSupport = false;      // byte 9
    int  pureReadVersion = 0;          // byte 49 (1..4)
    bool realTimePureReadOn = false;   // byte 28
    bool realTimePureReadSupport = false; // byte 29

    // Recording
    bool recordingModeSupport = false; // byte 10
    BYTE bdRecordingMode = 0;          // byte 11
    BYTE dvdRecordingMode = 0;         // byte 13

    // Misc toggles
    bool peakPowerReducerSupport = false;  // byte 16
    bool peakPowerReducerOn = false;       // byte 17
    bool smoothTraySupport = false;        // byte 20 != 0xFF
    bool smoothTrayOn = false;             // byte 20 == 1
    bool driveStatusSupport = false;       // byte 22 != 0xFF
    BYTE driveStatus = 0;                  // byte 23 (1 = speed limited)
    bool ledOffSupport = false;            // byte 24 != 0xFF
    bool ledOffOn = false;                 // byte 24 == 1
    bool bdrHighSpeedSupport = false;      // byte 26 != 0xFF
    bool bdrHighSpeedOn = false;           // byte 26 == 1
    bool highSpeedDataReadSupport = false; // byte 41 != 0xFF
    bool cdCheckSupport = false;           // byte 44
    bool discStatusSupport = false;        // byte 46
    bool usbBusPowerSupport = false;       // byte 47
    bool forceEjectSupport = false;        // byte 48
    bool customEcoSupport = false;         // byte 50
    bool selectTrackInspectionSupport = false; // byte 52
    BYTE driveTypeCode = 0;                // byte 53
    bool switchCdRomSpeedTableSupport = false; // byte 54
    bool fragileCdSupport = false;         // byte 55 != 0
    bool fragileCdOn = false;              // byte 56 != 0

    // Raw bytes for debug / future use
    BYTE raw[256] = {};
};

struct PioneerRtPureReadStatus {
    bool valid = false;
    uint32_t errorSectors = 0;   // r[0..3]
    uint32_t playSectors = 0;    // r[4..7]
    uint32_t currentLBA = 0;     // r[8..11]
};

// Media families used by Pioneer utility for ID extraction.
enum class PioneerMediaFamily {
    CDROM = 0,
    DVDROM = 1,
    DVDDashR = 2,    // DVD-R / DVD-RW
    DVDPlusR = 3,    // DVD+R / DVD+RW
    DVDRAM = 4,
    BDR = 5,         // BD-R / BD-RE
    CDR = 6,         // CD-R / CD-RW
    BDROM = 7,
    Unknown = 255
};

struct PioneerMediaInfo {
    bool valid = false;
    uint16_t mediaCode = 0;          // (r[10] << 8) | r[6]
    BYTE lowByte = 0;                // mediaCode & 0xFF — selects disc-type label
    std::string discTypeLabel;       // e.g. "CD-R", "BD-R Dual"
    PioneerMediaFamily family = PioneerMediaFamily::Unknown;
};

struct PioneerCdCheckResult {
    bool valid = false;
    uint16_t c1Uncorrectable = 0;   // r[4..5]
    uint16_t c2Uncorrectable = 0;   // r[14..15]
    uint32_t endAddress = 0;        // r[18..21]
    bool dataValid = true;          // r[22..25] != 0xFFFFFFFF
    uint16_t tePeak = 0;            // r[60..61]
    uint16_t teIntegrationMax = 0;  // r[62..63]
    bool teDataValid = false;       // TE fields are valid only when neither is 0xFFFF
};

class PioneerVendor {
public:
    explicit PioneerVendor(ScsiDrive& drive) : m_drive(drive) {}

    // INQUIRY-string vendor check. Cheap; safe to call before any other op.
    bool IsPioneerDrive();

    // Reads & caches the 256-byte 0xF4 capability block. Returns false if
    // the drive is not Pioneer or the read fails.
    bool ReadCapabilities(PioneerCapabilities& caps);

    // Returns cached capabilities or reads them on demand.
    const PioneerCapabilities& Capabilities();

    // ── Identification ─────────────────────────────────────────────
    // Reads ASCII hex hardware version from buffer 0xF1 offset 20.
    bool GetHardwareVersion(uint32_t& versionHex, std::string& versionStr);
    // GET CONFIGURATION 0x46 with feature 0x0108 (Logical Unit Serial Number).
    bool GetSerialNumber(std::string& serial);

    // ── PureRead ───────────────────────────────────────────────────
    bool GetPureReadMode(PureReadMode& mode, bool& realTimeEnabled);
    bool SetPureReadMode(PureReadMode mode, bool realTimeEnabled = false,
        bool eepSave = true);
    bool GetRealTimePureReadStatus(PioneerRtPureReadStatus& status);
    bool ClearRealTimePureReadStatus();

    // ── Quiet / Performance / SET CD SPEED vendor byte 10 ──────────
    // Writes byte 10 of SET CD SPEED with 0x80|mode (or 0xC0|mode if eepSave).
    bool SetSpeedMode(PioneerSpeedMode mode, bool eepSave = false,
        int readMultiplier = -1, int writeMultiplier = -1);

    // ── Boolean feature toggles (WRITE BUFFER 0xFA) ────────────────
    bool SetPeakPowerReducer(bool on, bool eepSave = true);
    bool SetSmoothTray(bool on, bool eepSave = true);
    bool SetLedOff(bool on, bool eepSave = true);
    bool SetBdrHighSpeedRecording(bool on, bool eepSave = true);
    bool SetHighSpeedDataRead(bool on, bool eepSave = true);

    // Fragile / Rental CD mode: slows rotation when reading audio CDs.
    // Useful for fragile or marginal pressings — improves read reliability
    // at the cost of speed. Internal Pioneer name is "Rental CD Mode".
    // Note: payload semantics from utility are (w[2]=eepSave, w[3]=off-flag).
    bool SetFragileCdMode(bool on, bool eepSave = true);

    // ── Recording mode ─────────────────────────────────────────────
    bool SetRecordingMode(PioneerRecordingMode bdMode,
        PioneerRecordingMode dvdMode, bool eepSave = true);

    // ── Utility commands via buffer 0xE6 ───────────────────────────
    // Force-eject: vendor command at offset 0x20000.
    bool ForceEject();
    // USB bus-power check: write at 0x10000 then read back result.
    bool RunBusPowerCheck(uint32_t& reading);

    // ── CD Check (audio quality measurement) ───────────────────────
    bool CdCheckStart(uint32_t startLBA, uint32_t unitSize);
    bool CdCheckStop();
    bool CdCheckRead(PioneerCdCheckResult& result);
    // Same as CdCheckStart but returns SCSI sense bytes for diagnostics.
    bool CdCheckStartWithSense(uint32_t startLBA, uint32_t unitSize,
        BYTE& senseKey, BYTE& asc, BYTE& ascq);

    // ── Media code / Media ID / write protection ───────────────────
    // Reads the Pioneer vendor media-code via WRITE/READ BUFFER 0xE1 with
    // command 0x91/0x40. Fills disc-type label and media family.
    bool GetMediaInfo(PioneerMediaInfo& info);

    // Extracts the manufacturer media identifier string for the inserted
    // disc, dispatching by media family (READ DISC STRUCTURE / READ TOC
    // format 4 depending on family). Returns false if the family does not
    // expose a usable media ID or the read fails. Zero bytes in the ASCII
    // payload are converted to underscores.
    bool GetMediaId(const PioneerMediaInfo& info, std::string& id);

    // Convenience: read media info then extract its media ID.
    bool GetMediaId(std::string& id);

    // Reads disc write-protection state via READ DISC STRUCTURE format 0xC0.
    // mediaType is selected automatically from the media-code high nibble
    // (BD-like 0x90/0xA0 -> 1, else 0). Sets `writeProtected = true` when
    // (response[4] & 0x0F) != 0.
    bool IsWriteProtected(bool& writeProtected);

    // ── Disc / drive status ────────────────────────────────────────
    // Drive status comes straight from cap block byte 23 (1 = speed limited).
    int GetDriveStatusCode();
    // Disc status from READ DISC INFORMATION (r[2] & 3): 0 blank, 1 writable,
    // 2 finalized/writable, other = cannot write.
    bool GetDiscStatus(int& status, std::string& description);

    // ── Convenience: apply optimal-audio-extraction settings ───────
    // Enables PureRead + Real-Time PureRead, sets Quiet speed mode, turns on
    // Fragile CD mode when supported. The PureRead mode is selectable:
    //   Master  (default) - interpolate unreadable samples (gap-free rip)
    //   Perfect           - report a read error instead of interpolating
    // Saves to EEPROM only if persist=true. Returns true if any setting was
    // applied.
    bool ApplyAudioExtractionPreset(bool persist = false,
        PureReadMode pureReadMode = PureReadMode::Master);

    // Inverse of ApplyAudioExtractionPreset: restore default spin/speed, turn
    // Fragile/Rental CD mode off, and PureRead off. Must run before a burn --
    // the slow-spin Quiet/Fragile read modes a rip leaves behind prevent the
    // drive from streaming a write (the BDR-S13U aborts mid-burn with COMMAND
    // SEQUENCE ERROR). Session-only (eepSave=false). No-op on non-Pioneer drives.
    bool ClearAudioExtractionPreset();

    // ── Pretty-print all detected Pioneer features ─────────────────
    void PrintCapabilitiesReport();

private:
    ScsiDrive& m_drive;
    PioneerCapabilities m_caps;
    bool m_capsRead = false;
    int m_isPioneerCached = -1;  // -1 unknown, 0 no, 1 yes

    // Common helpers
    bool ReadBuffer(BYTE bufId, uint32_t offset, BYTE* dst, DWORD length, BYTE mode = 2);
    bool WriteBuffer(BYTE bufId, uint32_t offset, const BYTE* src, DWORD length, BYTE mode = 1);
    bool WriteFeatureCommand(BYTE cmdId, BYTE arg2, BYTE arg3 = 0, BYTE arg4 = 0,
        BYTE arg5 = 0, BYTE arg6 = 0);
};

// ── RAII: temporarily disable Pioneer PureRead for a measurement scan ───────
// PureRead interpolates after retries on audio reads, which would mask the
// raw C1/C2 error counts a quality scan is trying to measure. The guard
// snapshots the current PureRead mode AND the Real-Time PureRead flag at
// construction, forces both Off, and restores the full snapshot on
// destruction. Real-Time PureRead is a separate error-hiding flag, so it must
// be turned off and put back too — otherwise a disc left with mode=Off but
// Real-Time PureRead=On would scan unguarded, and any scan would silently drop
// the user's real-time setting. Volatile (eepSave=false) — changes do not
// persist across power-cycles and never touch the drive's saved setting. Pass
// `active=false` (e.g. on non-Pioneer drives) to no-op the whole thing.
class PioneerPureReadOffGuard {
public:
    explicit PioneerPureReadOffGuard(ScsiDrive& drive, bool active);
    ~PioneerPureReadOffGuard();

    PioneerPureReadOffGuard(const PioneerPureReadOffGuard&) = delete;
    PioneerPureReadOffGuard& operator=(const PioneerPureReadOffGuard&) = delete;

private:
    PioneerVendor m_pioneer;
    bool m_active = false;
    bool m_restore = false;
    PureReadMode m_previousMode = PureReadMode::Off;
    bool m_previousRealTime = false;
};

// ── RAII: force Pioneer drive into Performance speed mode for a scan ────────
// Pioneer measurement scans (CD BLER / C1 quality / rot detection) have
// firmware-set internal scan rates, but bumping the drive's advanced-quiet
// speed mode to Performance gives the firmware permission to run at the
// fastest speed it supports, which can shorten a full-disc scan noticeably
// on drives that honor the setting. The guard snapshots the current speed
// mode at construction, sets it to Performance, and restores the snapshot
// on destruction. Volatile (eepSave=false) — change does not persist across
// power-cycles. Pass `active=false` (e.g. on non-Pioneer drives) to no-op.
class PioneerPerformanceModeGuard {
public:
    explicit PioneerPerformanceModeGuard(ScsiDrive& drive, bool active);
    ~PioneerPerformanceModeGuard();

    PioneerPerformanceModeGuard(const PioneerPerformanceModeGuard&) = delete;
    PioneerPerformanceModeGuard& operator=(const PioneerPerformanceModeGuard&) = delete;

private:
    PioneerVendor m_pioneer;
    bool m_active = false;
    bool m_restore = false;
    PioneerSpeedMode m_previousMode = PioneerSpeedMode::Default;
};
