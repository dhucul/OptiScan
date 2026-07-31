#define NOMINMAX
#include "OpticalDrive.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <algorithm>
#include <filesystem>
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

bool OpticalDrive::VerifyWrittenArtifacts(const std::wstring& basePath, const DiscInfo& disc,
	std::vector<DWORD>& mismatchedSectors) {
	std::cout << "\n=== Verifying Written Artifacts ===\n";
	mismatchedSectors.clear();

	if (disc.rawSectors.empty()) {
		std::cout << "ERROR: No in-memory sectors are available for verification.\n";
		return false;
	}

	auto recordMismatch = [&](size_t sectorIndex) {
		const DWORD logicalIndex = static_cast<DWORD>(
			std::min<size_t>(sectorIndex, static_cast<size_t>(MAXDWORD)));
		if (std::find(mismatchedSectors.begin(), mismatchedSectors.end(), logicalIndex) ==
			mismatchedSectors.end()) {
			mismatchedSectors.push_back(logicalIndex);
		}
	};

	auto verifyArtifact = [&](const std::wstring& path, const std::vector<size_t>& sectorIndices,
		size_t sourceOffset, size_t bytesPerSector) {
		std::ifstream input(std::filesystem::path(path), std::ios::binary | std::ios::ate);
		const uint64_t expectedSize =
			static_cast<uint64_t>(sectorIndices.size()) * static_cast<uint64_t>(bytesPerSector);
		const std::streampos actualSize = input.is_open() ? input.tellg() : std::streampos(-1);
		if (!input.is_open() || actualSize < 0 || static_cast<uint64_t>(actualSize) != expectedSize) {
			std::wcout << L"ERROR: Missing or incorrectly sized artifact: " << path << L"\n";
			for (const size_t sectorIndex : sectorIndices) recordMismatch(sectorIndex);
			return false;
		}

		input.seekg(0, std::ios::beg);
		std::vector<BYTE> actual(bytesPerSector);
		bool matched = true;
		for (const size_t sectorIndex : sectorIndices) {
			if (sectorIndex >= disc.rawSectors.size() ||
				sourceOffset + bytesPerSector > disc.rawSectors[sectorIndex].size()) {
				recordMismatch(sectorIndex);
				matched = false;
				continue;
			}

			input.read(reinterpret_cast<char*>(actual.data()),
				static_cast<std::streamsize>(actual.size()));
			if (!input ||
				!std::equal(actual.begin(), actual.end(),
					disc.rawSectors[sectorIndex].begin() + sourceOffset)) {
				recordMismatch(sectorIndex);
				matched = false;
			}
		}
		return matched;
	};

	bool matched = true;
	std::vector<size_t> mainSectorIndices;
	size_t sectorIndex = 0;
	for (const auto& track : disc.tracks) {
		if (disc.selectedSession > 0 && track.session != disc.selectedSession) continue;

		DWORD start = track.pregapLBA;
		if (track.endLBA < start) continue;
		DWORD count = track.endLBA - start + 1;

		if (disc.pregapMode == PregapMode::Skip) {
			start = track.startLBA;
			if (track.endLBA < start) continue;
			count = track.endLBA - start + 1;
		}
		else if (disc.pregapMode == PregapMode::Separate && track.pregapLBA < track.startLBA) {
			const DWORD pregapCount = track.startLBA - track.pregapLBA;
			std::vector<size_t> pregapSectorIndices;
			pregapSectorIndices.reserve(pregapCount);
			for (DWORD i = 0; i < pregapCount; ++i) {
				pregapSectorIndices.push_back(sectorIndex++);
			}

			const std::wstring pregapBase = basePath + L"_track" +
				std::to_wstring(track.trackNumber) + L"_pregap";
			matched = verifyArtifact(pregapBase + L".bin", pregapSectorIndices,
				0, AUDIO_SECTOR_SIZE) && matched;
			if (disc.includeSubchannel) {
				matched = verifyArtifact(pregapBase + L".sub", pregapSectorIndices,
					AUDIO_SECTOR_SIZE, SUBCHANNEL_SIZE) && matched;
			}

			start = track.startLBA;
			if (track.endLBA < start) continue;
			count = track.endLBA - start + 1;
		}

		for (DWORD i = 0; i < count; ++i) {
			mainSectorIndices.push_back(sectorIndex++);
		}
	}

	if (sectorIndex != disc.rawSectors.size()) {
		std::cout << "ERROR: Artifact layout does not consume every in-memory sector.\n";
		for (size_t i = sectorIndex; i < disc.rawSectors.size(); ++i) recordMismatch(i);
		matched = false;
	}

	matched = verifyArtifact(basePath + L".bin", mainSectorIndices, 0, AUDIO_SECTOR_SIZE) && matched;
	if (disc.includeSubchannel) {
		matched = verifyArtifact(basePath + L".sub", mainSectorIndices,
			AUDIO_SECTOR_SIZE, SUBCHANNEL_SIZE) && matched;
	}

	std::cout << "Artifacts verified: " << (matched && mismatchedSectors.empty() ? "YES" : "NO") << "\n";
	std::cout << "Mismatched sectors: " << mismatchedSectors.size() << "\n";
	return matched && mismatchedSectors.empty();
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
