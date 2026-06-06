#include "DriveSelection.h"
#include "ConsoleColors.h"
#include "Drive.h"
#include "MenuHelpers.h"
#include <iostream>
#include <windows.h>

wchar_t SelectAudioDrive(const std::vector<wchar_t>& audioDrives) {
	Console::Warning("\nMultiple audio CDs detected. Select drive:\n");
	std::string msg;  // plain-text mirror of the list for the modal box
	for (size_t i = 0; i < audioDrives.size(); i++) {
		HANDLE h = OpenDriveHandle(audioDrives[i]);
		std::string name = (h != INVALID_HANDLE_VALUE) ? GetDriveName(h) : "CD/DVD drive";
		int tracks = (h != INVALID_HANDLE_VALUE) ? GetAudioTrackCount(h) : 0;
		if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
		std::cout << "  " << (i + 1) << ". [";
		Console::SetColor(Console::Color::Yellow);
		std::cout << static_cast<char>(audioDrives[i]) << ":";
		Console::Reset();
		std::cout << "] " << name;
		if (tracks > 0) std::cout << " (" << tracks << " tracks)";
		std::cout << "\n";

		msg += std::to_string(i + 1) + ". [";
		msg += static_cast<char>(audioDrives[i]);
		msg += ":] " + name;
		if (tracks > 0) msg += " (" + std::to_string(tracks) + " tracks)";
		msg += "\n";
	}
	if (!msg.empty() && msg.back() == '\n') msg.pop_back();
	std::cout << "Choice: ";
	bool ok = false;
	int pick = GetMenuChoice("Select Audio Drive", msg.c_str(),
		1, static_cast<int>(audioDrives.size()), 1, &ok);
	if (!ok) return 0;   // 0 = caller treats as "no drive selected"
	return audioDrives[pick - 1];
}

wchar_t SelectWriterDrive(const std::vector<wchar_t>& cdDrives,
                          wchar_t defaultLetter) {
	if (cdDrives.empty()) return 0;

	Console::Heading("\nSelect drive to write with:\n");
	std::string msg;  // plain-text mirror of the list for the modal box
	int defaultIdx = 1;
	for (size_t i = 0; i < cdDrives.size(); i++) {
		wchar_t letter = cdDrives[i];
		if (letter == defaultLetter) defaultIdx = static_cast<int>(i + 1);

		HANDLE h = OpenDriveHandle(letter);
		std::string name = (h != INVALID_HANDLE_VALUE) ? GetDriveName(h) : "CD/DVD drive";
		int tracks = (h != INVALID_HANDLE_VALUE) ? GetAudioTrackCount(h) : -1;
		if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

		std::cout << "  " << (i + 1) << ". [";
		Console::SetColor(Console::Color::Yellow);
		std::cout << static_cast<char>(letter) << ":";
		Console::Reset();
		std::cout << "] " << name;

		msg += std::to_string(i + 1) + ". [";
		msg += static_cast<char>(letter);
		msg += ":] " + name;

		if (tracks == -1) {
			Console::SetColor(Console::Color::DarkGray);
			std::cout << " - No disc";
			Console::Reset();
			msg += " - No disc";
		}
		else if (tracks == -2) {
			Console::SetColor(Console::Color::DarkGray);
			std::cout << " - Blank/empty";
			Console::Reset();
			msg += " - Blank/empty";
		}
		else if (tracks > 0) {
			std::cout << " - ";
			Console::SetColor(Console::Color::Green);
			std::cout << "Audio CD (" << tracks << " tracks)";
			Console::Reset();
			msg += " - Audio CD (" + std::to_string(tracks) + " tracks)";
		}
		else {
			std::cout << " - Data disc";
			msg += " - Data disc";
		}
		if (letter == defaultLetter) {
			Console::SetColor(Console::Color::DarkGray);
			std::cout << "  [current]";
			Console::Reset();
			msg += "  [current]";
		}
		std::cout << "\n";
		msg += "\n";
	}
	if (!msg.empty() && msg.back() == '\n') msg.pop_back();

	bool ok = false;
	int pick = GetMenuChoice("Select Writer Drive", msg.c_str(),
		1, static_cast<int>(cdDrives.size()), defaultIdx, &ok);
	if (!ok) return 0;
	return cdDrives[pick - 1];
}