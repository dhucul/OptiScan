#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleColors.h"
#include "WriteDiscInternal.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <windows.h>

// ============================================================================
// Helper: Convert wide string to UTF-8 string (Windows API)
// ============================================================================
static std::string WideToUTF8(const std::wstring& wide) {
	if (wide.empty()) return std::string();
	int requiredSize = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
	if (requiredSize == 0) return std::string();

	std::string utf8(requiredSize, '\0');
	if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		wide.data(), static_cast<int>(wide.size()), utf8.data(), requiredSize,
		nullptr, nullptr) == 0) return std::string();
	return utf8;
}

static bool UTF8ToWide(const std::string& utf8, std::wstring& wide) {
	wide.clear();
	if (utf8.empty()) return true;
	const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (required <= 0) return false;
	wide.resize(required);
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		utf8.data(), static_cast<int>(utf8.size()), wide.data(), required) == required;
}

// ============================================================================
// Helper: Trim trailing whitespace and carriage returns from a wide string
// ============================================================================
static void TrimTrailing(std::wstring& s) {
	size_t end = s.find_last_not_of(L" \t\r\n");
	if (end != std::wstring::npos)
		s.erase(end + 1);
	else
		s.clear();
}

// ============================================================================
// ParseCueSheet - Parse CUE file to extract track information with pregaps
// ============================================================================
bool OpticalDrive::ParseCueSheet(const std::wstring& cueFile,
	std::vector<TrackWriteInfo>& tracks) {
	std::string discTitle, discPerformer, discMCN;
	return ParseCueSheet(cueFile, tracks, discTitle, discPerformer, discMCN);
}

bool OpticalDrive::ParseCueSheet(const std::wstring& cueFile,
	std::vector<TrackWriteInfo>& tracks,
	std::string& discTitle, std::string& discPerformer,
	std::string& discMCN) {

	std::ifstream rawFile(cueFile, std::ios::binary);
	if (!rawFile.is_open()) {
		Console::Error("Cannot open CUE file: ");
		std::wcout << cueFile << L"\n";
		return false;
	}
	std::string cueBytes((std::istreambuf_iterator<char>(rawFile)),
		std::istreambuf_iterator<char>());
	if (cueBytes.size() >= 3 && static_cast<unsigned char>(cueBytes[0]) == 0xEF &&
		static_cast<unsigned char>(cueBytes[1]) == 0xBB &&
		static_cast<unsigned char>(cueBytes[2]) == 0xBF) {
		cueBytes.erase(0, 3);
	}
	std::wstring cueText;
	if (!UTF8ToWide(cueBytes, cueText)) {
		Console::Error("CUE sheet is not valid UTF-8\n");
		return false;
	}
	std::wistringstream file(cueText);

	discTitle.clear();
	discPerformer.clear();
	discMCN.clear();
	tracks.clear();

	std::wstring line;
	TrackWriteInfo currentTrack = {};
	currentTrack.hasPregap = false;
	bool inTrack = false;
	int fileCount = 0;

	auto extractQuoted = [](const std::wstring& ln, const std::wstring& keyword) -> std::string {
		size_t pos = ln.find(keyword);
		if (pos == std::wstring::npos) return {};
		size_t q1 = ln.find(L'"', pos + keyword.size());
		if (q1 == std::wstring::npos) return {};
		size_t q2 = ln.find(L'"', q1 + 1);
		if (q2 == std::wstring::npos) return {};
		std::wstring val = ln.substr(q1 + 1, q2 - q1 - 1);
		return WideToUTF8(val);
		};

	while (std::getline(file, line)) {
		// Trim leading whitespace
		size_t start = line.find_first_not_of(L" \t");
		if (start == std::wstring::npos) continue;
		line = line.substr(start);

		// Trim trailing whitespace and stray CR (handles mixed line endings)
		TrimTrailing(line);
		if (line.empty()) continue;

		if (line.find(L"FILE") == 0) {
			fileCount++;
			if (fileCount > 1) {
				Console::Error("Multi-file CUE sheets are not supported\n");
				tracks.clear();
				return false;
			}
		}
		else if (line.find(L"CATALOG") == 0 && !inTrack) {
			// CATALOG specifies the disc's 13-digit Media Catalog Number (EAN/UPC)
			std::wistringstream iss(line);
			std::wstring cmd, catalog;
			iss >> cmd >> catalog;
			discMCN = WideToUTF8(catalog);
		}
		else if (line.find(L"TRACK") == 0) {
			if (inTrack && currentTrack.trackNumber > 0) {
				tracks.push_back(currentTrack);
			}
			inTrack = true;
			currentTrack = {};
			currentTrack.hasPregap = false;
			currentTrack.dataMode = 0;

			if (line.find(L"AUDIO") != std::wstring::npos) {
				currentTrack.isAudio = true;
				currentTrack.dataMode = 0;
			}
			else if (line.find(L"MODE1/2352") != std::wstring::npos) {
				currentTrack.isAudio = false;
				currentTrack.dataMode = 1;
			}
			else if (line.find(L"MODE2/2352") != std::wstring::npos) {
				currentTrack.isAudio = false;
				currentTrack.dataMode = 2;
			}
			else {
				Console::Warning("Unsupported track mode in CUE: ");
				std::wcout << line << L"\n";
				Console::Info("Only AUDIO, MODE1/2352, and MODE2/2352 are supported\n");
				tracks.clear();
				return false;
			}

			std::wistringstream iss(line);
			std::wstring cmd;
			iss >> cmd >> currentTrack.trackNumber;
		}
		else if (line.find(L"TITLE") == 0) {
			std::string val = extractQuoted(line, L"TITLE");
			if (!val.empty()) {
				if (inTrack)
					currentTrack.title = val;
				else
					discTitle = val;
			}
		}
		else if (line.find(L"PERFORMER") == 0) {
			std::string val = extractQuoted(line, L"PERFORMER");
			if (!val.empty()) {
				if (inTrack)
					currentTrack.performer = val;
				else
					discPerformer = val;
			}
		}
		else if (line.find(L"INDEX") == 0 && inTrack) {
			int indexNum = 0;
			std::wstring timeStr;
			std::wistringstream iss(line);
			std::wstring cmd;
			iss >> cmd >> indexNum >> timeStr;

			// Ignore INDEX 00 for data tracks (pregap is not applicable)
			if (!currentTrack.isAudio && indexNum == 0) continue;

			int mm = 0, ss = 0, ff = 0;
			wchar_t sep;
			std::wistringstream tss(timeStr);
			tss >> mm >> sep >> ss >> sep >> ff;

			DWORD lba = mm * 60 * 75 + ss * 75 + ff;

			if (indexNum == 0) {
				currentTrack.pregapLBA = lba;
				currentTrack.hasPregap = true;
			}
			else if (indexNum == 1) {
				currentTrack.startLBA = lba;
			}
		}
		else if (line.find(L"ISRC") == 0 && inTrack) {
			std::wistringstream iss(line);
			std::wstring cmd, isrc;
			iss >> cmd >> isrc;
			currentTrack.isrcCode = WideToUTF8(isrc);
		}
		else if (line.find(L"PREGAP") == 0 && inTrack) {
			// PREGAP generates silence not present in the BIN file.
			// This is different from INDEX 00, which marks an existing region.
			Console::Warning("PREGAP command detected but not supported (track ");
			std::cout << currentTrack.trackNumber
				<< ") -- only INDEX 00 pregaps are handled\n";
		}
		else if (line.find(L"POSTGAP") == 0 && inTrack) {
			Console::Warning("POSTGAP command detected but not supported (track ");
			std::cout << currentTrack.trackNumber << ")\n";
		}
		else if (line.find(L"FLAGS") == 0 && inTrack) {
			Console::Warning("FLAGS command detected but not written (track ");
			std::cout << currentTrack.trackNumber << ")\n";
		}
	}

	if (inTrack && currentTrack.trackNumber > 0) {
		tracks.push_back(currentTrack);
	}

	if (fileCount == 0) {
		Console::Error("No FILE directive found in CUE sheet\n");
		tracks.clear();
		return false;
	}

	// Validate parsed tracks
	if (tracks.empty()) {
		Console::Error("No tracks found in CUE sheet\n");
		return false;
	}

	for (size_t i = 0; i < tracks.size(); i++) {
		if (i > 0 && tracks[i].startLBA <= tracks[i - 1].startLBA) {
			Console::Error("Track ");
			std::cout << tracks[i].trackNumber
				<< " has non-increasing start LBA (expected > "
				<< tracks[i - 1].startLBA << ", got "
				<< tracks[i].startLBA << ")\n";
			tracks.clear();
			return false;
		}
		if (tracks[i].hasPregap && tracks[i].pregapLBA > tracks[i].startLBA) {
			Console::Warning("Track ");
			std::cout << tracks[i].trackNumber
				<< " has pregap LBA (" << tracks[i].pregapLBA
				<< ") after start LBA (" << tracks[i].startLBA
				<< ") -- ignoring pregap\n";
			tracks[i].hasPregap = false;
		}
	}

	// Compute endLBA for all tracks except the last.
	// Must happen BEFORE data track removal so the last audio track inherits
	// the data track's startLBA as its boundary.
	for (size_t i = 0; i < tracks.size(); i++) {
		if (i + 1 < tracks.size()) {
			tracks[i].endLBA = tracks[i + 1].hasPregap
				? tracks[i + 1].pregapLBA - 1
				: tracks[i + 1].startLBA - 1;
		}
	}
	// Zero is a valid end for a one-sector track, so use an unambiguous sentinel
	// for the original final track whose boundary depends on the BIN size.
	tracks.back().endLBA = (std::numeric_limits<DWORD>::max)();

	// Remove non-audio tracks (enhanced CDs have a data session that cannot
	// be written back as part of an audio disc image).
	if (!tracks.front().isAudio) {
		Console::Error("CUE begins with a data track; audio-only writing cannot preserve its LBA layout\n");
		tracks.clear();
		return false;
	}
	auto it = std::remove_if(tracks.begin(), tracks.end(),
		[](const TrackWriteInfo& t) { return !t.isAudio; });
	size_t removed = std::distance(it, tracks.end());
	tracks.erase(it, tracks.end());
	if (removed > 0) {
		Console::Warning("Skipped ");
		std::cout << removed << " data track(s) (audio-only write)\n";
	}

	if (tracks.empty()) {
		Console::Error("No audio tracks found in CUE sheet\n");
		return false;
	}

	Console::Success("Parsed CUE sheet: ");
	std::cout << tracks.size() << " tracks\n";

	if (!discTitle.empty() || !discPerformer.empty()) {
		Console::Info("CD-Text: ");
		if (!discPerformer.empty()) std::cout << discPerformer;
		if (!discPerformer.empty() && !discTitle.empty()) std::cout << " - ";
		if (!discTitle.empty()) std::cout << discTitle;
		std::cout << "\n";
	}

	for (const auto& t : tracks) {
		Console::Info("  Track ");
		std::cout << std::setw(2) << t.trackNumber << ": LBA "
			<< std::setw(6) << t.startLBA << " - ";
		// The last track's endLBA isn't known until the BIN size is read
		// (set later in WriteDisc), so show a placeholder instead of 0.
		if (t.endLBA == (std::numeric_limits<DWORD>::max)())
			std::cout << "   end";
		else
			std::cout << std::setw(6) << t.endLBA;
		if (t.hasPregap && t.startLBA >= t.pregapLBA) {
			std::cout << " (pregap at " << std::setw(6) << t.pregapLBA
				<< ", " << std::setw(3) << (t.startLBA - t.pregapLBA) << " frames)";
		}
		if (!t.title.empty()) {
			std::cout << " \"" << t.title << "\"";
		}
		std::cout << "\n";
	}

	return true;
}
