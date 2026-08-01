// ============================================================================
// DriveTypes.h - Drive capability and health structures
// ============================================================================
#pragma once

#include <windows.h>
#include <vector>
#include <string>

enum class RawSectorLayout {
	Unknown = 0,
	DataC2Sub,
	DataSubC2
};

enum class DriveReadMethod {
	Unknown = 0,
	ReadCD,
	ReadCDDA
};

struct DriveCharacterization {
	bool performed = false;
	bool loadedFromCache = false;
	bool profileSaved = false;
	std::string vendor;
	std::string model;
	std::string firmware;
	std::string serialNumber;
	DriveReadMethod preferredReadMethod = DriveReadMethod::Unknown;
	RawSectorLayout rawSectorLayout = RawSectorLayout::Unknown;
	bool readCdAudio = false;
	bool readCdda = false;
	bool rawSubchannelFunctional = false;
	bool c2Functional = false;
	bool stableRepeatedReads = false;
	bool cacheDefeatVerified = false;
	int leadInReadableSectors = 0;
	int leadOutReadableSectors = 0;
	std::string notes;
};

bool LoadDriveCharacterizationProfile(const std::string& vendor,
	const std::string& model, const std::string& firmware,
	DriveCharacterization& profile);
bool SaveDriveCharacterizationProfile(const DriveCharacterization& profile);
std::wstring GetDriveCharacterizationProfilePath();

// ── Drive health indicators ─────────────────────────────────────────────────
// Quick status check for the physical drive and inserted media.
struct DriveHealthCheck {
	bool driveResponding = false;  // Drive answers SCSI TEST UNIT READY
	bool mediaPresent = false;     // A disc is inserted
	bool mediaReady = false;       // Disc is spun up and readable
	bool trayOpen = false;         // Drive tray is open
	bool spinningUp = false;       // Drive is in the process of spinning up
	bool writeProtected = false;   // Media is read-only
	std::string mediaType;         // "CD-DA", "CD-ROM", "UNKNOWN"
	int firmwareErrors = 0;        // Firmware-reported error count
};

// ── CD-ROM chipset / controller identification ──────────────────────────────
// Populated by probing SCSI INQUIRY data, vendor strings, firmware signatures,
// and known model-to-chipset mappings.  Useful for understanding drive quirks
// and setting optimal extraction parameters.
enum class ChipsetFamily {
	Unknown,
	MediaTek,        // MediaTek (formerly MT1818/MT1898 etc.) - most modern slim drives
	Renesas,         // Renesas (NEC) - used in many USB/laptop drives
	Panasonic,       // Panasonic/Matsushita MN103 series
	Sanyo,           // Sanyo LC897xx series - older CD/DVD
	Philips,         // Philips SAA78xx / CDD series
	Sony,            // Sony CXD series
	Plextor,         // Plextor custom (Sanyo-derived) - premium audio drives
	LiteOn,          // LiteOn (MediaTek-based) - common desktop drives
	Pioneer,         // Pioneer custom chipsets
	Realtek,         // Realtek USB bridge controllers
	JMicron,         // JMicron JMS578/JMS567 USB-SATA bridges
	ASMedia,         // ASMedia ASM1153/ASM1351 USB bridges
	VIA,             // VIA VT6315 / VT1708 series
	NEC,             // NEC/Renesas legacy controllers
	Ricoh            // Ricoh controllers
};

struct ChipsetInfo {
	ChipsetFamily family = ChipsetFamily::Unknown;
	std::string chipsetName;           // e.g. "MediaTek MT1959"
	std::string detectionMethod;       // How the chipset was identified
	std::string interfaceType;         // "SATA", "USB", "IDE/ATAPI"
	std::string usbBridge;             // USB bridge chip if detected (e.g. "JMicron JMS578")
	bool isUSBAttached = false;        // Drive is behind a USB bridge
	bool knownAudioQuirks = false;     // Chipset has known audio extraction issues
	std::string quirkDescription;      // Description of known quirks
	int confidencePercent = 0;         // 0-100 detection confidence
	std::string plextorTLA;            // Plextor TLA / hardware revision (e.g. "0301"); empty on non-Plextor
};

// ── Drive capabilities descriptor ───────────────────────────────────────────
// Populated by querying the drive's SCSI MODE SENSE / GET CONFIGURATION pages.
// Used to determine rip-quality suitability and available features.
struct DriveCapabilities {
	// ── Identification ──
	std::string vendor;                           // e.g. "PLEXTOR"
	std::string model;                            // e.g. "PX-716A"
	std::string firmware;                         // Firmware revision string
	std::string serialNumber;                     // From VPD page 0x80

	// ── Core ripping capabilities ──
	bool supportsC2ErrorReporting = false;        // Can return C2 error pointers
	bool supportsAccurateStream = false;          // Guarantees no jitter in streaming reads
	bool supportsCDText = false;                  // Can read CD-TEXT from lead-in
	bool supportsWriteCDText = false;             // Can write CD-TEXT to lead-in
	bool supportsRawRead = false;                 // Can perform raw sector reads (0xBE)

	// ── Advanced features ──
	bool supportsOverreadLeadIn = false;          // Can read before LBA 0 (lead-in overread)
	bool supportsOverreadLeadOut = false;         // Can read past the lead-out
	bool supportsSubchannelRaw = false;           // Can return raw P–W subchannel data
	bool supportsSubchannelDeinterleaved = false; // Can return corrected/deinterleaved R–W data
	bool supportsSubchannelQ = false;             // Formatted Q reads were verified with loaded CD media
	bool supportsCDDA = false;                    // Digital audio extraction supported
	bool supportsMultiSession = false;            // Can read multi-session discs
	bool activeCDReadProbesPerformed = false;     // Media-dependent READ CD/Q/overread probes ran

	// ── Audio playback features ──
	bool supportsAudioPlay = false;               // Legacy hardware/analog CD audio playback
	bool supportsCompositeOutput = false;         // Composite audio output jack
	bool supportsSeparateVolume = false;          // Per-channel volume control
	bool supportsSeparateMute = false;            // Per-channel mute

	// ── Mechanical features ──
	bool supportsEject = false;                   // Can eject the tray via software
	bool supportsLockMedia = false;               // Can lock the tray closed
	bool isChanger = false;                       // Multi-disc changer mechanism
	int loadingMechanism = 0;                     // 0=caddy, 1=tray, 2=popup, 4/5=changer

	// ── Performance info ──
	int maxReadSpeedKB = 0;                       // Maximum read speed in KB/s
	int maxWriteSpeedKB = 0;                      // Maximum write speed (0 = read-only drive)
	int currentReadSpeedKB = 0;                   // Currently configured read speed
	int currentWriteSpeedKB = 0;                  // Currently configured write speed
	int bufferSizeKB = 0;                         // Drive internal buffer / cache size
	std::vector<int> supportedReadSpeeds;         // All supported read speeds in KB/s
	std::vector<int> supportedWriteSpeeds;        // All supported write speeds in KB/s

	// ── Readable media types ──
	bool readsCDR = false;
	bool readsCDRW = false;
	bool readsDVD = false;
	bool readsBD = false;

	// ── Writable media types ──
	bool writesCDR = false;
	bool writesCDRW = false;
	bool writesDVD = false;
	bool writesDVDRAM = false;                    // DVD-RAM write support
	bool writesBD = false;                        // Blu-ray write support

	// ── Write features ──
	bool supportsTestWrite = false;               // Simulation / test-write mode
	bool supportsBufferUnderrunProtection = false; // BUP / Burn-Free
	bool supportsWriteTAO = false;                // Track-At-Once
	bool supportsWriteSAO = false;                // Session-At-Once / Disc-At-Once
	bool supportsWriteRAW = false;                // Raw write mode

	// ── Current media info ──
	bool mediaPresent = false;                    // Whether a disc is currently loaded
	WORD currentMediaProfile = 0;                 // MMC profile code (e.g. 0008h CD-ROM)
	std::string currentMediaType;                 // Detected media type string
};

// ── Drive-specific configuration overrides ───────────────────────────────────
// Used for drives with known quirks or optimal settings for audio extraction.
struct DriveSpecificConfig {
	std::string vendor;
	std::string model;
	
	// Audio extraction settings
	int readOffset = 0;                           // Sample offset correction
	bool forceAccurateStream = false;             // Override detection
	bool forceC2ErrorReporting = false;           // Enable/disable C2
	bool disableC2ErrorReporting = false;         // Explicitly disable C2
	
	// Speed settings for best audio quality
	int recommendedReadSpeed = 0;                 // 0 = max, otherwise KB/s
	bool limitSpeedForAudio = false;              // True if slower = better quality
	
	// Behavioral quirks
	bool requiresExtraSpinup = false;             // Need longer spin-up time
	bool hasJitterIssues = false;                 // Known to produce jitter
	bool requiresCacheFlush = false;              // Flush cache between reads
	int retryCount = 3;                           // Default retry count for errors
	
	// Feature overrides
	bool supportsOverreadLeadIn = false;          // Override capability detection
	bool supportsOverreadLeadOut = false;
	
	// Burst mode optimization (NEW FIELDS)
	bool optimizedForBurst = false;               // Drive benefits from multi-sector reads
	int burstReadSize = 26;                       // Optimal sectors per batch (0 = default)
	bool cacheBehaviorPredictable = false;        // Cache doesn't interfere with accuracy
	bool fastSeekRecovery = false;                // Quick recovery after cache defeat seeks
};

// Known drive configurations for optimal audio extraction
static const DriveSpecificConfig knownDriveConfigs[] = {
	// ═══════════════════════════════════════════════════════════════
	// Pioneer BD Drives - EXCELLENT for audio extraction
	// ═══════════════════════════════════════════════════════════════
	{
		"PIONEER", "BDR-S13U",
		667,                     // readOffset (AccurateRip verified)
		true,                    // forceAccurateStream (guaranteed jitter-free)
		true,                    // forceC2ErrorReporting (reliable C2)
		false,                   // disableC2ErrorReporting
		10560,                   // recommendedReadSpeed (24x = maximum)
		false,                   // limitSpeedForAudio (excellent at full speed)
		false,                   // requiresExtraSpinup
		false,                   // hasJitterIssues (rock-solid mechanical)
		false,                   // requiresCacheFlush (smart cache design)
		2,                       // retryCount (usually succeeds first try)
		true,                    // supportsOverreadLeadIn
		true,                    // supportsOverreadLeadOut
		true,                    // optimizedForBurst ← KEY FEATURE
		26,                      // burstReadSize (optimal batch size)
		true,                    // cacheBehaviorPredictable ← SAFE CACHE
		true                     // fastSeekRecovery
	},
	{
		"PIONEER", "BD-RW BDR-S12U",
		667, true, true, false, 8467, false, false, false, false, 3, true, true,
		true, 26, true, true  // Burst optimized
	},
	{
		"PIONEER", "BD-RW BDR-209",
		667, true, true, false, 8467, false, false, false, false, 3, true, true,
		true, 26, true, true  // Burst optimized
	},
	
	// Plextor Premium drives - legendary audio quality
	{
		"PLEXTOR", "CD-R PREMIUM2",
		30, true, true, false, 0, false, false, false, false, 2, true, true,
		true, 26, true, false  // Burst capable
	},
	{
		"PLEXTOR", "CD-R PREMIUM",
		30, true, true, false, 0, false, false, false, false, 2, true, true,
		true, 26, true, false  // Burst capable
	},
	
	// LG drives - good but may need slower speeds
	{
		"LG", "BD-RE WH16NS60",
		6, false, false, false, 4233, true, false, false, false, 5, false, false,
		false, 26, false, false  // Single-sector recommended
	}
};

// ── Drive validation tier ───────────────────────────────────────────────────
// Three-tier classification for recognizing drives and preserving the
// Curated/KnownGoodFamily distinction. Curated wins over family if a drive
// appears in both lists.
enum class DriveTier {
	Unknown = 0,         // Not recognized
	KnownGoodFamily,     // Recognized family/rebrand, per-drive params unverified
	Curated,             // Individual entry in knownDriveConfigs with verified
	                     // offset / C2 / accurate-stream values
};

// Lightweight family table. Vendor matches exactly (after upper/trim normalize);
// model matches as a substring. No parameters: this list only declares "we
// recognize this family as broadly functional", not "we vouch for its numbers".
struct DriveFamilyEntry {
	const char* vendorPattern;
	const char* modelPattern;
	const char* familyName;     // Returned to callers that request the matched family.
};

static const DriveFamilyEntry knownDriveFamilies[] = {
	// Genuine pre-LiteOn Plextor classics — PX-708/712/716/755/760 etc.
	// Renowned for audio extraction; README lists these as best-supported.
	// Premium / Premium2 are individually Curated and take precedence.
	// "PX-7" is unambiguous: LiteOn-era Plextor uses PX-8xx (see below).
	{ "PLEXTOR", "PX-7",     "Plextor classic (PX-7xx series)" },

	// Plextor-branded LiteOn iHAS rebrands (post-2009 Plextor is LiteOn OEM).
	{ "PLEXTOR", "PX-891",   "LiteOn iHAS124 (Plextor rebrand)" },
	{ "PLEXTOR", "PX-880",   "LiteOn iHAS122 (Plextor rebrand)" },
	{ "PLEXTOR", "PX-870",   "LiteOn iHAS rebrand" },
	{ "PLEXTOR", "PX-820",   "LiteOn iHAS rebrand" },

	// Native LiteOn iHAS / iHBS. Inquiry vendor varies by firmware era:
	// "LITE-ON", "LITEON", or "PLDS" (Philips Lite-On Digital Solutions JV).
	{ "LITE-ON", "IHAS",     "LiteOn iHAS" },
	{ "LITEON",  "IHAS",     "LiteOn iHAS" },
	{ "PLDS",    "IHAS",     "LiteOn iHAS (PLDS firmware)" },
	{ "LITE-ON", "IHBS",     "LiteOn iHBS (BD)" },
	{ "LITEON",  "IHBS",     "LiteOn iHBS (BD)" },
	{ "PLDS",    "IHBS",     "LiteOn iHBS (PLDS firmware)" },
	{ "LITE-ON", "IHOS",     "LiteOn iHOS (BD)" },

	// ASUS rebrands of LiteOn.
	{ "ASUS",    "DRW-",     "ASUS DRW (LiteOn-based)" },
	{ "ASUS",    "BW-",      "ASUS BW (LiteOn-based BD)" },
	{ "ASUS",    "BC-",      "ASUS BC (LiteOn-based BD-ROM)" },

	// Pioneer DVR / BDR families. Specific BDR-S12U/S13U/209 are Curated and
	// take precedence; this catches the other Pioneer models in the family.
	{ "PIONEER", "BDR-",     "Pioneer BDR family" },
	{ "PIONEER", "DVR-",     "Pioneer DVR family" },

	// LG / Hitachi-LG. Inquiry vendor is usually "HL-DT-ST", occasionally "LG".
	// WH16NS60 is Curated and takes precedence.
	{ "HL-DT-ST", "BD-",     "LG/HL-DT-ST BD family" },
	{ "HL-DT-ST", "BH",      "LG/HL-DT-ST BD writer" },
	{ "HL-DT-ST", "WH",      "LG/HL-DT-ST BD writer" },
	{ "HL-DT-ST", "GH",      "LG/HL-DT-ST DVD writer" },
	{ "HL-DT-ST", "DVDRAM",  "LG/HL-DT-ST DVD-RAM" },
	{ "LG",       "BD-",     "LG BD family" },
};
