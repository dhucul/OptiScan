#include "Drive.h"
#include "ConsoleColors.h"
#include "InterruptHandler.h"
#include <iostream>
#include <algorithm>
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <chrono>

HANDLE OpenDriveHandle(wchar_t letter) {
	std::wstring devPath = L"\\\\.\\" + std::wstring(1, letter) + L":";
	return CreateFileW(devPath.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

std::string GetDriveName(HANDLE h) {
	STORAGE_PROPERTY_QUERY query = {};
	query.PropertyId = StorageDeviceProperty;
	query.QueryType = PropertyStandardQuery;

	BYTE buffer[1024] = {};
	DWORD ret;
	if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
		buffer, sizeof(buffer), &ret, nullptr)) {
		return "CD/DVD drive";
	}

	auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);
	std::string name;

	auto appendTrimmed = [&](DWORD offset) {
		if (offset && buffer[offset]) {
			std::string part = reinterpret_cast<char*>(buffer + offset);
			while (!part.empty() && part.back() == ' ')
				part.pop_back();
			if (!part.empty()) {
				if (!name.empty()) name += " ";
				name += part;
			}
		}
		};

	appendTrimmed(desc->VendorIdOffset);
	appendTrimmed(desc->ProductIdOffset);

	return name.empty() ? "CD/DVD drive" : name;
}

int GetAudioTrackCount(HANDLE h) {
	DWORD ret;
	if (!DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY, nullptr, 0, nullptr, 0, &ret, nullptr))
		return -1;

	CDROM_TOC toc = {};
	if (!DeviceIoControl(h, IOCTL_CDROM_READ_TOC, nullptr, 0, &toc, sizeof(toc), &ret, nullptr))
		return -2;

	int n = toc.LastTrack - toc.FirstTrack + 1;
	int audioTracks = 0;
	for (int i = 0; i < n; i++) {
		if ((toc.TrackData[i].Control & AUDIO_TRACK_MASK) == 0) audioTracks++;
	}
	return audioTracks;
}

bool WaitForMediaReady(HANDLE h, int maxWaitMs) {
	auto start = std::chrono::steady_clock::now();
	while (true) {
		DWORD ret;
		if (DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY, nullptr, 0, nullptr, 0, &ret, nullptr)) {
			return true;
		}
		DWORD err = GetLastError();
		if (err == ERROR_NOT_READY) {
			Sleep(250);
		}
		else if (err == ERROR_MEDIA_CHANGED) {
			continue;
		}
		else {
			break;
		}
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - start).count();
		if (elapsed >= maxWaitMs)
			break;
	}
	return false;
}

bool CheckForAudioTracks(HANDLE h) {
	int count = GetAudioTrackCount(h);
	return count > 0;
}

std::vector<wchar_t> ScanDrives(std::vector<wchar_t>& audioDrives, bool verbose) {
	std::vector<wchar_t> cdDrives;
	audioDrives.clear();
	DWORD driveMask = GetLogicalDrives();

	for (wchar_t letter = L'A'; letter <= L'Z'; letter++) {
		if (!(driveMask & (1 << (letter - L'A'))))
			continue;

		std::wstring root = std::wstring(1, letter) + L":\\";
		if (GetDriveTypeW(root.c_str()) != DRIVE_CDROM)
			continue;

		cdDrives.push_back(letter);
		if (verbose) {
			std::cout << "  [";
			Console::SetColor(Console::Color::Yellow);
			std::cout << static_cast<char>(letter) << ":";
			Console::Reset();
			std::cout << "] ";
		}

		HANDLE h = OpenDriveHandle(letter);
		if (h == INVALID_HANDLE_VALUE) {
			if (verbose) std::cout << "\n";
			continue;
		}

		if (verbose) std::cout << GetDriveName(h);

		int audioTracks = GetAudioTrackCount(h);
		if (audioTracks == -1) {
			if (verbose) {
				Console::SetColor(Console::Color::DarkGray);
				std::cout << " - No disc";
				Console::Reset();
			}
		}
		else if (audioTracks == -2) {
			if (verbose) {
				Console::SetColor(Console::Color::DarkGray);
				std::cout << " - Empty/Blank";
				Console::Reset();
			}
		}
		else if (audioTracks > 0) {
			if (verbose) {
				std::cout << " - ";
				Console::SetColor(Console::Color::Green);
				std::cout << "AUDIO CD (" << audioTracks << " tracks)";
				Console::Reset();
			}
			audioDrives.push_back(letter);
		}
		else {
			if (verbose) std::cout << " - Data disc";
		}

		CloseHandle(h);
		if (verbose) std::cout << "\n";
	}

	return cdDrives;
}

wchar_t WaitForDisc(const std::vector<wchar_t>& cdDrives, int timeoutSeconds) {
	Console::Warning("\nNo audio CD detected. Insert disc or enter drive letter (ESC to cancel, Enter to wait): ");

	// Show available drive letters so the user knows what to type
	std::cout << "\n  Available drives: ";
	for (size_t i = 0; i < cdDrives.size(); i++) {
		if (i > 0) std::cout << ", ";
		Console::SetColor(Console::Color::Yellow);
		std::cout << static_cast<char>(cdDrives[i]) << ":";
		Console::Reset();
	}
	std::cout << "\n";

	const DWORD startTime = GetTickCount();
	const DWORD timeoutMs = (timeoutSeconds > 0) ? static_cast<DWORD>(timeoutSeconds) * 1000 : 0;
	int lastSecondsRemaining = -1;

	while (true) {
		if (timeoutMs > 0) {
			DWORD elapsed = GetTickCount() - startTime;
			if (elapsed >= timeoutMs) {
				Console::Warning("\nTimeout waiting for disc.\n");
				return 0;
			}
			int secondsRemaining = static_cast<int>((timeoutMs - elapsed) / 1000);
			if (secondsRemaining != lastSecondsRemaining) {
				lastSecondsRemaining = secondsRemaining;
				std::cout << "\rWaiting... " << secondsRemaining << "s remaining (ESC to cancel)   ";
			}
		}

		if (g_interrupt.IsInterrupted()) {
			Console::Warning("\nInterrupted.\n");
			return 0;
		}

		for (wchar_t letter : cdDrives) {
			HANDLE h = OpenDriveHandle(letter);
			if (h != INVALID_HANDLE_VALUE) {
				if (WaitForMediaReady(h, 2000) && CheckForAudioTracks(h)) {
					CloseHandle(h);
					Console::Info("\nAudio CD detected in drive ");
					std::cout << static_cast<char>(letter) << ":\n";
					return letter;
				}
				CloseHandle(h);
			}
		}

		Sleep(DRIVE_POLL_INTERVAL_MS);
	}
}

std::string GetDiscStatus(HANDLE h, bool& hasAudio, int& audioTracks) {
	hasAudio = false;
	audioTracks = 0;

	int count = GetAudioTrackCount(h);
	if (count == -1) return "No disc";
	if (count == -2) return "Empty/Blank";

	audioTracks = count;
	if (audioTracks > 0) {
		hasAudio = true;
		return "AUDIO CD (" + std::to_string(audioTracks) + " tracks)";
	}
	return "Data disc";
}

DWORD leadOutLBA = 400000;  // ~89 min, beyond most CDs