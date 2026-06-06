#pragma once
#include <windows.h>
#include <vector>
#include <string>

// Drive operation constants
constexpr int DRIVE_POLL_INTERVAL_MS = 500;
constexpr DWORD AUDIO_TRACK_MASK = 0x04;

// Drive information and operations
HANDLE OpenDriveHandle(wchar_t letter);
std::string GetDriveName(HANDLE h);
int GetAudioTrackCount(HANDLE h);
bool WaitForMediaReady(HANDLE h, int maxWaitMs = 5000);
bool CheckForAudioTracks(HANDLE h);
// Enumerate CD/DVD drives. `audioDrives` is filled with drives that currently
// contain an audio CD. When `verbose` is true (the default) the scan also
// prints a per-drive line with vendor/model and media status; pass false when
// a separate selector (e.g. SelectWriterDrive) will list the drives, to avoid
// duplicate output.
std::vector<wchar_t> ScanDrives(std::vector<wchar_t>& audioDrives,
                                bool verbose = true);
wchar_t WaitForDisc(const std::vector<wchar_t>& cdDrives, int timeoutSeconds = 0);
std::string GetDiscStatus(HANDLE h, bool& hasAudio, int& audioTracks);