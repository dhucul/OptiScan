// ============================================================================
// ScsiDrive.h - Low-level SCSI drive communication
// ============================================================================
#pragma once

#include "ScsiTypes.h"
#include "DriveTypes.h"
#include "Constants.h"
#include <windows.h>
#include <winioctl.h>     // DEVICE_TYPE — required by ntddstor.h
#include <ntddcdrm.h>
#include <ntddscsi.h>
#include <vector>
#include <string>

class ScsiDrive {
private:
	HANDLE m_handle = INVALID_HANDLE_VALUE;
	wchar_t m_driveLetter = 0;   // remembered on Open() so Reopen() can re-target
	WORD m_currentSpeed = CD_SPEED_MAX;
	C2Mode m_c2Mode = C2Mode::NotSupported;
	bool m_c1BlockErrorsAvailable = false;     // True if bytes 294-295 contain valid C1 data
	int m_maxRetries = 5;
	int m_retryDelayMs = 100;
	bool m_c2Functional = true;        // C2 pointer data is actually populated

	// Current Pioneer SET CD SPEED byte-10 mode (bits 0-5 of speed-mode value).
	// Sticky for the lifetime of the open handle so that SetSpeed/Quiet/Perf
	// mode choices made via SetCdSpeedPioneer survive subsequent plain
	// SetSpeed calls from the rip workflow. Zero = default/no mode override.
	BYTE m_pioneerSpeedMode = 0;

	// Main-channel selection byte (READ CD CDB[9]) used for CD-DA reads.
	// Starts at 0xF8 (sync + headers + user data + EDC/ECC). Some drives —
	// notably Hitachi-LG (HL-DT-ST) — reject that combination for a CD-DA
	// typed read and return CHECK CONDITION instantly without touching the
	// disc, producing an all-zero rip. When 0xF8 is refused we fall back to
	// 0x10 (user-data-only — the canonical CD-DA read) and cache it for the
	// rest of the session so every later read uses the accepted form. The
	// audio bytes are identical between the two forms for CD-DA, so the
	// switch is transparent to offset/CRC/AccurateRip. Reset on Open().
	BYTE m_cddaMainChannelFlags = 0xF8;

	// True once a CD-DA read has succeeded, fixing m_cddaMainChannelFlags to a
	// form the drive accepts. After that a read failure is a genuine medium
	// error (bad sector), so we must NOT waste a second attempt with the
	// alternate form — the 0xF8->0x10 probe only runs until the form is known.
	bool m_cddaReadFormProbed = false;

	// Expected Sector Type used in the READ CD CDB[1] for CD-DA reads. Starts
	// at 0x04 (CD-DA). Some drives — notably modern Pioneer BD burners (e.g.
	// BDR-S13U) — return CHECK CONDITION for a CD-DA-typed READ CD and only
	// accept expected-sector-type 0 (any). When the 0x04 forms are refused the
	// fallback ladder switches to 0x00 and caches it for the session. For an
	// audio track the returned 2352 bytes are identical. Reset on Open().
	BYTE m_cddaSectorType = 0x04;

	// True once any READ CD form has returned GOOD this session, fixing the
	// accepted (m_cddaSectorType, m_cddaMainChannelFlags) pair. Gates the lazy
	// EnsureCddaReadForm() probe so subchannel/pregap/C2 reads can adopt the
	// same form ReadCdAudio discovers, without re-probing every sector. Distinct
	// from m_cddaReadFormProbed, which additionally resolves the HL-DT-ST
	// GOOD-but-zero ambiguity for the audio main channel. Reset on Open().
	bool m_cddaFormDiscovered = false;

	// Sense (key/ASC/ASCQ) of the most recent failed CD-DA read. Lets the
	// copy workflow report *why* a drive rejected extraction instead of
	// guessing. Populated by ReadCdAudio; reset on Open().
	BYTE m_lastReadSenseKey = 0;
	BYTE m_lastReadASC = 0;
	BYTE m_lastReadASCQ = 0;

	// ── Cached capability probe results ─────────────────────
	int m_qcheckProbed = -1;           // -1 = not probed, 0 = unsupported, 1 = supported
	int m_liteonScanProbed = -1;       // -1 = not probed, 0 = unsupported, 1 = supported
	int m_liteonJitterProbed = -1;     // -1 = not probed, 0 = unsupported, 1 = supported
	int m_liteonFeTeProbed = -1;       // -1 = not probed, 0 = unsupported, 1 = supported
	int m_pioneerScanProbed = -1;      // -1 = not probed, 0 = unsupported, 1 = supported

	// Lowest read-speed multiplier the drive will actually honor, discovered by
	// GetLowestHonoredSpeed(): request 1x, read back what MODE SENSE 0x2A reports
	// the drive clamped to. -1 = not yet probed. Cached for the open handle
	// since the floor is a fixed drive/media property; reset on Open().
	int m_lowestHonoredSpeed = -1;

public:
	// ── Type aliases for backward compatibility ──────────────
	using C2ReadOptions = ::C2ReadOptions;  // Re-export from ScsiTypes.h

	ScsiDrive() = default;
	~ScsiDrive() { Close(); }

	// Non-copyable
	ScsiDrive(const ScsiDrive&) = delete;
	ScsiDrive& operator=(const ScsiDrive&) = delete;

	// ── Core operations ──────────────────────────────────────
	bool Open(wchar_t driveLetter);
	void Close();
	// Close and re-Open the same drive letter. Drops the OS handle and all
	// per-handle driver state carried over from the prior (read) session, so the
	// next operation runs on a clean handle. Returns false (handle left closed)
	// if the re-Open fails.
	bool Reopen();
	bool IsOpen() const { return m_handle != INVALID_HANDLE_VALUE; }

	// ── Speed control ────────────────────────────────────────
	void SetSpeed(int multiplier, int writeMultiplier = -1);

	// Non-breaking helper: attempts set + best-effort verify using MODE SENSE 2Ah.
	// Returns true when command was accepted and (for explicit speed requests) verification matched.
	bool TrySetSpeedAndVerify(int multiplier, int writeMultiplier = -1,
		WORD* outActualRead = nullptr, WORD* outActualWrite = nullptr);

	WORD GetCurrentSpeed() const { return m_currentSpeed; }
	bool GetActualSpeed(WORD& readSpeed, WORD& writeSpeed);

	// Discovers the lowest read speed the drive will actually honor. Requests 1x
	// and reads back what the drive clamped to via MODE SENSE 0x2A; many drives
	// refuse 1x for CD-DA and silently run at 4x/8x/10x, so the clamped value is
	// the true floor. Returns a multiplier (>=1); falls back to 1 if the speed
	// can't be read back. Result is cached for the open handle. Does not leave
	// the drive at this speed unless `apply` is true.
	int GetLowestHonoredSpeed(bool apply = false);

	// ── Sector reading ───────────────────────────────────────
	bool ReadSector(DWORD lba, BYTE* audio, BYTE* subchannel);
	bool ReadSectorWithC2(DWORD lba, BYTE* audio, BYTE* subchannel, int& c2Errors);
	bool ReadSectorAudioOnly(DWORD lba, BYTE* audio);
	bool ReadSectorsAudioOnly(DWORD startLBA, DWORD count, BYTE* audio);
	bool ReadDataSector(DWORD lba, BYTE* data);
	bool ReadSectorQ(DWORD lba, int& qTrack, int& qIndex);
	bool ReadSectorQSingle(DWORD lba, int& qTrack, int& qIndex);
	bool ReadSectorQAdaptive(DWORD lba, int& qTrack, int& qIndex,
		DWORD pregapLBA, DWORD startLBA);
	bool ReadSectorQAnyType(DWORD lba, int& qTrack, int& qIndex);

	// ── Enhanced C2 reading ──────────────────────────────────
	bool ReadSectorWithC2Ex(DWORD lba, BYTE* audio, BYTE* subchannel, int& c2Errors,
		BYTE* c2Raw, const C2ReadOptions& options,
		BYTE* outSenseKey = nullptr, BYTE* outASC = nullptr, BYTE* outASCQ = nullptr,
		int* outC1BlockErrors = nullptr, int* outC2BlockErrors = nullptr);
	bool ReadSectorWithC2ExMultiPass(DWORD lba, BYTE* audio, BYTE* subchannel,
		int& c2Errors, BYTE* c2Raw, const C2ReadOptions& options,
		BYTE* outSenseKey = nullptr, BYTE* outASC = nullptr, BYTE* outASCQ = nullptr);
	bool PlextorReadC2(DWORD lba, BYTE* audio, int& c2Errors, BYTE* c2Raw, bool countBytes,
		BYTE* outSenseKey = nullptr, BYTE* outASC = nullptr, BYTE* outASCQ = nullptr,
		int* outC1BlockErrors = nullptr, int* outC2BlockErrors = nullptr);
	bool ValidateC2Accuracy(DWORD testLBA);
	bool IsPlextor();
	bool IsPioneer();
	bool SupportsC1BlockErrors() const;

	// ── Plextor Q-Check hardware quality scan ────────────────
	// Uses vendor commands 0xE9 (start scan) and 0xEB (poll results) to
	// perform the same C1/C2/CU measurement that QPXTool uses.  The drive
	// scans at ~1x internally; no audio data is transferred.
	bool PlextorQCheckStart(DWORD startLBA, DWORD endLBA);
	bool PlextorQCheckPoll(int& c1, int& c2, int& cu, DWORD& currentLBA, bool& scanDone);
	bool PlextorQCheckStop();
	bool SupportsQCheck();

	// ── LiteOn/MediaTek quality scan (0xDF vendor command) ───
	// Used by QPXTool for MediaTek-based LiteOn, ASUS, and Plextor OEM
	// drives.  Similar to Q-Check but uses a different command set.
	bool SupportsLiteOnScan();
	bool LiteOnScanStart(DWORD startLBA, DWORD endLBA);
	bool LiteOnScanPoll(int& c1, int& c2, int& cu, DWORD& currentLBA, bool& scanDone);
	bool LiteOnScanStop();

	// ── LiteOn/MediaTek jitter & beta scan (0xDF/0x1B vendor command) ─
	// Physical-layer measurement: jitter (EFM timing variation) and beta
	// (pit/land asymmetry).  Complements the C1/C2/CU LiteOn scan above.
	bool SupportsLiteOnJitter();
	bool LiteOnJitterStart(DWORD startLBA, DWORD endLBA);
	bool LiteOnJitterPoll(int& jitter, int& beta, DWORD& currentLBA, bool& scanDone);
	bool LiteOnJitterStop();

	// ── LiteOn/MediaTek focus/tracking-error scan (0xDF/0x08 vendor command) ─
	// Servo-level measurement of focus error (FE) and tracking error (TE)
	// amplitude. Uses DF 08 02 <type> <lba> to init and DF 08 01 to read a
	// slice, with DF 02 09 for position. CDB[4..7] must carry the LBA — the
	// drive returns sk=5 asc=24 (INVALID FIELD IN CDB) when it is left zero.
	bool SupportsLiteOnFeTe();
	bool LiteOnFeTeStart(DWORD startLBA, DWORD endLBA);
	bool LiteOnFeTePoll(int& fe, int& te, DWORD& currentLBA, bool& scanDone);
	bool LiteOnFeTeStop();

	// ── Pioneer quality scan (0x3B/0x3C vendor commands) ─────
	// Uses Pioneer's two-phase WRITE/READ BUFFER protocol to perform
	// hardware-driven BLER/E22 error scanning.  The drive scans internally
	// and reports C1 (BLER) and C2 (E22) error counts per time slice.
	//   Send scan request: CDB 3B 02 E1 (WRITE BUFFER)
	//   Read scan results: CDB 3C 02 E1 (READ BUFFER)
	bool SupportsPioneerScan();
	bool PioneerScanStart(DWORD startLBA, DWORD endLBA);
	bool PioneerScanPoll(int& c1, int& c2, int& cu, DWORD& currentLBA, bool& scanDone);
	bool PioneerScanStop();

	// ── Drive capabilities ───────────────────────────────────────
	bool CheckC2Support();
	bool GetDriveInfo(std::string& vendor, std::string& model);
	bool DetectCapabilities(DriveCapabilities& caps);
	bool GetModePage2A(std::vector<BYTE>& pageData);
	bool TestOverread(bool leadIn);

	// ── Chipset / controller identification ──────────────────────
	bool DetectChipset(ChipsetInfo& info);

	// ── Drive offset detection ───────────────────────────────
	bool DetectDriveOffset(OffsetDetectionResult& result);
	bool LookupAccurateRipOffset(DriveOffsetInfo& info);
	bool DetectOffsetFromPregap(int trackStartLBA, int& estimatedOffset);

	// ── Plextor "Hide CDR Media" (vendor mode page 0x31) ─────
	// When enabled, a Plextor drive reports inserted CD-R / CD-RW media
	// as pressed CD on subsequent disc-info queries. Useful for working
	// around audio copy-protection schemes whose behaviour changes when
	// the inserted disc is detected as CD-R, and for re-ripping CD-R
	// backups of protected pressings.
	bool SupportsHideCDRMedia();
	bool SetHideCDRMedia(bool enable);

	// ── Plextor PoweRec (0xED) ───────────────────────────────
	// Optimal write-power calibration. Smarter than generic SEND OPC (0x54)
	// on supported Plextor writers; query state or toggle on/off.
	bool GetPoweRec(bool& enabled);
	bool SetPoweRec(bool enable);

	// ── Plextor SpeedRead (0xE9 / 0xBB) ──────────────────────
	// Lifts the RPC-derived read-speed cap on CD-ROM media so the drive
	// runs at its true physical maximum. Apply once at session start.
	bool SetSpeedRead(bool enable);

	// ── Plextor SilentMode (0xE9 / 0xD8) ─────────────────────
	// Reduces drive noise by capping spin-up speed and softening seeks.
	// Useful for long unattended secure / paranoid rip sessions.
	bool GetSilentMode(bool& enabled);
	bool SetSilentMode(bool enable);

	// ── Plextor TLA / hardware revision (0xF1 mode 9) ────────
	// Board-revision string stored in EEPROM (e.g. "0301"). Disambiguates
	// firmware-identical drives with different hardware silicon.
	bool GetPlextorTLA(std::string& tla);

	// ── Plextor TestWrite (0xE9 / 0xBE) ──────────────────────
	// When enabled, the drive simulates writes (laser stays at read power)
	// so the next WriteDisc pass dry-runs the full pipeline without burning
	// anything. Always pair an enable with a disable after the write.
	bool SetPlextorTestWrite(bool enable);

	// ── Plextor VariRec for CD (0xE9 / 0x02) ─────────────────
	// Variable write-strategy: lets the host nudge laser power off the
	// firmware default to tune burn quality on stubborn CD-R media.
	// powerOffset is signed (-4..+4 on most generations); 0 = factory.
	bool GetVariRecCD(bool& enabled, int& powerOffset);
	bool SetVariRecCD(bool enable, int powerOffset);

	// ── Media control ────────────────────────────────────────
	bool Eject();
	bool SpinDown();
	bool GetMediaProfile(WORD& profileCode, std::string& profileName);
	bool RequestSenseProgress(BYTE& senseKey, BYTE& asc, BYTE& ascq, int& progressPercent);

	// ── MMC disc structure queries (no sector I/O) ───────────
	bool ReadDiscCapacity(DWORD& lastLBA, int& sessions, int& lastTrack);
	bool ReadTrackInfo(int trackNumber, DWORD& startLBA, DWORD& trackLength,
		bool& isAudio, int& session, int& mode);

	// ── Raw SCSI access ──────────────────────────────────────
	bool SendSCSI(void* cdb, BYTE cdbLength, void* buffer, DWORD bufferSize,
		bool dataIn = true, DWORD timeoutSec = 60);
	bool SendSCSIWithSense(void* cdb, BYTE cdbLength, void* buffer, DWORD bufferSize,
		BYTE* senseKey, BYTE* asc, BYTE* ascq, bool dataIn = true,
		DWORD timeoutSec = 60);
	bool SeekToLBA(DWORD lba);

	// ── Enhanced error handling ──────────────────────────────
	bool GetMediaStatus(DriveHealthCheck& status);
	bool TestUnitReady();
	bool WaitForDriveReady(int timeoutSeconds = 30);
	std::string GetSenseDescription(BYTE senseKey, BYTE asc, BYTE ascq);

	// Sense of the most recent failed CD-DA read (see m_lastReadSenseKey).
	// Returns false if no read failure has been recorded this session.
	bool GetLastReadSense(BYTE& senseKey, BYTE& asc, BYTE& ascq) const {
		senseKey = m_lastReadSenseKey;
		asc = m_lastReadASC;
		ascq = m_lastReadASCQ;
		return m_lastReadSenseKey != 0 || m_lastReadASC != 0 || m_lastReadASCQ != 0;
	}

	// ── Retry configuration ──────────────────────────────────
	void SetMaxRetries(int retries) { m_maxRetries = retries; }
	void SetRetryDelay(int delayMs) { m_retryDelayMs = delayMs; }

	// Get drive-specific configuration if available
	bool GetDriveSpecificConfig(DriveSpecificConfig& config) const;

	// Classify the drive against knownDriveConfigs / knownDriveFamilies.
	// Curated takes precedence over a family match. `outFamilyName` is set
	// when the result is KnownGoodFamily, left untouched otherwise.
	DriveTier GetDriveTier(std::string* outFamilyName = nullptr) const;

	// Apply drive-specific settings for optimal audio extraction

	// Display user-friendly recommendations for this drive
	void DisplayDriveRecommendations() const;

	// Get recommendation text for current drive
	std::string GetDriveRecommendationText() const;

	// Pioneer-specific SET CD SPEED with vendor byte 10 control.
	// speedModeValue: bits 0-5 speed/session mode (0 = default).
	// eepSave: if true, setting persists to EEPROM across power cycles.
	bool SetCdSpeedPioneer(int multiplier, BYTE speedModeValue = 0,
		bool eepSave = false, int writeMultiplier = -1);

private:
	// Issues one READ CD for `count` CD-DA sectors starting at `lba`, with the
	// given subchannel-selection byte (CDB[10]), into `buffer`. Uses the cached
	// CD-DA main-channel form and transparently falls back from 0xF8 to 0x10
	// (caching the accepted form) when the drive rejects 0xF8. Shared by
	// ReadSector, ReadSectorAudioOnly, and ReadSectorsAudioOnly.
	bool ReadCdAudio(DWORD lba, DWORD count, BYTE subSelect, BYTE* buffer, DWORD bufferSize);

	// Reads the audio interval [lba, lba+sectors) purely to move the head so a
	// LiteOn vendor scan accumulates real error/servo counts. Chunked <= 16
	// sectors per read (0xFFFE ATAPI transfer ceiling). Read failures are ignored
	// — a defective sector is itself a scan result the drive counts.
	void LiteOnScanDriveHead(DWORD lba, DWORD sectors);

	// Lazily probe and cache the READ CD form (expected sector type + main
	// channel) the drive accepts for CD-DA, using a throwaway 1-sector read at
	// `lba`. No-op once m_cddaFormDiscovered is set. Lets subchannel, pregap,
	// and C2 reads adopt the same form ReadCdAudio would (e.g. expected sector
	// type 0x00 on Pioneer BD burners) instead of hard-coding 0x04/0xF8.
	void EnsureCddaReadForm(DWORD lba);

	// Issues one Pioneer WRITE BUFFER (0x3B) scan request for [lba, lba+count)
	// followed immediately by a READ BUFFER (0x3C) to retrieve the result —
	// QPxTool's cmd_cd_errc_read + cmd_cd_errc_getdata performed back-to-back.
	// On success fills c1 (BLER) and c2 (E22) with the parsed counters, already
	// passed through the >300 firmware-garbage guard. Shared by the probe and
	// the per-slice poll. Returns false if either transport hard-fails.
	bool PioneerScanReadSlice(DWORD lba, DWORD count, int& c1, int& c2);

	bool ReadSectorQRaw(DWORD lba, int& qTrack, int& qIndex);
	bool ParseRawSubchannel(const BYTE* sub, int& qTrack, int& qIndex);
	bool ProbeC1BlockErrors();
	bool ProbeC2Liveness();
};
