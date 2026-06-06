// ============================================================================
// ScsiDrive.Chipset.cpp - CD-ROM chipset / controller identification
//
// Identifies the drive's internal chipset family by combining:
//   1. SCSI INQUIRY vendor/model strings
//   2. Firmware revision signatures
//   3. Known model-to-chipset lookup tables
//   4. USB bridge detection via INQUIRY peripheral qualifier
//   5. Interface type probing (SATA vs IDE vs USB)
// ============================================================================
#define NOMINMAX
#include "ScsiDrive.h"
#include <algorithm>
#include <cctype>

// ── Known vendor/model -> chipset mappings ──────────────────────────────────
struct ChipsetMapping {
	const char* vendorPrefix;
	const char* modelPrefix;
	ChipsetFamily family;
	const char* chipsetName;
	bool hasAudioQuirks;
	const char* quirkDesc;
};

// NOTE: Entries with a non-empty modelPrefix MUST appear before the
// catch-all (empty modelPrefix) for the same vendor.  The first match wins.
static const ChipsetMapping knownChipsets[] = {
	// Plextor drives — custom Sanyo-derived controllers, gold standard for audio
	{ "PLEXTOR",  "PX-716",   ChipsetFamily::Plextor,   "Plextor Custom (Sanyo LC8971x)",  false, "" },
	{ "PLEXTOR",  "PX-755",   ChipsetFamily::Plextor,   "Plextor Custom (Sanyo LC8972x)",  false, "" },
	{ "PLEXTOR",  "PX-760",   ChipsetFamily::Plextor,   "Plextor Custom (Sanyo LC8972x)",  false, "" },
	{ "PLEXTOR",  "PX-712",   ChipsetFamily::Plextor,   "Plextor Custom (Sanyo LC8970x)",  false, "" },
	{ "PLEXTOR",  "PX-708",   ChipsetFamily::Plextor,   "Plextor Custom (Sanyo LC8970x)",  false, "" },
	{ "PLEXTOR",  "PX-W",     ChipsetFamily::Plextor,   "Plextor Custom (Sanyo LC897xx)",  false, "" },
	{ "PLEXTOR",  "DVDR",     ChipsetFamily::MediaTek,  "MediaTek (LiteOn OEM)",           false, "" },
	{ "PLEXTOR",  "",         ChipsetFamily::Plextor,   "Plextor Custom",                  false, "" },

	// LiteOn / PLDS — MediaTek-based since ~2006
	{ "LITE-ON",  "iHAS",     ChipsetFamily::LiteOn,    "MediaTek MT1818/MT1898",          false, "" },
	{ "LITE-ON",  "iHBS",     ChipsetFamily::LiteOn,    "MediaTek MT1959",                 false, "" },
	{ "LITE-ON",  "LH-",      ChipsetFamily::LiteOn,    "MediaTek MT1818",                 false, "" },
	{ "LITEON",   "",         ChipsetFamily::LiteOn,    "MediaTek (LiteOn)",               false, "" },
	{ "LITE-ON",  "",         ChipsetFamily::LiteOn,    "MediaTek (LiteOn)",               false, "" },
	{ "PLDS",     "",         ChipsetFamily::LiteOn,    "MediaTek (Philips-LiteOn)",       false, "" },

	// Pioneer — custom chipsets, excellent for audio extraction
	{ "PIONEER",  "BD-RW",    ChipsetFamily::Pioneer,   "Pioneer Custom (BD)",             false, "" },
	{ "PIONEER",  "BDR-",     ChipsetFamily::Pioneer,   "Pioneer Custom (BD)",             false, "" },
	{ "PIONEER",  "DVR-",     ChipsetFamily::Pioneer,   "Pioneer Custom (DVD-RW)",         false, "" },
	{ "PIONEER",  "",         ChipsetFamily::Pioneer,   "Pioneer Custom",                  false, "" },

	// ASUS — typically MediaTek-based, sometimes rebranded LiteOn
	{ "ASUS",     "BW-",      ChipsetFamily::MediaTek,  "MediaTek (ASUS OEM)",             false, "" },
	{ "ASUS",     "DRW-",     ChipsetFamily::MediaTek,  "MediaTek (ASUS OEM)",             false, "" },
	{ "ASUS",     "",         ChipsetFamily::MediaTek,  "MediaTek (ASUS OEM)",             false, "" },

	// Samsung (TSSTcorp) — MediaTek or proprietary
	{ "TSSTcorp", "CDDVDW",   ChipsetFamily::MediaTek,  "MediaTek (Samsung/Toshiba OEM)",   true,  "Some models report inaccurate C2 data" },
	{ "TSSTcorp", "BDDVDRW",  ChipsetFamily::MediaTek,  "MediaTek (Samsung/Toshiba OEM)",   true,  "Some models report inaccurate C2 data" },
	{ "TSSTcorp", "",         ChipsetFamily::MediaTek,  "MediaTek (Samsung/Toshiba OEM)",   true,  "Some models report inaccurate C2 data" },

	// Sony Optiarc — NEC/Renesas-based
	{ "SONY",     "AD-",      ChipsetFamily::NEC,       "Renesas/NEC (Sony Optiarc)",       false, "" },
	{ "SONY",     "AW-",      ChipsetFamily::NEC,       "Renesas/NEC (Sony Optiarc)",       false, "" },
	{ "SONY",     "DW-",      ChipsetFamily::NEC,       "Renesas/NEC (Sony Optiarc)",       false, "" },
	{ "SONY",     "DDU",      ChipsetFamily::Sony,      "Sony CXD (read-only)",             false, "" },
	{ "SONY",     "CDU",      ChipsetFamily::Sony,      "Sony CXD",                         false, "" },
	{ "SONY",     "",         ChipsetFamily::Sony,      "Sony Custom",                      false, "" },

	// NEC — Renesas chipsets
	{ "NEC",      "",         ChipsetFamily::NEC,       "Renesas/NEC",                      false, "" },
	{ "_NEC",     "",         ChipsetFamily::NEC,       "Renesas/NEC",                      false, "" },
	{ "OPTIARC",  "",         ChipsetFamily::NEC,       "Renesas/NEC (Optiarc)",            false, "" },

	// Panasonic / Matsushita — own controllers
	{ "MATSHITA", "",         ChipsetFamily::Panasonic,  "Panasonic MN103 Series",           true,  "Some laptop drives have limited C2 accuracy" },

	// LG (HL-DT-ST / HLDS) — specific models BEFORE the catch-all
	// Panasonic slim USB drives are sold under HL-DT-ST branding
	{ "HL-DT-ST", "DVDRAM GP", ChipsetFamily::Panasonic, "Panasonic (slim USB)",             true,  "USB slim drives may have jitter issues" },
	{ "HL-DT-ST", "BD-RE",    ChipsetFamily::MediaTek,  "MediaTek MT1959 (LG/HLDS)",       false, "" },
	{ "HL-DT-ST", "DVDRAM",   ChipsetFamily::MediaTek,  "MediaTek MT1818 (LG/HLDS)",       false, "" },
	{ "HL-DT-ST", "",         ChipsetFamily::MediaTek,  "MediaTek (LG/HLDS)",              false, "" },

	// Philips legacy
	{ "PHILIPS",  "CDD",      ChipsetFamily::Philips,   "Philips SAA78xx",                  false, "" },
	{ "PHILIPS",  "",         ChipsetFamily::Philips,   "Philips Custom",                   false, "" },

	// Ricoh
	{ "RICOH",    "",         ChipsetFamily::Ricoh,     "Ricoh Custom",                     false, "" },
};

// ── USB bridge detection strings ────────────────────────────────────────────
struct USBBridgeSignature {
	const char* vendorHint;
	const char* modelHint;
	const char* bridgeName;
};

static const USBBridgeSignature knownUSBBridges[] = {
	{ "JMicron",  "",         "JMicron JMS578" },
	{ "ASMT",     "1051",     "ASMedia ASM1051" },
	{ "ASMT",     "1153",     "ASMedia ASM1153" },
	{ "ASMT",     "1351",     "ASMedia ASM1351" },
	{ "Realtek",  "",         "Realtek RTL9210" },
	{ "VIA",      "",         "VIA VL716" },
};

// Helper: case-insensitive prefix match
static bool StartsWithCI(const std::string& str, const char* prefix) {
	size_t prefixLen = strlen(prefix);
	if (prefixLen == 0) return true;
	if (str.size() < prefixLen) return false;
	for (size_t i = 0; i < prefixLen; i++) {
		if (toupper(static_cast<unsigned char>(str[i])) !=
			toupper(static_cast<unsigned char>(prefix[i])))
			return false;
	}
	return true;
}

static bool ContainsCI(const std::string& str, const char* substr) {
	if (substr[0] == '\0') return false;  // empty pattern never matches
	std::string lower = str;
	std::string lowerSub = substr;
	std::transform(lower.begin(), lower.end(), lower.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::transform(lowerSub.begin(), lowerSub.end(), lowerSub.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return lower.find(lowerSub) != std::string::npos;
}

bool ScsiDrive::DetectChipset(ChipsetInfo& info) {
	info = ChipsetInfo{};

	// ── Step 1: SCSI INQUIRY for vendor, model, firmware ────────────────
	BYTE inqCdb[6] = { 0x12, 0, 0, 0, 96, 0 };
	std::vector<BYTE> inqBuf(96, 0);
	if (!SendSCSI(inqCdb, 6, inqBuf.data(), 96))
		return false;

	std::string vendor(reinterpret_cast<char*>(&inqBuf[8]), 8);
	std::string model(reinterpret_cast<char*>(&inqBuf[16]), 16);
	std::string firmware(reinterpret_cast<char*>(&inqBuf[32]), 4);

	// Bytes 36+ are vendor-specific; only read if the INQUIRY response
	// actually contains additional data (Additional Length in byte 4).
	std::string productRev;
	int additionalLen = inqBuf[4];
	if (additionalLen >= 32) {  // bytes 5..36+ present
		int revLen = std::min(20, additionalLen - 31);
		productRev = std::string(reinterpret_cast<char*>(&inqBuf[36]), revLen);
	}

	auto trimStr = [](std::string& s) {
		while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
			s.pop_back();
		};
	trimStr(vendor);
	trimStr(model);
	trimStr(firmware);
	trimStr(productRev);

	// ── Step 2: Detect interface type ───────────────────────────────────
	// Check for USB attachment by probing STORAGE_ADAPTER_DESCRIPTOR
	// via IOCTL_STORAGE_QUERY_PROPERTY (BusType field)
	{
		STORAGE_PROPERTY_QUERY query = {};
		query.PropertyId = StorageAdapterProperty;
		query.QueryType = PropertyStandardQuery;

		BYTE adapterBuf[256] = {};
		DWORD ret = 0;
		if (DeviceIoControl(m_handle, IOCTL_STORAGE_QUERY_PROPERTY,
			&query, sizeof(query), adapterBuf, sizeof(adapterBuf), &ret, nullptr)) {
			auto* desc = reinterpret_cast<STORAGE_ADAPTER_DESCRIPTOR*>(adapterBuf);
			switch (desc->BusType) {
			case BusTypeUsb:
				info.interfaceType = "USB";
				info.isUSBAttached = true;
				break;
			case BusTypeAta:
			case BusTypeSata:
				info.interfaceType = "SATA";
				break;
			case BusTypeAtapi:
				info.interfaceType = "IDE/ATAPI";
				break;
			case BusTypeScsi:
				info.interfaceType = "SCSI";
				break;
			default:
				info.interfaceType = "Unknown";
				break;
			}
		}
	}

	// ── Step 3: USB bridge identification ───────────────────────────────
	if (info.isUSBAttached) {
		// Query device descriptor for USB bridge vendor
		STORAGE_PROPERTY_QUERY devQuery = {};
		devQuery.PropertyId = StorageDeviceProperty;
		devQuery.QueryType = PropertyStandardQuery;

		BYTE devBuf[1024] = {};
		DWORD ret = 0;
		if (DeviceIoControl(m_handle, IOCTL_STORAGE_QUERY_PROPERTY,
			&devQuery, sizeof(devQuery), devBuf, sizeof(devBuf), &ret, nullptr)) {
			auto* devDesc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(devBuf);

			std::string busVendor, busProduct;
			if (devDesc->VendorIdOffset && devBuf[devDesc->VendorIdOffset]) {
				busVendor = reinterpret_cast<char*>(devBuf + devDesc->VendorIdOffset);
				trimStr(busVendor);
			}
			if (devDesc->ProductIdOffset && devBuf[devDesc->ProductIdOffset]) {
				busProduct = reinterpret_cast<char*>(devBuf + devDesc->ProductIdOffset);
				trimStr(busProduct);
			}

			for (const auto& bridge : knownUSBBridges) {
				bool vendorMatch = bridge.vendorHint[0] != '\0' &&
					ContainsCI(busVendor, bridge.vendorHint);
				bool modelMatch = bridge.modelHint[0] != '\0' &&
					ContainsCI(busProduct, bridge.modelHint);
				if (vendorMatch || modelMatch) {
					info.usbBridge = bridge.bridgeName;
					break;
				}
			}

			if (info.usbBridge.empty() && !busVendor.empty()) {
				info.usbBridge = busVendor + " (generic USB bridge)";
			}
		}
	}

	// ── Step 4: Match vendor/model against known chipset table ──────────
	for (const auto& mapping : knownChipsets) {
		if (StartsWithCI(vendor, mapping.vendorPrefix) &&
			StartsWithCI(model, mapping.modelPrefix)) {
			info.family = mapping.family;
			info.chipsetName = mapping.chipsetName;
			info.knownAudioQuirks = mapping.hasAudioQuirks;
			info.quirkDescription = mapping.quirkDesc;
			info.detectionMethod = "Vendor/model lookup table";
			info.confidencePercent = (strlen(mapping.modelPrefix) > 0) ? 90 : 70;
			break;
		}
	}

	// ── Step 4b: Vendor-command probing to verify/correct the table ─────
	// Send harmless vendor-specific SCSI commands and check whether the
	// drive accepts them.  This disambiguates OEM rebrands (e.g. a
	// "PLEXTOR" PX-891SAF that is actually a MediaTek/LiteOn drive).
	{
		bool acceptsPlextorD8 = false;
		bool acceptsPlextorE9 = false;
		bool acceptsLiteOnF3  = false;
		bool acceptsLiteOnDF  = false;
		bool acceptsPioneer   = false;

		// ── Probe Plextor 0xD8 (vendor read) ──
		// Genuine Plextor AND some LiteOn drives accept this, so D8 alone
		// is not conclusive — but D8-rejected + F3-accepted = MediaTek.
		{
			BYTE cdb[12] = { 0xD8, 0, 0, 0, 0, 0, 0, 0, 1, 0x02, 0, 0 };
			std::vector<BYTE> buf(SECTOR_WITH_C2_SIZE);
			BYTE sk = 0, asc = 0, ascq = 0;
			bool ok = SendSCSIWithSense(cdb, 12, buf.data(),
				static_cast<DWORD>(buf.size()), &sk, &asc, &ascq);
			acceptsPlextorD8 = ok || (sk <= 0x01);
		}

		// ── Probe Plextor 0xE9 (Q-Check init) ──
		// Only genuine Plextor PX-708/712/716/755/760 accept this.
		{
			BYTE cdb[12] = {};
			cdb[0] = 0xE9;
			cdb[1] = 0x00;
			cdb[9] = 150;
			BYTE sk = 0, asc = 0, ascq = 0;
			bool ok = SendSCSIWithSense(cdb, 12, nullptr, 0, &sk, &asc, &ascq);
			acceptsPlextorE9 = ok || (sk <= 0x01);
			// Stop any scan we may have started
			if (acceptsPlextorE9) PlextorQCheckStop();
		}

		// ── Probe LiteOn/MediaTek 0xF3 (new scan) ──
		{
			SeekToLBA(0);
			BYTE cdb[12] = {};
			cdb[0] = 0xF3;
			cdb[1] = 0x0E;
			std::vector<BYTE> buf(0x10, 0);
			BYTE sk = 0, asc = 0, ascq = 0;
			bool ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x10,
				&sk, &asc, &ascq);
			acceptsLiteOnF3 = ok || (sk <= 0x01);
		}

		// ── Probe LiteOn/MediaTek 0xDF (old scan) ──
		if (!acceptsLiteOnF3) {
			BYTE cdb[12] = {};
			cdb[0] = 0xDF;
			cdb[1] = 0xA3;
			cdb[2] = 0x00;
			BYTE sk = 0, asc = 0, ascq = 0;
			bool ok = SendSCSIWithSense(cdb, 12, nullptr, 0,
				&sk, &asc, &ascq);
			acceptsLiteOnDF = ok || (sk <= 0x01);
			// Clean up
			if (acceptsLiteOnDF) {
				BYTE stop[12] = {};
				stop[0] = 0xDF;
				stop[1] = 0xA3;
				stop[2] = 0x01;
				SendSCSI(stop, 12, nullptr, 0, false, 5);
			}
		}

		// ── Probe Pioneer 0x3C (READ BUFFER with vendor feature) ──
		{
			BYTE cdb[10] = {};
			cdb[0] = 0x3C;   // READ BUFFER
			cdb[1] = 0x02;
			cdb[2] = 0xE1;   // Pioneer vendor feature selector
			cdb[8] = 0x20;
			std::vector<BYTE> buf(32, 0);
			BYTE sk = 0, asc = 0, ascq = 0;
			bool ok = SendSCSIWithSense(cdb, 10, buf.data(), 32,
				&sk, &asc, &ascq);
			acceptsPioneer = ok || (sk <= 0x01);
		}

		// ── Resolve the chipset based on probe results ──

		// Case 1: Drive claims to be Plextor but accepts LiteOn commands
		// and does NOT accept Plextor-exclusive E9 → it's MediaTek OEM
		if (info.family == ChipsetFamily::Plextor &&
			(acceptsLiteOnF3 || acceptsLiteOnDF) && !acceptsPlextorE9) {
			info.family = ChipsetFamily::MediaTek;
			info.chipsetName = "MediaTek (LiteOn OEM, Plextor-branded)";
			info.knownAudioQuirks = false;
			info.quirkDescription.clear();
			info.detectionMethod = "Vendor command probing (LiteOn "
				+ std::string(acceptsLiteOnF3 ? "0xF3" : "0xDF")
				+ " accepted, Plextor 0xE9 rejected)";
			info.confidencePercent = 95;
		}
		// Case 2: Drive is Plextor and accepts E9 → genuine Plextor
		else if (info.family == ChipsetFamily::Plextor && acceptsPlextorE9) {
			info.detectionMethod = "Vendor command probing (Plextor 0xE9 accepted)";
			info.confidencePercent = 98;
		}
		// Case 3: Drive is Plextor, accepts D8 but not E9, no LiteOn response
		else if (info.family == ChipsetFamily::Plextor && acceptsPlextorD8 &&
			!acceptsLiteOnF3 && !acceptsLiteOnDF) {
			info.detectionMethod = "Vendor command probing (Plextor 0xD8 accepted)";
			info.confidencePercent = 85;
		}
		// Case 4: Table said LiteOn/MediaTek — confirm with probe
		else if ((info.family == ChipsetFamily::LiteOn ||
			info.family == ChipsetFamily::MediaTek) &&
			(acceptsLiteOnF3 || acceptsLiteOnDF)) {
			info.detectionMethod = "Vendor command probing (LiteOn "
				+ std::string(acceptsLiteOnF3 ? "0xF3" : "0xDF")
				+ " confirmed)";
			info.confidencePercent = std::max(info.confidencePercent, 95);
		}
		// Case 5: Table said Pioneer — confirm with probe
		else if (info.family == ChipsetFamily::Pioneer && acceptsPioneer) {
			info.detectionMethod = "Vendor command probing (Pioneer 0x3C/0xE1 confirmed)";
			info.confidencePercent = std::max(info.confidencePercent, 95);
		}
		// Case 6: Unknown drive that accepts LiteOn commands → MediaTek
		else if (info.family == ChipsetFamily::Unknown &&
			(acceptsLiteOnF3 || acceptsLiteOnDF)) {
			info.family = ChipsetFamily::MediaTek;
			info.chipsetName = "MediaTek (detected via LiteOn vendor commands)";
			info.detectionMethod = "Vendor command probing (LiteOn "
				+ std::string(acceptsLiteOnF3 ? "0xF3" : "0xDF") + " accepted)";
			info.confidencePercent = 90;
		}
		// Case 7: Unknown drive that accepts Pioneer commands → Pioneer
		else if (info.family == ChipsetFamily::Unknown && acceptsPioneer) {
			info.family = ChipsetFamily::Pioneer;
			info.chipsetName = "Pioneer Custom (detected via vendor commands)";
			info.detectionMethod = "Vendor command probing (Pioneer 0x3C/0xE1 accepted)";
			info.confidencePercent = 90;
		}
	}

	// ── Step 4c: Read Plextor TLA (hardware revision) ──────────────────
	// Only meaningful on genuine Plextor drives that accepted the E9 probe.
	if (info.family == ChipsetFamily::Plextor) {
		std::string tla;
		if (GetPlextorTLA(tla)) info.plextorTLA = tla;
	}

	// ── Step 5: Firmware-based heuristics for unknown drives ────────────
	if (info.family == ChipsetFamily::Unknown) {
		// MediaTek firmware often contains "MT" prefix or specific revision formats
		if (ContainsCI(firmware, "MT") || ContainsCI(productRev, "MT")) {
			info.family = ChipsetFamily::MediaTek;
			info.chipsetName = "MediaTek (firmware signature)";
			info.detectionMethod = "Firmware revision analysis";
			info.confidencePercent = 60;
		}
		// Renesas/NEC firmware patterns
		else if (ContainsCI(firmware, "NE") || ContainsCI(vendor, "NEC")) {
			info.family = ChipsetFamily::NEC;
			info.chipsetName = "Renesas/NEC (firmware signature)";
			info.detectionMethod = "Firmware revision analysis";
			info.confidencePercent = 55;
		}
		else {
			info.chipsetName = "Unknown (" + vendor + " " + model + ")";
			info.detectionMethod = "No match found";
			info.confidencePercent = 10;
		}
	}

	// ── Step 6: USB quirk annotations ───────────────────────────────────
	if (info.isUSBAttached && !info.knownAudioQuirks) {
		info.knownAudioQuirks = true;
		if (info.quirkDescription.empty()) {
			info.quirkDescription = "USB-attached drives may have jitter and "
				"reduced C2 accuracy due to bridge translation";
		}
	}

	return true;
}