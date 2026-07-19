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

// IOCTL_STORAGE_CHECK_VERIFY reports two transient conditions that are *not*
// "no disc", and treating either as one is what makes a disc moved from one
// drive to another read as an empty tray:
//
//   ERROR_MEDIA_CHANGED — the storage stack latches a media-change event and
//     reports it exactly once, on the first access after the swap. A handle
//     opened right after the user moves a disc hits this every time, so the
//     retry below is unconditional: it consumes the latched event and the next
//     call gives the real answer at no cost.
//   ERROR_NOT_READY — returned both by an empty tray and by a disc that is
//     still spinning up, indistinguishable at this layer. Only a bounded wait
//     can tell them apart, so the caller supplies the budget it can afford.
int GetAudioTrackCount(HANDLE h, int notReadyWaitMs) {
	DWORD ret;
	const auto start = std::chrono::steady_clock::now();
	int mediaChangedRetries = 4;   // bounded: never spin on a wedged device

	while (!DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY, nullptr, 0, nullptr, 0, &ret, nullptr)) {
		const DWORD err = GetLastError();

		if (err == ERROR_MEDIA_CHANGED) {
			if (mediaChangedRetries-- > 0)
				continue;          // event consumed; re-ask immediately
			return -1;
		}
		if (err != ERROR_NOT_READY)
			return -1;             // a real failure, not a spin-up

		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - start).count();
		if (elapsed >= notReadyWaitMs)
			return -1;
		Sleep(250);
	}

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
	int mediaChangedRetries = 4;
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
			// Latched media-change event: re-ask at once rather than sleeping.
			// Bounded, because this branch doesn't sleep — the old `continue`
			// here also skipped the timeout check below, so a device that kept
			// re-reporting the event span forever.
			if (mediaChangedRetries-- <= 0) break;
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

namespace {

// One CD/DVD drive and what the media probe made of its contents.
struct DriveProbe {
	wchar_t     letter = 0;
	bool        opened = false;
	std::string name;
	int         audioTracks = -1;  // >0 audio CD, 0 data, -1 no disc, -2 empty/blank
};

// Extra time ScanDrives will spend waiting out disc spin-up, in total and per
// drive. Only ever spent when the quick pass found no audio disc at all — i.e.
// exactly when the alternative is reporting "no audio CD anywhere" for a disc
// the user has just seated.
constexpr int SCAN_SPINUP_BUDGET_MS = 5000;
constexpr int SCAN_SPINUP_PER_DRIVE_MS = 4000;

// Once one disc has answered, the remaining candidates get only a short look.
// They still have to be probed — stopping at the first hit would under-report
// `audioDrives`, and a caller that sees one disc where there are two switches
// to it silently instead of offering the choice. But a drive loaded alongside
// one that just came ready is either ready too or empty, so a brief probe is
// enough and an empty tray costs little.
constexpr int SCAN_SPINUP_AFTER_HIT_MS = 750;

}  // namespace

std::vector<wchar_t> ScanDrives(std::vector<wchar_t>& audioDrives, bool verbose) {
	std::vector<wchar_t> cdDrives;
	audioDrives.clear();
	DWORD driveMask = GetLogicalDrives();

	// ── Pass 1: probe every drive, no spin-up wait ───────────
	std::vector<DriveProbe> probes;
	for (wchar_t letter = L'A'; letter <= L'Z'; letter++) {
		if (!(driveMask & (1 << (letter - L'A'))))
			continue;

		std::wstring root = std::wstring(1, letter) + L":\\";
		if (GetDriveTypeW(root.c_str()) != DRIVE_CDROM)
			continue;

		cdDrives.push_back(letter);

		DriveProbe p;
		p.letter = letter;

		HANDLE h = OpenDriveHandle(letter);
		if (h != INVALID_HANDLE_VALUE) {
			p.opened = true;
			if (verbose) p.name = GetDriveName(h);
			p.audioTracks = GetAudioTrackCount(h);
			CloseHandle(h);
		}
		probes.push_back(p);
	}

	// ── Pass 2: re-probe the "no disc" drives with a grace ───
	// A disc the user has just moved between drives is usually still spinning up
	// when the next workflow rescans, so the quick pass reads it as an empty
	// tray. Callers treat an empty result as "keep the drive we already had"
	// (see ReselectSourceDriveIfMultiple), which silently runs the workflow
	// against the drive the disc was taken *out* of. Re-probe only when nothing
	// was found, so a scan that already succeeded never pays the wait.
	bool foundAudio = false;
	for (const auto& p : probes) {
		if (p.audioTracks > 0) { foundAudio = true; break; }
	}

	if (!foundAudio) {
		if (verbose) Console::Info("  Waiting for a disc to become ready...\n");

		int budget = SCAN_SPINUP_BUDGET_MS;
		bool hit = false;
		for (auto& p : probes) {
			if (budget <= 0) break;
			// A data disc (0) is a definitive answer; so is a drive we couldn't open.
			if (!p.opened || p.audioTracks >= 0) continue;

			HANDLE h = OpenDriveHandle(p.letter);
			if (h == INVALID_HANDLE_VALUE) continue;

			const int slice = (std::min)(
				budget, hit ? SCAN_SPINUP_AFTER_HIT_MS : SCAN_SPINUP_PER_DRIVE_MS);

			const auto start = std::chrono::steady_clock::now();
			p.audioTracks = GetAudioTrackCount(h, slice);
			budget -= static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count());
			CloseHandle(h);

			if (p.audioTracks > 0) hit = true;
		}
	}

	// ── Report ───────────────────────────────────────────────
	for (const auto& p : probes) {
		if (p.audioTracks > 0)
			audioDrives.push_back(p.letter);

		if (!verbose) continue;

		std::cout << "  [";
		Console::SetColor(Console::Color::Yellow);
		std::cout << static_cast<char>(p.letter) << ":";
		Console::Reset();
		std::cout << "] ";

		if (!p.opened) {
			std::cout << "\n";
			continue;
		}

		std::cout << p.name;
		if (p.audioTracks == -1) {
			Console::SetColor(Console::Color::DarkGray);
			std::cout << " - No disc";
			Console::Reset();
		}
		else if (p.audioTracks == -2) {
			Console::SetColor(Console::Color::DarkGray);
			std::cout << " - Empty/Blank";
			Console::Reset();
		}
		else if (p.audioTracks > 0) {
			std::cout << " - ";
			Console::SetColor(Console::Color::Green);
			std::cout << "AUDIO CD (" << p.audioTracks << " tracks)";
			Console::Reset();
		}
		else {
			std::cout << " - Data disc";
		}
		std::cout << "\n";
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