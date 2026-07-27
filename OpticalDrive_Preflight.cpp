#define NOMINMAX
#include "OpticalDrive.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <fstream>
#include <iomanip>
// ... other includes as needed

bool OpticalDrive::ValidateDiscStructure(const DiscInfo& disc, std::vector<std::string>& issues) {
	issues.clear();

	if (disc.tracks.empty()) {
		issues.push_back("No tracks found on disc");
		return false;
	}

	for (size_t i = 0; i < disc.tracks.size(); i++) {
		const auto& t = disc.tracks[i];
		if (t.endLBA < t.startLBA) {
			issues.push_back("Track " + std::to_string(t.trackNumber) + " has invalid LBA range");
		}
		if (i > 0 && t.startLBA <= disc.tracks[i - 1].endLBA) {
			issues.push_back("Track " + std::to_string(t.trackNumber) + " overlaps with previous track");
		}
	}

	return issues.empty();
}

bool OpticalDrive::VerifyWrittenFile(const std::wstring& filename, const DiscInfo& disc,
	std::vector<DWORD>& mismatchedSectors) {
	std::cout << "\n=== Verifying Written File ===\n";
	mismatchedSectors.clear();

	std::ifstream file(filename, std::ios::binary);
	if (!file) {
		std::cout << "ERROR: Cannot open file for verification.\n";
		return false;
	}

	file.seekg(0, std::ios::end);
	std::streamsize fileSize = file.tellg();
	file.seekg(0, std::ios::beg);

	size_t expectedSectors = disc.rawSectors.size();
	size_t expectedSize = expectedSectors * AUDIO_SECTOR_SIZE;
	if (expectedSectors == 0) {
		std::cout << "ERROR: No in-memory sectors are available for verification.\n";
		return false;
	}

	if (fileSize < 0 || static_cast<uint64_t>(fileSize) != expectedSize) {
		std::cout << "WARNING: File size mismatch. Expected: " << expectedSize
			<< ", Actual: " << fileSize << "\n";
		return false;
	}

	std::cout << "Verifying " << expectedSectors << " sectors...\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Verify");
	progress.Start();

	std::vector<BYTE> fileSector(AUDIO_SECTOR_SIZE);
	DWORD sectorNum = 0;
	bool readFailed = false;

	for (size_t i = 0; i < disc.rawSectors.size(); i++) {
		if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
			progress.Finish(false);
			return false;
		}

		file.read(reinterpret_cast<char*>(fileSector.data()), AUDIO_SECTOR_SIZE);
		if (!file) {
			std::cout << "\nERROR: Read error at sector " << sectorNum << "\n";
			readFailed = true;
			break;
		}

		const auto& origSector = disc.rawSectors[i];
		if (origSector.size() < AUDIO_SECTOR_SIZE) {
			std::cout << "\nERROR: In-memory sector " << sectorNum
				<< " is shorter than an audio sector\n";
			readFailed = true;
			break;
		}

		if (memcmp(fileSector.data(), origSector.data(), AUDIO_SECTOR_SIZE) != 0) {
			mismatchedSectors.push_back(sectorNum);
		}

		sectorNum++;
		progress.Update(static_cast<int>(sectorNum), static_cast<int>(expectedSectors));
	}

	const bool complete = !readFailed &&
		sectorNum == expectedSectors &&
		mismatchedSectors.empty();
	progress.Finish(complete);
	file.close();

	std::cout << "\n=== Verification Results ===\n";
	std::cout << "Sectors verified: " << sectorNum << "\n";
	std::cout << "Mismatches: " << mismatchedSectors.size() << "\n";

	if (complete) {
		std::cout << "*** FILE VERIFIED SUCCESSFULLY ***\n";
		return true;
	}
	else {
		std::cout << "*** VERIFICATION FAILED ***\n";
		if (mismatchedSectors.size() <= 10) {
			std::cout << "Mismatched sectors: ";
			for (DWORD lba : mismatchedSectors) {
				std::cout << lba << " ";
			}
			std::cout << "\n";
		}
		return false;
	}
}

bool OpticalDrive::CheckDiskSpace(const std::wstring& path, DWORD sectorsNeeded) {
	ULARGE_INTEGER freeBytes;
	if (GetDiskFreeSpaceExW(path.c_str(), &freeBytes, nullptr, nullptr)) {
		ULONGLONG needed = static_cast<ULONGLONG>(sectorsNeeded) * AUDIO_SECTOR_SIZE;
		return freeBytes.QuadPart >= needed;
	}
	return true;
}

bool OpticalDrive::RunPreflightChecks(DiscInfo& disc, std::vector<std::string>& warnings) {
	warnings.clear();

	std::vector<std::string> structureIssues;
	if (!ValidateDiscStructure(disc, structureIssues)) {
		warnings.insert(warnings.end(), structureIssues.begin(), structureIssues.end());
	}

	return warnings.empty();
}
