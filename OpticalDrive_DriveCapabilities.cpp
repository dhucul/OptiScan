#define NOMINMAX
#include "OpticalDrive.h"
#include "PioneerVendor.h"
#include <iostream>
#include <iomanip>
#include <algorithm>

// ============================================================================
// Drive Capabilities Detection & Reporting
// ============================================================================

bool OpticalDrive::DetectDriveCapabilities(DriveCapabilities& caps) {
	// Delegate to the comprehensive ScsiDrive::DetectCapabilities
	// which queries INQUIRY, VPD 0x80, Mode Page 2A, GET PERFORMANCE,
	// C2 support, raw read, and overread capabilities
	bool result = m_drive.DetectCapabilities(caps);
	if (result) {
		m_hasAccurateStream = caps.supportsAccurateStream;
		m_capabilitiesDetected = true;
	}
	return result;
}

void OpticalDrive::PrintDriveCapabilities(const DriveCapabilities& caps) {
	auto yn = [](bool v) -> const char* { return v ? "YES" : "NO"; };

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              DRIVE CAPABILITIES REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	// --- Identification ---
	std::cout << "\n--- Identification ---\n";
	std::cout << "  Vendor:          " << caps.vendor << "\n";
	std::cout << "  Model:           " << caps.model << "\n";
	std::cout << "  Firmware:        " << (caps.firmware.empty() ? "(unknown)" : caps.firmware) << "\n";
	std::cout << "  Serial Number:   " << (caps.serialNumber.empty() ? "(not reported)" : caps.serialNumber) << "\n";

	// --- Core Ripping Capabilities ---
	std::cout << "\n--- Core Ripping Capabilities ---\n";
	std::cout << "  CD-DA Extraction:      " << yn(caps.supportsCDDA) << "\n";
	std::cout << "  Accurate Stream:       " << yn(caps.supportsAccurateStream) << "\n";
	std::cout << "  C2 Error Reporting:    " << yn(caps.supportsC2ErrorReporting) << "\n";
	std::cout << "  Raw Read:              " << yn(caps.supportsRawRead) << "\n";
	std::cout << "  CD-TEXT Reading:       " << yn(caps.supportsCDText) << "\n";

	// --- Subchannel & Overread ---
	std::cout << "\n--- Subchannel & Overread ---\n";
	std::cout << "  Raw Subchannel:        " << yn(caps.supportsSubchannelRaw) << "\n";
	std::cout << "  Q-Channel:             " << yn(caps.supportsSubchannelQ) << "\n";
	std::cout << "  Overread Lead-In:      " << yn(caps.supportsOverreadLeadIn) << "\n";
	std::cout << "  Overread Lead-Out:     " << yn(caps.supportsOverreadLeadOut) << "\n";

	// --- Media Type Support ---
	std::cout << "\n--- Media Type Support ---\n";
	std::cout << "  Reads:  CD-R=" << yn(caps.readsCDR)
		<< "  CD-RW=" << yn(caps.readsCDRW)
		<< "  DVD=" << yn(caps.readsDVD)
		<< "  BD=" << yn(caps.readsBD) << "\n";
	std::cout << "  Writes: CD-R=" << yn(caps.writesCDR)
		<< "  CD-RW=" << yn(caps.writesCDRW)
		<< "  DVD=" << yn(caps.writesDVD) << "\n";

	// --- Audio Playback ---
	std::cout << "\n--- Audio Playback ---\n";
	std::cout << "  Digital Audio Play:    " << yn(caps.supportsDigitalAudioPlay) << "\n";
	std::cout << "  Separate Volume:       " << yn(caps.supportsSeparateVolume) << "\n";
	std::cout << "  Separate Mute:         " << yn(caps.supportsSeparateMute) << "\n";
	std::cout << "  Composite Output:      " << yn(caps.supportsCompositeOutput) << "\n";

	// --- Mechanical Features ---
	std::cout << "\n--- Mechanical Features ---\n";
	const char* mechNames[] = { "Caddy", "Tray", "Pop-up", "Changer", "Reserved", "Slot" };
	const char* mechName = (caps.loadingMechanism >= 0 && caps.loadingMechanism <= 5)
		? mechNames[caps.loadingMechanism] : "Unknown";
	std::cout << "  Loading Mechanism:     " << mechName << "\n";
	std::cout << "  Eject:                 " << yn(caps.supportsEject) << "\n";
	std::cout << "  Lock Media:            " << yn(caps.supportsLockMedia) << "\n";
	std::cout << "  Multi-Session:         " << yn(caps.supportsMultiSession) << "\n";
	std::cout << "  Disc Changer:          " << yn(caps.isChanger) << "\n";

	// --- Write Capabilities ---
	bool canWrite = caps.writesCDR || caps.writesCDRW || caps.writesDVD
		|| caps.writesDVDRAM || caps.writesBD;
	std::cout << "\n--- Write Capabilities ---\n";
	if (!canWrite) {
		std::cout << "  (read-only drive)\n";
	}
	else {
		std::cout << "  Writes CD-R:           " << yn(caps.writesCDR) << "\n";
		std::cout << "  Writes CD-RW:          " << yn(caps.writesCDRW) << "\n";
		std::cout << "  Writes DVD:            " << yn(caps.writesDVD) << "\n";
		std::cout << "  Writes DVD-RAM:        " << yn(caps.writesDVDRAM) << "\n";
		std::cout << "  Writes BD:             " << yn(caps.writesBD) << "\n";
		std::cout << "  Test Write (Simulate): " << yn(caps.supportsTestWrite) << "\n";
		std::cout << "  Buffer Underrun Prot:  " << yn(caps.supportsBufferUnderrunProtection) << "\n";
		std::cout << "  Write TAO:             " << yn(caps.supportsWriteTAO) << "\n";
		std::cout << "  Write SAO/DAO:         " << yn(caps.supportsWriteSAO) << "\n";
	}

	// --- Performance ---
	std::cout << "\n--- Performance ---\n";
	if (caps.maxReadSpeedKB > 0)
		std::cout << "  Max Read Speed:        " << caps.maxReadSpeedKB << " KB/s ("
		<< caps.maxReadSpeedKB / 176 << "x)\n";
	if (caps.currentReadSpeedKB > 0)
		std::cout << "  Current Read Speed:    " << caps.currentReadSpeedKB << " KB/s ("
		<< caps.currentReadSpeedKB / 176 << "x)\n";
	if (caps.maxWriteSpeedKB > 0)
		std::cout << "  Max Write Speed:       " << caps.maxWriteSpeedKB << " KB/s ("
		<< caps.maxWriteSpeedKB / 176 << "x)\n";
	else if (canWrite)
		std::cout << "  Max Write Speed:       (not reported)\n";
	else
		std::cout << "  Max Write Speed:       (read-only drive)\n";
	if (caps.currentWriteSpeedKB > 0)
		std::cout << "  Current Write Speed:   " << caps.currentWriteSpeedKB << " KB/s ("
		<< caps.currentWriteSpeedKB / 176 << "x)\n";
	if (caps.bufferSizeKB > 0)
		std::cout << "  Buffer Size:           " << caps.bufferSizeKB << " KB\n";

	if (!caps.supportedReadSpeeds.empty()) {
		std::cout << "  Supported Read Speeds: ";
		for (size_t i = 0; i < caps.supportedReadSpeeds.size(); i++) {
			if (i > 0) std::cout << ", ";
			std::cout << caps.supportedReadSpeeds[i] / 176 << "x";
		}
		std::cout << "\n";
	}

	if (!caps.supportedWriteSpeeds.empty()) {
		std::cout << "  Supported Write Speeds:";
		for (size_t i = 0; i < caps.supportedWriteSpeeds.size(); i++) {
			if (i > 0) std::cout << ", ";
			std::cout << caps.supportedWriteSpeeds[i] / 176 << "x";
		}
		std::cout << "\n";
	}

	// --- Drive Accuracy Rating ---
	std::cout << "\n--- Drive Accuracy Rating ---\n";

	int ratingScore = 0;
	constexpr int kMaxScore = 100;

	// CD-DA extraction is mandatory for any audio ripping (30 pts)
	if (caps.supportsCDDA) {
		ratingScore += 30;
		std::cout << "  [+30] CD-DA extraction supported\n";
	}
	else {
		std::cout << "  [  0] CD-DA extraction NOT supported (critical)\n";
	}

	// Accurate Stream means jitter-free delivery — no re-reads needed (25 pts)
	if (caps.supportsAccurateStream) {
		ratingScore += 25;
		std::cout << "  [+25] Accurate Stream reported\n";
	}
	else {
		std::cout << "  [  0] Accurate Stream NOT reported (will need verification reads)\n";
	}

	// C2 error reporting enables reliable error detection (20 pts)
	if (caps.supportsC2ErrorReporting) {
		ratingScore += 20;
		std::cout << "  [+20] C2 error reporting supported\n";
	}
	else {
		std::cout << "  [  0] C2 error reporting NOT supported\n";
	}

	// Overread lead-in/lead-out allows offset correction at disc edges (5+5 pts)
	if (caps.supportsOverreadLeadIn) {
		ratingScore += 5;
		std::cout << "  [+ 5] Overread into lead-in supported\n";
	}
	else {
		std::cout << "  [  0] Overread into lead-in NOT supported\n";
	}
	if (caps.supportsOverreadLeadOut) {
		ratingScore += 5;
		std::cout << "  [+ 5] Overread into lead-out supported\n";
	}
	else {
		std::cout << "  [  0] Overread into lead-out NOT supported\n";
	}

	// Raw read support for sector-level access (5 pts)
	if (caps.supportsRawRead) {
		ratingScore += 5;
		std::cout << "  [+ 5] Raw read supported\n";
	}
	else {
		std::cout << "  [  0] Raw read NOT supported\n";
	}

	// Subchannel support aids metadata verification (3+3 pts)
	if (caps.supportsSubchannelRaw) {
		ratingScore += 3;
		std::cout << "  [+ 3] Raw subchannel read supported\n";
	}
	if (caps.supportsSubchannelQ) {
		ratingScore += 3;
		std::cout << "  [+ 3] Q-channel de-interleaved read supported\n";
	}

	// Buffer size bonus: larger cache reduces re-read overhead (up to 4 pts)
	if (caps.bufferSizeKB >= 2048) {
		ratingScore += 4;
		std::cout << "  [+ 4] Large buffer (" << caps.bufferSizeKB << " KB)\n";
	}
	else if (caps.bufferSizeKB >= 512) {
		ratingScore += 2;
		std::cout << "  [+ 2] Moderate buffer (" << caps.bufferSizeKB << " KB)\n";
	}

	ratingScore = std::min(ratingScore, kMaxScore);

	const char* grade;
	const char* summary;
	if (ratingScore >= 90) {
		grade = "A+";
		summary = "Excellent -- ideal for bit-perfect secure ripping.";
	}
	else if (ratingScore >= 80) {
		grade = "A";
		summary = "Very good -- capable of accurate extraction with C2 or verification reads.";
	}
	else if (ratingScore >= 65) {
		grade = "B";
		summary = "Good -- usable but may need multi-pass verification for full confidence.";
	}
	else if (ratingScore >= 50) {
		grade = "C";
		summary = "Fair -- limited accuracy features; use Paranoid rip mode.";
	}
	else {
		grade = "D";
		summary = "Poor -- not recommended for accurate audio extraction.";
	}

	std::cout << "\n  Score: " << ratingScore << "/" << kMaxScore
		<< "  Grade: " << grade << "\n";
	std::cout << "  " << summary << "\n";

	// --- Pioneer vendor features (only on Pioneer drives) ---
	{
		PioneerVendor pioneer(GetDriveRef());
		if (pioneer.IsPioneerDrive()) {
			PioneerCapabilities pc;
			pioneer.ReadCapabilities(pc);
			pioneer.PrintCapabilitiesReport();
		}
	}

	std::cout << std::string(60, '=') << "\n";
}

void OpticalDrive::ShowDriveRecommendations() {
	m_drive.DisplayDriveRecommendations();
}

void OpticalDrive::EnsureCapabilitiesDetected() {
	if (!m_capabilitiesDetected) {
		DriveCapabilities caps;
		DetectDriveCapabilities(caps);
	}
}

// ============================================================================
// Chipset / Controller Identification
// ============================================================================

bool OpticalDrive::DetectChipset(ChipsetInfo& info) {
	return m_drive.DetectChipset(info);
}

void OpticalDrive::PrintChipsetInfo(const ChipsetInfo& info) {
	auto familyName = [](ChipsetFamily f) -> const char* {
		switch (f) {
		case ChipsetFamily::MediaTek:  return "MediaTek";
		case ChipsetFamily::Renesas:   return "Renesas";
		case ChipsetFamily::Panasonic: return "Panasonic";
		case ChipsetFamily::Sanyo:     return "Sanyo";
		case ChipsetFamily::Philips:   return "Philips";
		case ChipsetFamily::Sony:      return "Sony";
		case ChipsetFamily::Plextor:   return "Plextor";
		case ChipsetFamily::LiteOn:    return "LiteOn";
		case ChipsetFamily::Pioneer:   return "Pioneer";
		case ChipsetFamily::Realtek:   return "Realtek";
		case ChipsetFamily::JMicron:   return "JMicron";
		case ChipsetFamily::ASMedia:   return "ASMedia";
		case ChipsetFamily::VIA:       return "VIA";
		case ChipsetFamily::NEC:       return "NEC/Renesas";
		case ChipsetFamily::Ricoh:     return "Ricoh";
		default:                       return "Unknown";
		}
	};

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "            CHIPSET / CONTROLLER REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	std::cout << "\n--- Controller Identification ---\n";
	std::cout << "  Chipset Family:     " << familyName(info.family) << "\n";
	std::cout << "  Chipset Name:       " << info.chipsetName << "\n";
	std::cout << "  Detection Method:   " << info.detectionMethod << "\n";
	std::cout << "  Confidence:         " << info.confidencePercent << "%\n";
	if (!info.plextorTLA.empty()) {
		std::cout << "  Plextor TLA:        #" << info.plextorTLA << "\n";
	}

	std::cout << "\n--- Interface ---\n";
	std::cout << "  Interface Type:     " << info.interfaceType << "\n";
	std::cout << "  USB Attached:       " << (info.isUSBAttached ? "YES" : "NO") << "\n";
	if (!info.usbBridge.empty()) {
		std::cout << "  USB Bridge Chip:    " << info.usbBridge << "\n";
	}

	if (info.knownAudioQuirks) {
		std::cout << "\n--- Audio Extraction Quirks ---\n";
		Console::Warning("  Known quirks detected:\n");
		std::cout << "  " << info.quirkDescription << "\n";
	}
	else {
		std::cout << "\n--- Audio Extraction Quirks ---\n";
		Console::Success("  No known audio extraction issues for this chipset.\n");
	}

	// Audio quality recommendation based on chipset
	std::cout << "\n--- Recommendation ---\n";
	switch (info.family) {
	case ChipsetFamily::Plextor:
	{
		Console::Success("  Excellent chipset for audio extraction.\n");
		bool qcheck = m_drive.SupportsQCheck();
		if (qcheck) {
			std::cout << "  Supports hardware C1/C2/CU scanning (Q-Check) and D8 vendor reads.\n";
		}
		else {
			std::cout << "  Supports D8 vendor reads. Q-Check (0xE9) not available on this model.\n";
		}
		break;
	}
	case ChipsetFamily::Pioneer:
		Console::Success("  Very good chipset for audio extraction.\n");
		std::cout << "  Pioneer drives are known for reliable Accurate Stream and C2.\n";
		if (m_drive.SupportsPioneerScan()) {
			std::cout << "  Supports hardware BLER/E22 scanning (0x3B/0x3C vendor commands).\n";
		}
		break;
	case ChipsetFamily::LiteOn:
		Console::Info("  Good chipset for audio extraction.\n");
		std::cout << "  MediaTek-based; supports D8 vendor reads on many models.\n";
		break;
	case ChipsetFamily::MediaTek:
		Console::Info("  Adequate chipset for audio extraction.\n");
		std::cout << "  Verify C2 accuracy with the C2 Validation Test (menu option 21).\n";
		break;
	case ChipsetFamily::Panasonic:
		Console::Warning("  Mixed results for audio extraction.\n");
		std::cout << "  Laptop/slim drives may have limited C2 accuracy. Use secure rip mode.\n";
		break;
	default:
		Console::Info("  Unknown chipset quality rating.\n");
		std::cout << "  Run C2 Validation and Drive Capabilities tests for more information.\n";
		break;
	}

	std::cout << std::string(60, '=') << "\n";
}