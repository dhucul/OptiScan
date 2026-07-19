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
// Returns the number of audio tracks, -1 for "no disc" and -2 for empty/blank.
// A freshly seated disc reports neither state cleanly: the storage stack raises
// a one-shot media-change event and then ERROR_NOT_READY while the disc spins
// up. The media-change retry is unconditional (it costs nothing); `notReadyWaitMs`
// is the extra budget the caller is willing to spend waiting out a spin-up
// before accepting "no disc".
int GetAudioTrackCount(HANDLE h, int notReadyWaitMs = 0);
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