// ============================================================================
// AudioFileSource.h - Shared WAV/FLAC input handling for the write workflows.
//
// These helpers were originally private to WriteTracksWorkflow.cpp. They carry
// no dependency on a source disc, so both "write tracks using the current
// disc's pregaps" and "write disc from a CUE sheet" build their PCM input the
// same way: probe a RIFF/WAVE header for its payload, decode FLAC through
// flac.exe first when needed, and optionally sample-shift the finished image
// by the burner's write offset.
// ============================================================================
#pragma once

#include <climits>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

// Located PCM payload inside a .wav file (possibly one decoded from .flac).
// Only the header is read - the audio itself is streamed straight to the
// output image when the caller needs it, so a 700 MB image never sits in RAM.
struct WavPayload {
    std::wstring originalPath;   // .wav or .flac as supplied by the user
    std::wstring wavPath;        // path actually opened for PCM (== originalPath unless decoded)
    std::wstring tempWavPath;    // non-empty if we created a temp WAV from FLAC
    DWORD dataOffset = 0;        // byte offset of "data" payload inside wavPath
    DWORD dataBytes = 0;         // size of the "data" payload
    DWORD sectorCount = 0;       // ceil(dataBytes / AUDIO_SECTOR_SIZE)
};

bool Utf8ToWide(const std::string& input, std::wstring& output);
bool EndsWithLower(const std::wstring& s, const std::wstring& suffix);
void FormatMSF(DWORD lba, std::ostringstream& out);

// Read RIFF/WAVE header and locate the "data" chunk. Accepts files with extra
// chunks before "data" (LIST/INFO/bext/etc.). Rejects anything that is not
// 16-bit / 44100 Hz / stereo PCM, with the reason in `err`.
bool ProbeWavFile(const std::wstring& path, WavPayload& out, std::string& err);

// Decode a FLAC file to a temp WAV using flac.exe. Returns false if flac.exe
// is missing or the decode fails.
bool DecodeFlacToWav(const std::wstring& flacPath, const std::wstring& outWavPath);

// True when flac.exe can be found on PATH or beside the executable. Checked
// up front so a CUE full of FLAC references fails before anything is decoded,
// rather than midway through with a decoder-specific error.
bool FlacDecoderAvailable();

// Extension-dispatching wrapper: .flac decodes to `tempWavPath` first, .wav is
// probed in place. Fills every WavPayload field including sectorCount.
bool PrepareAudioFile(const std::wstring& path, const std::wstring& tempWavPath,
                      WavPayload& out, std::string& err);

// Delete any temp WAVs the payloads created and clear their tempWavPath.
void CleanupTempWavs(std::vector<WavPayload>& payloads);

// Collect .wav / .flac files from a folder, sorted alphabetically (case-insensitive).
std::vector<std::wstring> ScanAudioFiles(const std::wstring& folder);

// Shift a finished .bin image by a whole number of samples, in place.
bool ApplyFileSampleOffset(const std::wstring& binPath, int offsetSamples,
                           uint64_t totalBytes);

// Sentinel returned by SelectWriteOffset to mean "user chose Back".
// Picked far outside any plausible drive offset (typically +/-1000 samples max).
constexpr int WRITE_OFFSET_BACK = INT_MIN;

// Look up a drive's read offset in the AccurateRip database. Needs no disc.
bool LookupDriveReadOffset(wchar_t driveLetter, int& readOffset,
                           std::string& source);

// Prompt for the write-offset compensation value to apply before burning.
int SelectWriteOffset(int burnerReadOffset, bool burnerReadOffsetKnown);
