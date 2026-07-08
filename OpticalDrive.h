// ============================================================================
// OpticalDrive.h - Main audio CD copying orchestration
// ============================================================================
#pragma once

#include "DiscTypes.h"
#include "ScsiDrive.h"
#include "Progress.h"
#include "ConsoleColors.h"
#include <functional>
#include <string>

class OpticalDrive {
public:
	OpticalDrive() = default;
	~OpticalDrive() { Close(); }

	// Drive access
	bool Open(wchar_t driveLetter) { return m_drive.Open(driveLetter); }
	void Close() { m_drive.Close(); }

	// Drive identity / validation forwarders. GetDriveInfo is non-const because
	// the underlying ScsiDrive method is.
	bool GetDriveInfo(std::string& vendor, std::string& model) {
		return m_drive.GetDriveInfo(vendor, model);
	}
	bool IsKnownConfig(DriveSpecificConfig& cfg) const {
		return m_drive.GetDriveSpecificConfig(cfg);
	}
	DriveTier GetDriveTier(std::string* outFamilyName = nullptr) const {
		return m_drive.GetDriveTier(outFamilyName);
	}

	// Configuration menus
	int SelectSpeed();
	int SelectSubchannel();
	int SelectErrorHandling();
	LogOutput SelectLogging();
	int SelectC2Detection();
	int SelectScanSpeed();
	int SelectWriteSpeed();
	int SelectOffset();
	int SelectSecureRipMode(int selectedSpeed = 0);
	SecureRipConfig GetSecureRipConfig(SecureRipMode mode);
	int SelectPregapMode();
	int SelectCacheDefeat();
	int SelectHideCDRMedia();
	int SelectSilentMode();
	int SelectPlextorWriteOptions(bool& outTestWrite, bool& outVariRecEnable, int& outVariRecOffset);

	// TOC reading
	bool ReadTOC(DiscInfo& disc, bool skipPregapScan = false);
	bool ReadFullTOC(DiscInfo& disc);
	bool ReadCDText(DiscInfo& disc);
	bool ReadISRC(DiscInfo& disc);
	bool ReadMCN(DiscInfo& disc);   // Media Catalog Number (UPC/EAN) from Q subchannel
	bool DetectHiddenTrack(DiscInfo& disc);
	bool DetectHiddenLastTrack(DiscInfo& disc);

	// TOC-less disc scan — builds track list from Q subchannel when TOC is bad
	bool ScanDiscWithoutTOC(DiscInfo& disc, int scanSpeed = 4);

	// Disc reading
	bool ReadDisc(DiscInfo& disc, int errorMode,
		std::function<void(int, int)> progress = nullptr);
	bool ReadDiscSecure(DiscInfo& disc, const SecureRipConfig& config,
		SecureRipResult& result, std::function<void(int, int)> progress = nullptr);
	bool ReadDiscBurst(DiscInfo& disc, std::function<void(int, int)> progress = nullptr,
		int speedOverride = 0, int errorMode = 2);

	// Drive-independent recovery rip: rebuilds hard sectors from cross-read
	// consensus (per-byte majority voting + jitter alignment) rather than
	// trusting the drive's C2.  See RecoveryTypes.h for the strategy.
	bool ReadDiscRecovery(DiscInfo& disc, const RecoveryRipConfig& config,
		RecoveryRipResult& result, std::function<void(int, int)> progress = nullptr);
	bool SaveRecoveryReport(const RecoveryRipResult& result, const std::wstring& filename);

	// Quality scanning
	bool RunBlerScan(const DiscInfo& disc, BlerResult& result, int scanSpeed = 8);
	bool RunQCheckScan(const DiscInfo& disc, QCheckResult& result, int scanSpeed = 8);
	void PrintQCheckReport(const QCheckResult& result);
	bool SaveQCheckLog(const QCheckResult& result, const std::wstring& filename);
	bool RunJitterScan(const DiscInfo& disc, JitterResult& result, int scanSpeed = 8);
	void PrintJitterReport(const JitterResult& result);
	bool SaveJitterLog(const JitterResult& result, const std::wstring& filename);
	bool RunFeTeScan(const DiscInfo& disc, FeTeResult& result, int scanSpeed = 8);
	void PrintFeTeReport(const FeTeResult& result);
	bool SaveFeTeLog(const FeTeResult& result, const std::wstring& filename);
	bool RunC2Scan(const DiscInfo& disc, BlerResult& result, int scanSpeed = 8);
	void PrintC2ScanReport(const BlerResult& result, const DiscInfo& disc, int scanSpeed);
	void PrintC2Chart(const BlerResult& result, int width = 60, int height = 10);
	void PrintC2SenseCodeChart(const std::vector<C2SectorError>& badSectors, const DiscInfo& disc, const BlerResult& result);

	// Output
	bool SaveToFile(const DiscInfo& disc, const std::wstring& basePath);
	bool SaveBlerLog(const BlerResult& result, const std::wstring& filename);
	void PrintBlerGraph(const BlerResult& result, int width = 60, int height = 10);
	bool SaveReadLog(const DiscInfo& disc, const std::wstring& filename);
	bool SaveSecureRipLog(const SecureRipResult& result, const std::wstring& filename);
	bool GenerateCueSheet(const DiscInfo& disc, const std::wstring& audioFilePath,
		const std::wstring& cueOutputPath);

	// BLER Analysis and Reporting
	void AnalyzeBlerResults(BlerResult& result, const std::vector<DWORD>& errorLBAs, int scanSpeed);
	void PrintBlerReport(const DiscInfo& disc, const BlerResult& result);

	// Disc rot C1 integration
	void AnalyzeC1RotPatterns(const QCheckResult& c1Result,
		DWORD firstLBA, DWORD lastLBA, DiscRotAnalysis& analysis);

	// Offset handling
	int DetectDriveOffset();
	bool DetectDriveOffset(OffsetDetectionResult& result);
	void ApplyOffsetCorrection(DiscInfo& disc);

	// Media control
	bool Eject() { return m_drive.Eject(); }

	// Direct drive access (for protection scanning, etc.)
	ScsiDrive& GetDriveRef() { return m_drive; }

	// Enhanced disc rot detection
	bool RunDiscRotScan(DiscInfo& disc, DiscRotAnalysis& result, int scanSpeed);
	bool TestReadConsistency(DWORD lba, int passes, int& inconsistentCount, int readSpeed = 8);
	void AnalyzeErrorPatterns(const std::vector<DWORD>& errorLBAs, DiscRotAnalysis& result);
	void PrintDiscRotReport(const DiscRotAnalysis& result);
	bool SaveDiscRotLog(const DiscRotAnalysis& result, const std::wstring& filename);

	// Additional disc rot detection
	bool RunSpeedComparisonTest(DiscInfo& disc, std::vector<SpeedComparisonResult>& results);
	bool CheckLeadAreas(DiscInfo& disc, int scanSpeed = 4);
	void GenerateSurfaceMap(DiscInfo& disc, const std::wstring& filename, int scanSpeed = 8);

	// Additional error detection methods
	bool RunMultiPassVerification(DiscInfo& disc, std::vector<MultiPassResult>& results,
		int passes = 3, int scanSpeed = 8);
	bool AnalyzeAudioContent(DiscInfo& disc, AudioAnalysisResult& result, int scanSpeed = 16);
	bool RunSeekTimeAnalysis(DiscInfo& disc, std::vector<SeekTimeResult>& results);
	// Accept a scan speed for subchannel verification (default kept for compatibility)
	bool VerifySubchannelIntegrity(DiscInfo& disc, int& errorCount, int scanSpeed = 8);
	bool RunComprehensiveScan(DiscInfo& disc, ComprehensiveScanResult& result, int speed = 8);
	void PrintComprehensiveReport(const ComprehensiveScanResult& result);
	bool SaveComprehensiveReport(const ComprehensiveScanResult& result, const std::wstring& filename);

	// Subchannel burn verification
	bool VerifySubchannelBurnStatus(DiscInfo& disc, SubchannelBurnResult& result, int scanSpeed = 8);
	void PrintSubchannelBurnReport(const SubchannelBurnResult& result);

	// Enhanced error checking
	bool ValidateDiscStructure(const DiscInfo& disc, std::vector<std::string>& issues);
	bool VerifyWrittenFile(const std::wstring& filename, const DiscInfo& disc,
		std::vector<DWORD>& mismatchedSectors);
	bool CheckDiskSpace(const std::wstring& path, DWORD sectorsNeeded);
	bool RunPreflightChecks(DiscInfo& disc, std::vector<std::string>& warnings);
	ErrorStatistics CalculateErrorStatistics(const DiscInfo& disc);

	// CRC verification
	uint32_t CalculateTrackCRC(const DiscInfo& disc, int trackIndex);
	bool VerifyTrackCRCs(const DiscInfo& disc, std::vector<CRCVerification>& results);
	bool CompareDiscCRCs(const std::vector<std::pair<int, uint32_t>>& originalCRCs,
		const std::vector<std::pair<int, uint32_t>>& copyCRCs);
	int DetectSampleOffset(const std::vector<std::vector<BYTE>>& origSectors,
		const std::vector<std::vector<BYTE>>& copySectors,
		int maxOffsetSamples = 3000);
	void ApplySampleOffset(std::vector<std::vector<BYTE>>& rawSectors, int offsetSamples);

	// Drive capabilities
	bool DetectDriveCapabilities(DriveCapabilities& caps);
	void PrintDriveCapabilities(const DriveCapabilities& caps);

	// CD-Text write-path probe (see OpticalDrive_DriveCapabilities.cpp). Runs
	// MODE SELECT + MODE SENSE readback for each SAO/RAW + subchannel write mode
	// to report which the drive actually ACCEPTS, silently DOWNGRADES, or
	// REJECTS -- so we know whether host-side CD-Text (lead-in R-W) is even
	// possible on this drive (WRITE BUFFER 0x3B is rejected by many non-Plextor
	// drives). Non-destructive: no disc I/O; restores default SAO write params.
	void ProbeCDTextWritePaths();

	// Disc fingerprinting
	bool GenerateDiscFingerprint(const DiscInfo& disc, DiscFingerprint& fingerprint);
	bool CalculateCDDBId(const DiscInfo& disc, CDDBFingerprint& cddb);
	bool CalculateMusicBrainzId(const DiscInfo& disc, MusicBrainzFingerprint& mb);
	bool CalculateAccurateRipId(const DiscInfo& disc, AccurateRipFingerprint& ar);
	bool CalculateAudioFingerprint(const DiscInfo& disc, AudioFingerprint& audio);
	void PrintDiscFingerprint(const DiscFingerprint& fingerprint);
	bool SaveDiscFingerprint(const DiscFingerprint& fingerprint, const std::wstring& filename);

	// C2 validation
	bool ValidateC2Accuracy(DWORD testLBA);

	// Display drive-specific recommendations
	void ShowDriveRecommendations();
	std::string GetDriveRecommendation() {
		return m_drive.GetDriveRecommendationText();
	}

	// Chipset / controller identification
	bool DetectChipset(ChipsetInfo& info);
	void PrintChipsetInfo(const ChipsetInfo& info);

	// ── Disk Writing Operations ─────────────────────────────────────
	// Subchannel is always drive-generated via the SAO path, which is reliable
	// and reproduces pregaps exactly. Raw P-W subchannel writing from a .sub file
	// was removed: many drives "accept" a raw/DAO-96 subchannel mode at the MODE
	// SELECT level but cannot actually write it, producing a garbage burn. The
	// trailing bool is retained (ignored) for source compatibility with older
	// callers and is scheduled for removal.
	bool WriteDisc(const std::wstring& binFile,
		const std::wstring& cueFile, const std::wstring& subFile,
		int speed, bool usePowerCalibration, bool discAlreadyBlanked = false,
		bool /*attemptSubchannel_deprecated*/ = false);

	// quiet=true suppresses the console readout (media type / status) for callers
	// that only need the isFull/isRewritable results and re-report the disc state
	// themselves later (e.g. Copy disc / Write tracks, which print the media type
	// in WriteDisc right before the burn). Erase CD-RW leaves it false to show it.
	// outIsBlank (optional) is set true when the disc is already empty (disc
	// status 0x00) so callers can skip an unnecessary erase. It is left false
	// when the disc state can't be determined (e.g. the GET CONFIGURATION
	// fallback), so the caller errs on the side of erasing.
	bool CheckRewritableDisk(bool& isFull, bool& isRewritable, bool quiet = false,
		bool* outIsBlank = nullptr);

	// skipConfirm=true bypasses the built-in "erase all data?" prompt for callers
	// that have already gated the operation (e.g. Erase CD-RW only reaches the
	// blank pass when the disc is confirmed rewritable AND not already blank).
	bool BlankRewritableDisk(int speed, bool quickBlank = true, bool skipConfirm = false);

	bool PerformPowerCalibration();

	bool VerifyWriteCompletion(const std::wstring& binFile);

	// ── Production Write Implementation ───────────────────────────
	struct TrackWriteInfo {
		int trackNumber;
		DWORD startLBA;
		DWORD endLBA;
		DWORD pregapLBA;
		bool isAudio;
		bool hasPregap;        // true only when INDEX 00 was explicitly in the CUE file
		int dataMode;          // 0=audio, 1=MODE1/2352, 2=MODE2/2352
		std::string isrcCode;
		std::string title;     // CD-Text: track title from CUE TITLE command
		std::string performer; // CD-Text: track performer from CUE PERFORMER command
	};

	bool ParseCueSheet(const std::wstring& cueFile,
		std::vector<TrackWriteInfo>& tracks);
	// Overload that also extracts disc-level CD-Text metadata and MCN
	bool ParseCueSheet(const std::wstring& cueFile,
		std::vector<TrackWriteInfo>& tracks,
		std::string& discTitle, std::string& discPerformer,
		std::string& discMCN);

	bool WriteAudioSectors(const std::wstring& binFile,
		const std::wstring& subFile,
		const std::vector<TrackWriteInfo>& tracks,
		DWORD totalSectors,
		bool hasSubchannel,
		bool needsDeinterleave = false,
		int subchannelMode = 0,
		const std::string& discMCN = "");

	bool VerifyWrittenDisc(const std::vector<TrackWriteInfo>& tracks);

	// IMAPI2 fallback path used when the drive rejects raw-SCSI CUE SHEET writes.
	bool WriteDiscIMAPI(const std::wstring& binFile,
		const std::vector<TrackWriteInfo>& tracks,
		DWORD totalSectors, int speed);

	// Disc balance / wobble detection
	bool CheckDiscBalance(DiscInfo& disc, int& balanceScore);

private:
	ScsiDrive m_drive;
	bool m_hasAccurateStream = false;            // Cached from DetectDriveCapabilities
	bool m_capabilitiesDetected = false;         // Whether capabilities have been queried

	// Ensure drive capabilities have been queried at least once
	void EnsureCapabilitiesDetected();

	// Internal constants
	static constexpr int MAX_RETRIES = 5;
	static constexpr int RETRY_SPEED_REDUCTION = 4;

	// Internal reading methods
	bool ReadSectorWithRetry(DWORD lba, BYTE* data, int sectorSize, bool isAudio,
		bool includeSubchannel, int& retryCount, bool detectC2, int* c2Errors);
	bool ReadSectorSecure(DWORD lba, BYTE* data, int sectorSize, bool isAudio,
		const SecureRipConfig& config, SecureSectorResult& result, DWORD maxLBA = 0);
	// Recovery-rip per-sector consensus rescue. Rebuilds the 2352-byte audio
	// payload of one hard sector into audioOut by aligned per-byte voting.
	bool RescueSectorConsensus(DWORD lba, BYTE* audioOut, const RecoveryRipConfig& cfg,
		DWORD maxLBA, bool c2TieBreak, RecoverySectorResult& sr);
	bool DefeatDriveCache(DWORD currentLBA, DWORD maxLBA = 0);
	bool FlushDriveCache();
	uint32_t HashSector(const BYTE* data, int size);
	uint32_t CalculateSectorHash(const BYTE* data);

	// Shared utility
	DWORD CalculateTotalAudioSectors(const DiscInfo& disc) const;

	// Pioneer vendor-scan fallback for the per-sector C2 scans (display options
	// 7 "C2 Error Scan" and 8 "BLER Scan"). On Pioneer BD burners (e.g. BDR-S13U)
	// the per-sector READ CD C2 area reads all-zero, so those scans would report
	// a clean disc regardless of its real condition. When the drive is Pioneer
	// with the vendor quality scan available, this runs that scan (the same
	// 0x3B/0x3C path as option 6) and fills `result` from it so the caller's
	// report and CSV log reflect real C1/C2/CU. Returns true when it handled the
	// scan; false to let the caller fall through to the per-sector path.
	bool RunPioneerVendorC2Fallback(const DiscInfo& disc, BlerResult& result,
		int scanSpeed, const char* featureLabel);

	// Pioneer CU cross-check for the vendor quality scan (option 6). The Pioneer
	// 0x3B/0x3C vendor scan reports C1 (BLER) and the E22 second-stage counter
	// but no uncorrectable (E32/CU) figure. This runs the separate Pioneer CD
	// Check (0xE6+0x300000) protocol over the same audio range to obtain a real
	// uncorrectable measurement, filling the pioneerCdCheck* fields of `result`.
	// Returns true only when the CD Check produced valid data. Quick no-op on
	// non-Pioneer drives and on Pioneer firmware that dropped the protocol (e.g.
	// BDR-S13U: the start command fails fast). Cancellable via ESC/Ctrl+C.
	bool RunPioneerCdCheckCrosscheck(const DiscInfo& disc, QCheckResult& result);

	// Disc rot analysis helpers
	void ClassifyZone(DWORD lba, DWORD firstLBA, DWORD lastLBA, int c2Errors, DiscZoneStats& zones);
	int CalculateClusterTolerance(int scanSpeed);
	void DetectErrorClusters(const std::vector<DWORD>& errorLBAs, std::vector<ErrorCluster>& clusters, int scanSpeed = 8);
	std::string AssessRotRisk(const DiscRotAnalysis& result);
	int CalculateOverallScore(const ComprehensiveScanResult& result);

	// Audio analysis helpers
	bool IsSectorSilent(const BYTE* data);
	bool IsSectorClipped(const BYTE* data);

	// Fingerprint helpers
	int CDDBSum(int n);
	std::string Base64Encode(const BYTE* data, size_t length);
	void SHA1Hash(const BYTE* data, size_t length, BYTE* output);

	// BLER Report Helpers
	void PrintBlerZoneStats(const BlerResult& result);
	void PrintBlerClusters(const BlerResult& result);
	void PrintBlerWorstSectors(const BlerResult& result);
	void PrintBlerDensityDistribution(const BlerResult& result);
	void PrintBlerPerTrackSummary(const DiscInfo& disc, const BlerResult& result);
	void PrintBlerMarginAnalysis(const BlerResult& result);
	void PrintBlerQualitySummary(const BlerResult& result);
};
