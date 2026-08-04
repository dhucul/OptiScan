#define NOMINMAX
#include "OpticalDrive.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <filesystem>
// ... other includes as needed

namespace {
bool WideToUtf8(const std::wstring& input, std::string& output) {
	output.clear();
	if (input.empty()) return true;
	const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
	if (length <= 0) return false;
	output.resize(length);
	return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		input.data(), static_cast<int>(input.size()), output.data(), length,
		nullptr, nullptr) == length;
}
}

// ============================================================================
// File Output
// ============================================================================

bool OpticalDrive::SaveToFile(const DiscInfo& disc, const std::wstring& base) {
	struct PartialOutputCleanup {
		std::vector<std::filesystem::path> paths;
		bool committed = false;
		~PartialOutputCleanup() {
			if (committed) return;
			for (const auto& path : paths) {
				std::error_code ec;
				std::filesystem::remove(path, ec);
			}
		}
	} cleanup{ { std::filesystem::path(base + L".bin"),
		std::filesystem::path(base + L".cue") } };
	if (disc.includeSubchannel)
		cleanup.paths.push_back(std::filesystem::path(base + L".sub"));
	// Calculate and display original disc IDs for verification
	uint32_t originalDiscID1 = AccurateRip::CalculateDiscID1(disc);
	uint32_t originalDiscID2 = AccurateRip::CalculateDiscID2(disc);
	uint32_t originalCDDB = AccurateRip::CalculateCDDBID(disc);

	std::cout << "\n=== IMPORTANT: Original Disc AccurateRip IDs ===\n";
	std::cout << "These IDs are from the ORIGINAL disc TOC.\n";
	std::cout << "Burned copies will have DIFFERENT IDs but identical audio.\n";
	std::cout << "  Disc ID 1: " << std::hex << std::setfill('0')
		<< std::setw(8) << originalDiscID1 << std::dec << std::setfill(' ') << "\n";
	std::cout << "  Disc ID 2: " << std::hex << std::setfill('0')
		<< std::setw(8) << originalDiscID2 << std::dec << std::setfill(' ') << "\n";
	std::cout << "  CDDB ID:   " << std::hex << std::setfill('0')
		<< std::setw(8) << originalCDDB << std::dec << std::setfill(' ') << "\n";
	std::cout << "These IDs are saved in the .cue file for reference.\n\n";

	std::ofstream img(std::filesystem::path(base + L".bin"), std::ios::binary);
	if (!img) return false;

	std::ofstream sub;
	if (disc.includeSubchannel) {
		sub.open(std::filesystem::path(base + L".sub"), std::ios::binary);
		if (!sub) return false;
	}

	std::vector<std::wstring> pregapFiles;

	size_t sectorIdx = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		const auto& t = disc.tracks[i];
		if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;

		DWORD start = t.pregapLBA;
		if (t.endLBA < start) continue;
		DWORD count = t.endLBA - start + 1;

		if (disc.pregapMode == PregapMode::Skip) {
			start = t.startLBA;
			if (t.endLBA < start) continue;
			count = t.endLBA - start + 1;
		}
		else if (disc.pregapMode == PregapMode::Separate && t.pregapLBA < t.startLBA) {
			std::wstring pregapPath = base + L"_track" +
				std::to_wstring(t.trackNumber) + L"_pregap.bin";
			cleanup.paths.push_back(std::filesystem::path(pregapPath));
			std::ofstream pregapFile(std::filesystem::path(pregapPath), std::ios::binary);
			if (!pregapFile) return false;
			std::wstring pregapSubPath = base + L"_track" +
				std::to_wstring(t.trackNumber) + L"_pregap.sub";
			std::ofstream pregapSub;
			if (disc.includeSubchannel) {
				cleanup.paths.push_back(std::filesystem::path(pregapSubPath));
				pregapSub.open(std::filesystem::path(pregapSubPath), std::ios::binary);
				if (!pregapSub) return false;
			}

			DWORD pregapCount = t.startLBA - t.pregapLBA;
			for (DWORD j = 0; j < pregapCount; j++) {
				if (sectorIdx >= disc.rawSectors.size()) return false;
				const auto& s = disc.rawSectors[sectorIdx++];
				if (s.size() < AUDIO_SECTOR_SIZE) return false;
				pregapFile.write(reinterpret_cast<const char*>(s.data()), AUDIO_SECTOR_SIZE);
				if (!pregapFile) return false;
				if (disc.includeSubchannel) {
					if (s.size() < RAW_SECTOR_SIZE) return false;
					pregapSub.write(reinterpret_cast<const char*>(
						s.data() + AUDIO_SECTOR_SIZE), SUBCHANNEL_SIZE);
					if (!pregapSub) return false;
				}
			}
			pregapFile.flush();
			if (!pregapFile.good()) return false;
			pregapFiles.push_back(pregapPath);
			if (disc.includeSubchannel) {
				pregapSub.flush();
				if (!pregapSub.good()) return false;
				pregapFiles.push_back(pregapSubPath);
			}
			start = t.startLBA;
			if (t.endLBA < t.startLBA) continue;
			count = t.endLBA - t.startLBA + 1;
		}

		for (DWORD j = 0; j < count; j++) {
			if (sectorIdx >= disc.rawSectors.size()) return false;
			const auto& s = disc.rawSectors[sectorIdx++];
			if (s.size() < AUDIO_SECTOR_SIZE) return false;
			img.write(reinterpret_cast<const char*>(s.data()), AUDIO_SECTOR_SIZE);
			if (!img) return false;

			if (disc.includeSubchannel) {
				if (s.size() < RAW_SECTOR_SIZE) return false;
				sub.write(reinterpret_cast<const char*>(s.data() + AUDIO_SECTOR_SIZE), SUBCHANNEL_SIZE);
				if (!sub) return false;
			}
		}
	}

	if (sectorIdx != disc.rawSectors.size()) return false;
	img.flush();
	if (!img.good()) return false;
	if (disc.includeSubchannel) {
		sub.flush();
		if (!sub.good()) return false;
	}

	std::string fn;
	if (!WideToUtf8(base, fn)) return false;
	size_t p = fn.find_last_of("/\\");
	if (p != std::string::npos) fn = fn.substr(p + 1);

	std::ofstream cue(std::filesystem::path(base + L".cue"));
	if (!cue) return false;

	if (!disc.cdText.albumArtist.empty()) {
		cue << "PERFORMER \"" << disc.cdText.albumArtist << "\"\n";
	}
	if (!disc.cdText.albumTitle.empty()) {
		cue << "TITLE \"" << disc.cdText.albumTitle << "\"\n";
	}

	if (disc.includeSubchannel) cue << "REM Subchannel in " << fn << ".sub\n";

	// AccurateRip disc identification
	cue << "REM DISCID " << std::hex << std::setfill('0')
		<< std::setw(8) << AccurateRip::CalculateCDDBID(disc) << std::dec << "\n";
	cue << "REM ACCURATERIPID " << std::hex << std::setfill('0')
		<< std::setw(8) << AccurateRip::CalculateDiscID1(disc) << "-"
		<< std::setw(8) << AccurateRip::CalculateDiscID2(disc) << std::dec << "\n";

	// Media Catalog Number (UPC/EAN). A valid MCN is exactly 13 digits; emit it
	// as the standard CUE CATALOG command so it round-trips on a future raw-DAO
	// burn (and is preserved as image metadata otherwise).
	if (disc.mcn.length() == 13) {
		cue << "CATALOG " << disc.mcn << "\n";
	}

	cue << "FILE \"" << fn << ".bin\" BINARY\n";

	DWORD off = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		const auto& t = disc.tracks[i];
		if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;

		cue << "  TRACK " << std::setfill('0') << std::setw(2) << t.trackNumber;
		cue << (t.isAudio ? " AUDIO\n" :
			(t.mode == 2 ? " MODE2/2352\n" : " MODE1/2352\n"));

		if (t.trackNumber > 0 &&
			static_cast<size_t>(t.trackNumber) <= disc.cdText.trackTitles.size() &&
			!disc.cdText.trackTitles[t.trackNumber - 1].empty()) {
			cue << "    TITLE \"" << disc.cdText.trackTitles[t.trackNumber - 1] << "\"\n";
		}
		if (t.trackNumber > 0 &&
			static_cast<size_t>(t.trackNumber) <= disc.cdText.trackArtists.size() &&
			!disc.cdText.trackArtists[t.trackNumber - 1].empty()) {
			cue << "    PERFORMER \"" << disc.cdText.trackArtists[t.trackNumber - 1] << "\"\n";
		}

		if (!t.isrc.empty()) {
			cue << "    ISRC " << t.isrc << "\n";
		}

		if (t.hasPreemphasis) {
			cue << "    FLAGS PRE\n";
		}

		DWORD gap = 0;
		if (t.startLBA > t.pregapLBA)
			gap = t.startLBA - t.pregapLBA;

		if (disc.pregapMode == PregapMode::Include) {
			if (gap > 0) {
				cue << "    INDEX 00 " << std::setfill('0') << std::setw(2) << off / 75 / 60 << ":"
					<< std::setw(2) << (off / 75) % 60 << ":" << std::setw(2) << off % 75 << "\n";
				off += gap;
			}
			cue << "    INDEX 01 " << std::setfill('0') << std::setw(2) << off / 75 / 60 << ":"
				<< std::setw(2) << (off / 75) % 60 << ":" << std::setw(2) << off % 75 << "\n";
			if (t.endLBA >= t.startLBA)
				off += t.endLBA - t.startLBA + 1;
		}
		else {
			if (gap > 0 && i > 0) {
				cue << "    PREGAP " << std::setfill('0') << std::setw(2) << gap / 75 / 60 << ":"
					<< std::setw(2) << (gap / 75) % 60 << ":" << std::setw(2) << gap % 75 << "\n";
			}
			cue << "    INDEX 01 " << std::setfill('0') << std::setw(2) << off / 75 / 60 << ":"
				<< std::setw(2) << (off / 75) % 60 << ":" << std::setw(2) << off % 75 << "\n";
			if (t.endLBA >= t.startLBA)
				off += t.endLBA - t.startLBA + 1;
		}
	}

	cue.flush();
	if (!cue.good()) return false;

	std::cout << "\n=== Files Created ===\n";
	std::wcout << L"  " << base << L".bin\n";
	if (disc.includeSubchannel) std::wcout << L"  " << base << L".sub\n";
	std::wcout << L"  " << base << L".cue\n";

	for (const auto& pf : pregapFiles) {
		std::wcout << L"  " << pf << L"\n";
	}

	cleanup.committed = true;
	return true;
}

bool OpticalDrive::SaveBlerLog(const BlerResult& result, const std::wstring& filename) {
	std::ofstream log(std::filesystem::path(filename), std::ios::out | std::ios::trunc);
	if (!log) return false;

	// --- Summary section (commented for CSV parsers, readable for humans) ---
	log << "# ==============================\n";
	log << "# BLER Quality Scan Log\n";
	log << "# ==============================\n";
	log << "#\n";
	log << "# Quality Rating:        ";
	if (result.c2Unverified && !result.hasC1Data)
		log << "INCOMPLETE (no trustworthy C2 measurement)";
	else {
		log << result.qualityRating;
		if (result.c2Unverified)
			log << " (C1 quality only; C2 is not verified)";
	}
	log << "\n";
	log << "# Measurement Method:    "
		<< (result.measurementMethod.empty() ? "unspecified" : result.measurementMethod) << "\n";
	log << "# C2 Measurement:        "
		<< (result.c2Unverified ? "NOT VERIFIED / NOT MEASURED" : "MEASURED") << "\n";
	log << "# Total Sectors:         " << result.totalSectors << "\n";
	log << "# Disc Length:           "
		<< (result.totalSeconds / 60) << ":"
		<< std::setfill('0') << std::setw(2) << (result.totalSeconds % 60)
		<< std::setfill(' ') << " (mm:ss)\n";
	log << "#\n";
	log << "# --- Error Statistics ---\n";
	if (result.hasC1Data) {
		log << "# Total C1 Errors:       " << result.totalC1Errors << "\n";
		log << "# Avg C1/sec:            " << std::fixed << std::setprecision(2)
			<< result.avgC1PerSecond << "\n";
		log << "# Max C1/sec:            " << result.maxC1PerSecond << "\n";
	}
	if (result.c2Unverified) {
		log << "# Total C2 Errors:       N/A (not measured)\n";
		log << "# Sectors with C2:       N/A (not measured)\n";
		log << "# Avg C2/sec:            N/A (not measured)\n";
		log << "# Max C2/sec:            N/A (not measured)\n";
		log << "# Max C2 in One Sector:  N/A (not measured)\n";
		log << "# Longest Error Run:     N/A (not measured)\n";
	}
	else {
		log << "# Total C2 Errors:       " << result.totalC2Errors << " bits\n";
		log << "# Sectors with C2:       " << result.totalC2Sectors;
		if (result.totalSectors > 0)
			log << " (" << std::fixed << std::setprecision(3)
			<< (result.totalC2Sectors * 100.0 / result.totalSectors) << "%)";
		log << "\n";
		log << "# Avg C2/sec:            " << std::fixed << std::setprecision(2)
			<< result.avgC2PerSecond << "\n";
		log << "# Max C2/sec:            " << result.maxC2PerSecond << "\n";
		log << "# Max C2 in One Sector:  " << result.maxC2InSingleSector;
		if (result.maxC2InSingleSector > 0) log << " (LBA " << result.worstSectorLBA << ")";
		log << "\n";
		log << "# Longest Error Run:     " << result.consecutiveErrorSectors << " sectors\n";
	}
	log << "# Read Failures:         " << result.totalReadFailures << "\n";
	if (result.pioneerVendorQuality) {
		log << "# Pioneer E22 Total:     " << result.pioneerE22Total << " (diagnostic; not C2)\n";
		log << "# Pioneer E22 Avg/sec:   " << std::fixed << std::setprecision(2)
			<< result.pioneerE22AvgPerSecond << "\n";
		log << "# Pioneer E22 Peak/sec:  " << result.pioneerE22Peak << "\n";
		log << "# Pioneer E22 Rating:    " << result.pioneerE22Rating << "\n";
		if (result.pioneerCdCheckRun) {
			log << "# Uncorrectable CD Check: "
				<< (result.pioneerCdCheckC2Bytes > 0 ? "YES - DATA LOSS" : "NO")
				<< " (C1 uncorr=" << result.pioneerCdCheckC1Frames
				<< " frames, C2 uncorr=" << result.pioneerCdCheckC2Bytes
				<< " bytes, worst window)\n";
		}
		else {
			log << "# Uncorrectable CD Check: NOT MEASURED\n";
		}
	}
	log << "#\n";
	log << "# --- Red Book Compliance ---\n";
	if (result.c2Unverified)
		log << "# C2 Result:             N/A - no verified C2 measurement\n";
	else
		log << "# C2 Result:             "
			<< ((result.totalC2Errors == 0 && result.totalReadFailures == 0) ? "PASS" : "FAIL") << "\n";
	log << "#\n";

	// --- Zone stats ---
	log << "# --- Zone Error Rates ---\n";
	if (result.c2Unverified) {
		log << "# N/A - verified C2 zone data was not measured.\n#\n";
	}
	else {
	log << "# Inner  (0-33%):        " << std::fixed << std::setprecision(2)
		<< result.zoneStats.InnerErrorRate() << "% ("
		<< result.zoneStats.innerErrors << "/" << result.zoneStats.innerSectors << ")\n";
	log << "# Middle (33-66%):       " << std::fixed << std::setprecision(2)
		<< result.zoneStats.MiddleErrorRate() << "% ("
		<< result.zoneStats.middleErrors << "/" << result.zoneStats.middleSectors << ")\n";
	log << "# Outer  (66-100%):      " << std::fixed << std::setprecision(2)
		<< result.zoneStats.OuterErrorRate() << "% ("
		<< result.zoneStats.outerErrors << "/" << result.zoneStats.outerSectors << ")\n";
	log << "#\n";
	}

	// --- Error clusters ---
	if (!result.errorClusters.empty()) {
		log << "# --- Error Clusters (" << result.errorClusters.size() << " total) ---\n";
		log << "# Largest Cluster: " << result.largestClusterSize << " sectors\n";
		log << "# Edge Concentration: " << (result.hasEdgeConcentration ? "YES" : "NO") << "\n";
		log << "# Progressive Pattern: " << (result.hasProgressivePattern ? "YES" : "NO") << "\n";
		log << "#\n";
		log << "# ClusterIndex,StartLBA,EndLBA,SectorCount,ErrorCount\n";
		for (size_t i = 0; i < result.errorClusters.size(); i++) {
			const auto& c = result.errorClusters[i];
			log << "# " << i << "," << c.startLBA << "," << c.endLBA
				<< "," << c.size() << "," << c.errorCount << "\n";
		}
		log << "#\n";
	}

	// --- Per-second CSV data ---
	log << "# ==============================\n";
	log << (result.c2Unverified ? "# Per-Second Measured Quality Data\n" : "# Per-Second C2 Error Data\n");
	log << "# ==============================\n";
	if (result.c2Unverified) {
		if (result.pioneerVendorQuality) {
			log << "Time,Second,LBA,C1,PioneerE22\n";
			size_t count = std::max(result.perSecondC1.size(), result.perSecondPioneerE22.size());
			for (size_t i = 0; i < count; i++) {
				DWORD lba = i < result.perSecondC1.size() ? result.perSecondC1[i].first
					: result.perSecondPioneerE22[i].first;
				int c1 = i < result.perSecondC1.size() ? result.perSecondC1[i].second : 0;
				int e22 = i < result.perSecondPioneerE22.size() ? result.perSecondPioneerE22[i].second : 0;
				log << (i / 60) << ":" << std::setfill('0') << std::setw(2) << (i % 60)
					<< std::setfill(' ') << "," << i << "," << lba << "," << c1 << "," << e22 << "\n";
			}
		}
		else if (!result.perSecondC1.empty()) {
			log << "Time,Second,LBA,C1\n";
			for (size_t i = 0; i < result.perSecondC1.size(); i++)
				log << (i / 60) << ":" << std::setfill('0') << std::setw(2) << (i % 60)
					<< std::setfill(' ') << "," << i << "," << result.perSecondC1[i].first
					<< "," << result.perSecondC1[i].second << "\n";
		}
		else {
			log << "# No verified per-second C2 data is available.\n";
		}
		log.flush();
		return log.good();
	}
	log << "# (only seconds with non-zero errors)\n";

	// Count how many seconds have errors
	bool hasErrors = false;
	for (size_t i = 0; i < result.perSecondC2.size(); i++) {
		if (result.perSecondC2[i].second > 0) {
			hasErrors = true;
			break;
		}
	}

	if (hasErrors) {
		log << "Time,Second,LBA,C2_Errors\n";

		for (size_t i = 0; i < result.perSecondC2.size(); i++) {
			int c2 = result.perSecondC2[i].second;
			if (c2 == 0) continue;  // skip clean seconds

			int minutes = static_cast<int>(i) / 60;
			int seconds = static_cast<int>(i) % 60;
			DWORD lba = static_cast<DWORD>(result.perSecondC2[i].first);

			log << minutes << ":" << std::setfill('0') << std::setw(2) << seconds
				<< std::setfill(' ')
				<< "," << i
				<< "," << lba
				<< "," << c2 << "\n";
		}
	}
	else {
		log << "# No errors detected. All " << result.perSecondC2.size()
			<< " seconds read cleanly with zero C2 errors.\n";
	}

	log.flush();
	return log.good();
}

bool OpticalDrive::SaveReadLog(const DiscInfo& disc, const std::wstring& filename) {
	if (disc.readLog.empty()) {
		return false;
	}

	std::ofstream log(std::filesystem::path(filename), std::ios::out | std::ios::trunc);
	if (!log) {
		return false;
	}

	log << "# Sector Read Log\n";
	log << "# Format: LBA,Track,ReadTime(ms)\n";
	log << "LBA,Track,ReadTimeMs\n";

	for (const auto& entry : disc.readLog) {
		log << std::get<0>(entry) << ","
			<< std::get<1>(entry) << ","
			<< std::fixed << std::setprecision(2) << std::get<2>(entry) << "\n";
	}

	log.flush();
	return log.good();
}

bool OpticalDrive::GenerateCueSheet(const DiscInfo& disc, const std::wstring& audioFilePath,
	const std::wstring& cueOutputPath) {
	std::ofstream cue(std::filesystem::path(cueOutputPath), std::ios::out | std::ios::trunc);
	if (!cue) return false;

	std::string fn;
	if (!WideToUtf8(audioFilePath, fn)) return false;
	size_t p = fn.find_last_of("/\\");
	if (p != std::string::npos) fn = fn.substr(p + 1);

	if (!disc.cdText.albumArtist.empty()) {
		cue << "PERFORMER \"" << disc.cdText.albumArtist << "\"\n";
	}
	if (!disc.cdText.albumTitle.empty()) {
		cue << "TITLE \"" << disc.cdText.albumTitle << "\"\n";
	}

	if (disc.mcn.length() == 13) {
		cue << "CATALOG " << disc.mcn << "\n";
	}

	cue << "FILE \"" << fn << "\" WAVE\n";

	DWORD off = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		const auto& t = disc.tracks[i];
		cue << "  TRACK " << std::setfill('0') << std::setw(2) << t.trackNumber;
		cue << (t.isAudio ? " AUDIO\n" : " MODE1/2352\n");

		if (t.trackNumber > 0 &&
			static_cast<size_t>(t.trackNumber) <= disc.cdText.trackTitles.size() &&
			!disc.cdText.trackTitles[t.trackNumber - 1].empty()) {
			cue << "    TITLE \"" << disc.cdText.trackTitles[t.trackNumber - 1] << "\"\n";
		}
		if (t.trackNumber > 0 &&
			static_cast<size_t>(t.trackNumber) <= disc.cdText.trackArtists.size() &&
			!disc.cdText.trackArtists[t.trackNumber - 1].empty()) {
			cue << "    PERFORMER \"" << disc.cdText.trackArtists[t.trackNumber - 1] << "\"\n";
		}

		if (!t.isrc.empty()) {
			cue << "    ISRC " << t.isrc << "\n";
		}

		if (t.hasPreemphasis) {
			cue << "    FLAGS PRE\n";
		}

		DWORD gap = 0;
		if (t.startLBA > t.pregapLBA)
			gap = t.startLBA - t.pregapLBA;

		if (disc.pregapMode == PregapMode::Include) {
			if (gap > 0) {
				cue << "    INDEX 00 " << std::setfill('0') << std::setw(2) << off / 75 / 60 << ":"
					<< std::setw(2) << (off / 75) % 60 << ":" << std::setw(2) << off % 75 << "\n";
				off += gap;
			}
			cue << "    INDEX 01 " << std::setfill('0') << std::setw(2) << off / 75 / 60 << ":"
				<< std::setw(2) << (off / 75) % 60 << ":" << std::setw(2) << off % 75 << "\n";
			if (t.endLBA >= t.startLBA)
				off += t.endLBA - t.startLBA + 1;
		}
		else {
			if (gap > 0 && i > 0) {
				cue << "    PREGAP " << std::setfill('0') << std::setw(2) << gap / 75 / 60 << ":"
					<< std::setw(2) << (gap / 75) % 60 << ":" << std::setw(2) << gap % 75 << "\n";
			}
			cue << "    INDEX 01 " << std::setfill('0') << std::setw(2) << off / 75 / 60 << ":"
				<< std::setw(2) << (off / 75) % 60 << ":" << std::setw(2) << off % 75 << "\n";
			if (t.endLBA >= t.startLBA)
				off += t.endLBA - t.startLBA + 1;
		}
	}

	cue.close();
	return cue.good();
}

bool OpticalDrive::SaveSecureRipLog(const SecureRipResult& result, const std::wstring& filename) {
	const auto& log = result.log;
	if (log.entries.empty() && log.phaseStats.empty()) {
		return false;
	}

	std::ofstream out(std::filesystem::path(filename), std::ios::out | std::ios::trunc);
	if (!out) return false;

	// Header with configuration
	out << "# Secure Rip Log\n";
	out << "# Mode: " << log.modeName << "\n";
	out << "# Passes: " << log.minPasses << "-" << log.maxPasses
		<< ", Required matches: " << log.requiredMatches << "\n";
	out << "# C2 detection: " << (log.useC2 ? "YES" : "NO")
		<< ", Cache defeat: " << (log.cacheDefeat ? "YES" : "NO") << "\n";
	out << "#\n";

	// Overall summary
	out << "# === Summary ===\n";
	out << "# Total sectors:    " << log.totalSectors << "\n";
	out << "# Verified:         " << log.totalVerified
		<< " (" << std::fixed << std::setprecision(1)
		<< (log.totalSectors > 0 ? 100.0 * log.totalVerified / log.totalSectors : 0)
		<< "%)\n";
	out << "# Unsecure:         " << log.totalUnsecure << "\n";
	out << "# Total C2 errors:  " << log.totalC2Errors << "\n";
	out << "# Total duration:   " << std::fixed << std::setprecision(1)
		<< log.totalDurationSeconds << "s\n";
	out << "# Quality:          " << result.qualityAssessment << "\n";
	out << "# Confidence:       " << std::fixed << std::setprecision(1)
		<< result.securityConfidence << "%\n";
	if (result.byteRecoveredSectors > 0 || result.partialSectors > 0) {
		out << "# Byte-recovered:   " << result.byteRecoveredSectors
			<< " full, " << result.partialSectors << " partial, "
			<< result.unconfirmedBytes << " unconfirmed byte(s)\n";
		out << "# C2-disputed:      " << result.c2DisputedBytes
			<< " byte(s) across " << result.c2DisputedSectors << " sector(s)\n";
	}
	out << "#\n";

	// Per-phase breakdown
	out << "# === Phase Breakdown ===\n";
	for (const auto& ps : log.phaseStats) {
		out << "# Phase " << ps.phase << ": "
			<< ps.sectorsProcessed << " processed, "
			<< ps.sectorsVerified << " verified, "
			<< ps.sectorsFailed << " failed, "
			<< std::fixed << std::setprecision(1) << ps.durationSeconds << "s"
			<< " (avg " << std::setprecision(2) << ps.avgReadTimeMs << "ms/sector)\n";
	}
	out << "#\n";

	// Sector-level CSV
	out << "LBA,Track,Phase,Passes,Matches,C2Errors,ReadTimeMs,Verified,Hash,"
		<< "RescuePasses,BytesConfirmed,C2DisputedBytes\n";

	for (const auto& e : log.entries) {
		out << e.lba << ","
			<< e.track << ","
			<< e.phase << ","
			<< e.passesUsed << ","
			<< e.matchCount << ","
			<< e.c2Errors << ","
			<< std::fixed << std::setprecision(2) << e.readTimeMs << ","
			<< (e.verified ? "YES" : "NO") << ","
			<< std::hex << std::setfill('0') << std::setw(8) << e.hash
			<< std::dec << std::setfill(' ') << ","
			<< e.rescuePasses << ","
			<< e.bytesConfirmed << ","
			<< e.c2DisputedBytes << "\n";
	}

	out.flush();
	return out.good();
}
