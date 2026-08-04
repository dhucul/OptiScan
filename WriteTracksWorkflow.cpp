// ============================================================================
// WriteTracksWorkflow.cpp - Write ripped track files to disc using the
// pregap layout of the disc currently in the drive.
//
// Workflow:
//   1. Confirm the inserted disc has audio tracks and show its pregap layout.
//   2. Pick a folder containing one WAV (or FLAC) per audio track, sorted by
//      filename. FLAC inputs are decoded to a temp WAV via flac.exe.
//   3. Validate format (16-bit / 44100 Hz / stereo) and warn on length mismatch
//      vs the source TOC.
//   4. Build a temporary .bin (track audio + silence pregaps) and .cue with
//      matching INDEX 00 / 01 entries.
//   5. Eject the source disc, wait for a blank, reopen the drive.
//   6. Reuse the existing WriteDisc() pipeline (blanking, OPC, CUE sheet,
//      CD-Text, IMAPI fallback).
//   7. Clean up temp files.
//
// Pregaps in the produced disc are silence — the rip workflow drops pregap
// audio (PregapMode::Skip), so the original gap audio is not recoverable from
// the track files. Gap *durations* are preserved exactly.
// ============================================================================
#define NOMINMAX
#include "WriteTracksWorkflow.h"
#include "ConsoleColors.h"
#include "Constants.h"
#include "Drive.h"
#include "DriveSelection.h"
#include "FileUtils.h"
#include "GuiInput.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include "PioneerVendor.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace {

struct TrackSource {
    std::wstring originalPath;     // .wav or .flac as supplied by the user
    std::wstring wavPath;          // path actually opened for PCM (== originalPath unless decoded)
    std::wstring tempWavPath;      // non-empty if we created a temp WAV from FLAC
    DWORD dataOffset = 0;          // byte offset of "data" payload inside wavPath
    DWORD dataBytes = 0;           // size of the "data" payload
    DWORD sectorCount = 0;         // ceil(dataBytes / 2352)
    DWORD pregapSectors = 0;       // silence sectors emitted before this track's audio
    DWORD binStartLBA = 0;         // BIN LBA of INDEX 01 (audio start)
    DWORD binPregapLBA = 0;        // BIN LBA of INDEX 00 (pregap start) — only valid if pregapSectors > 0
    // Source-disc pregap audio captured at write time, offset-corrected.  Used
    // to preserve non-silent pregaps (live albums, continuous mixes) that the
    // PregapMode::Skip rip discards.  Empty → fall back to silence.
    std::vector<std::vector<BYTE>> pregapAudio;
    // Offset-corrected sectors immediately BEFORE this track's pregap, read
    // contiguously across the boundary so they carry the true audio for the
    // last few LBAs of the previous track's INDEX 01 region.  Used to repair
    // the previous track's gap-corrupted last WAV sector(s).  Captured even
    // when the pregap audio itself can't be read (drive refusing INDEX 00).
    std::vector<std::vector<BYTE>> headOverlap;
};

bool Utf8ToWide(const std::string& input, std::wstring& output) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
    if (wlen <= 0) { output.clear(); return false; }
    std::wstring wide(static_cast<size_t>(wlen), L'\0');
    int converted = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, wide.data(), wlen);
    if (converted <= 0) { output.clear(); return false; }
    if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
    output = std::move(wide);
    return true;
}

bool EndsWithLower(const std::wstring& s, const std::wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); i++) {
        if (towlower(s[s.size() - suffix.size() + i]) != suffix[i]) return false;
    }
    return true;
}

void FormatMSF(DWORD lba, std::ostringstream& out) {
    DWORD m = lba / (75 * 60);
    DWORD s = (lba / 75) % 60;
    DWORD f = lba % 75;
    out << std::setfill('0') << std::setw(2) << m << ":"
        << std::setw(2) << s << ":"
        << std::setw(2) << f << std::setfill(' ');
}

// Read RIFF/WAVE header and locate the "data" chunk. Accepts files with extra
// chunks before "data" (LIST/INFO/bext/etc.).
bool ProbeWavFile(const std::wstring& path, TrackSource& ts, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open file"; return false; }

    char riff[12] = {};
    f.read(riff, 12);
    if (f.gcount() != 12 || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file"; return false;
    }

    bool fmtSeen = false;
    while (f) {
        char chunkId[4];
        uint32_t chunkSize = 0;
        f.read(chunkId, 4);
        if (f.gcount() != 4) break;
        f.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (f.gcount() != 4) break;

        if (memcmp(chunkId, "fmt ", 4) == 0) {
			if (chunkSize < 16 || chunkSize > 1024 * 1024) {
				err = chunkSize < 16 ? "fmt chunk too small" : "fmt chunk is unreasonably large";
				return false;
			}
            std::vector<char> fmtData(chunkSize);
            f.read(fmtData.data(), chunkSize);
			if (f.gcount() != static_cast<std::streamsize>(chunkSize)) {
				err = "truncated fmt chunk";
				return false;
			}
			uint16_t audioFmt = 0, channels = 0, bps = 0;
			uint32_t sampleRate = 0;
			memcpy(&audioFmt, fmtData.data() + 0, sizeof(audioFmt));
			memcpy(&channels, fmtData.data() + 2, sizeof(channels));
			memcpy(&sampleRate, fmtData.data() + 4, sizeof(sampleRate));
			memcpy(&bps, fmtData.data() + 14, sizeof(bps));

            if (audioFmt != 1) { err = "not PCM (compressed WAV unsupported)"; return false; }
            if (channels != 2) { err = "not stereo"; return false; }
            if (sampleRate != 44100) { err = "sample rate is not 44100 Hz"; return false; }
            if (bps != 16) { err = "not 16-bit"; return false; }

            // Pad odd-sized chunks
            if (chunkSize & 1) f.seekg(1, std::ios::cur);
            fmtSeen = true;
        }
        else if (memcmp(chunkId, "data", 4) == 0) {
            if (!fmtSeen) { err = "data chunk before fmt chunk"; return false; }
			const std::streamoff dataOffset = f.tellg();
			if (dataOffset < 0 || static_cast<unsigned long long>(dataOffset) > MAXDWORD) {
				err = "data chunk offset is out of range";
				return false;
			}
			ts.dataOffset = static_cast<DWORD>(dataOffset);
            ts.dataBytes = chunkSize;
            return true;
        }
        else {
            // Skip unknown chunk (and pad byte for odd sizes)
            f.seekg(chunkSize + (chunkSize & 1), std::ios::cur);
        }
    }
    err = "data chunk not found";
    return false;
}

// Decode a FLAC file to a temp WAV using flac.exe. Returns false if flac.exe
// is missing or the decode fails.
bool DecodeFlacToWav(const std::wstring& flacPath, const std::wstring& outWavPath) {
    std::wstring cmd = L"flac --decode --silent --force -o \"" + outWavPath + L"\" \"" + flacPath + L"\"";

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    bool cancelled = false;
    DWORD waitResult = WAIT_TIMEOUT;
    while ((waitResult = WaitForSingleObject(pi.hProcess, 100)) == WAIT_TIMEOUT) {
        if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
            cancelled = true;
            // TerminateProcess is asynchronous. Keep the process handle and
            // wait until it is signalled so CleanupSources cannot delete the
            // output WAV while flac.exe is still writing it. This blocks only
            // the workflow worker; the GUI message loop remains responsive.
            const BOOL terminateIssued =
                TerminateProcess(pi.hProcess, ERROR_CANCELLED);
            if (terminateIssued) {
                waitResult = WaitForSingleObject(pi.hProcess, INFINITE);
            }
            else {
                // The process may have exited between the polling wait and
                // TerminateProcess. Waiting on the valid handle covers that
                // race and also prevents cleanup from overtaking a decoder
                // that Windows refused to terminate.
                waitResult = WaitForSingleObject(pi.hProcess, INFINITE);
            }
            break;
        }
    }
    DWORD exitCode = 1;
    const bool processExited = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return !cancelled && processExited && exitCode == 0;
}

// Collect .wav / .flac files from a folder, sorted alphabetically (case-insensitive).
std::vector<std::wstring> ScanAudioFiles(const std::wstring& folder) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return files;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (EndsWithLower(name, L".wav") || EndsWithLower(name, L".flac")) {
            files.push_back(folder + L"\\" + name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    return files;
}

// Append a track's PCM payload as 2352-byte sectors, padding the last sector
// with zeros if the WAV doesn't end on a sector boundary. Bails out if the
// WAV file delivers fewer bytes than the "data" chunk header promised — a
// truncated rip would otherwise produce silent-tail tracks with no error.
bool AppendWavToSectors(std::vector<std::vector<BYTE>>& sectors, const TrackSource& ts) {
    std::ifstream in(ts.wavPath, std::ios::binary);
    if (!in) return false;
    in.seekg(ts.dataOffset);

    DWORD remaining = ts.dataBytes;
    for (DWORD s = 0; s < ts.sectorCount; s++) {
        std::vector<BYTE> sec(AUDIO_SECTOR_SIZE, 0);
        DWORD toRead = (remaining < AUDIO_SECTOR_SIZE) ? remaining : AUDIO_SECTOR_SIZE;
        if (toRead > 0) {
            in.read(reinterpret_cast<char*>(sec.data()), toRead);
            DWORD got = static_cast<DWORD>(in.gcount());
            if (got != toRead) {
                // Short read: the WAV is shorter than its declared data size,
                // or a stream error occurred. Either way the caller would get
                // silent-tail audio, which would shift every following track
                // and corrupt AccurateRip CRCs. Fail loudly instead.
                return false;
            }
            remaining -= got;
        }
        sectors.push_back(std::move(sec));
    }
    return true;
}

void AppendSilenceSectors(std::vector<std::vector<BYTE>>& sectors, DWORD count) {
    for (DWORD i = 0; i < count; i++) {
        sectors.emplace_back(AUDIO_SECTOR_SIZE, 0);
    }
}

DWORD ComputeMarginSectors(int driveReadOffset) {
    DWORD marginSectors = 2;
    if (driveReadOffset != 0) {
		const int64_t signedOffset = static_cast<int64_t>(driveReadOffset);
		const int64_t absoluteOffset = signedOffset < 0 ? -signedOffset : signedOffset;
        DWORD needed = static_cast<DWORD>(
			(absoluteOffset * 4 + AUDIO_SECTOR_SIZE - 1)
            / AUDIO_SECTOR_SIZE) + 1;
        if (needed > marginSectors) marginSectors = needed;
    }
    return marginSectors;
}

// Read just the boundary sectors immediately BEFORE a pregap and apply offset
// correction.  These sit in the previous track's INDEX 01 (normal audio) range,
// so they read reliably even on drives that refuse to return pregap (INDEX 00)
// audio.  Used to repair the previous track's last WAV sector — corrupted by
// the rip's ApplyOffsetCorrection shifting samples across the PregapMode::Skip
// gap.
//
// Reads marginSectors before the pregap plus one sector AT pregapStart so the
// offset shift has a "next" sector to pull bytes from (typical CD drives can
// read the very first sector of a pregap region — it's the deeper sectors that
// some firmware refuses).  If even that one sector fails, falls back to reading
// just the boundary sectors and accepts that the very last byte-tail of the
// last overlap sector will be zero-padded (24 bytes for offset 6) — still much
// better than leaving the rip's gap corruption in place.
//
// Returns false only when even the boundary sectors are unreadable.
bool ReadBoundaryOverlap(OpticalDrive& copier, DWORD pregapStart,
    int driveReadOffset, DWORD marginSectors,
    std::vector<std::vector<BYTE>>& outHeadOverlap) {
    outHeadOverlap.clear();
    if (pregapStart < marginSectors) return false;

    DWORD readStart = pregapStart - marginSectors;
    DWORD readCount = marginSectors + 1;  // +1 sector for the offset shift target

    std::vector<std::vector<BYTE>> readBuf(readCount,
        std::vector<BYTE>(AUDIO_SECTOR_SIZE, 0));

    auto& drive = copier.GetDriveRef();
    for (DWORD j = 0; j < readCount; j++) {
        if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) return false;
        if (!drive.ReadSectorAudioOnly(readStart + j, readBuf[j].data())) {
            if (j < marginSectors) {
                // A boundary-overlap sector itself is unreadable — give up;
                // burning silence here would corrupt the previous track's
                // tail audio in the BIN.
                return false;
            }
            // The +1 shift-target sector failed (drives that refuse INDEX 00
            // hit this).  Leave it zero-filled and stop reading.  The last
            // `byteOffset` bytes of the last overlap sector will end up zero
            // after ApplySampleOffset, but that tiny tail sits right at the
            // pregap boundary — typically inside the CRC's 16-sector edge
            // trim, so it doesn't break track CRCs.
            break;
        }
    }

    if (driveReadOffset != 0) {
        copier.ApplySampleOffset(readBuf, driveReadOffset);
    }

    outHeadOverlap.reserve(marginSectors);
    for (DWORD j = 0; j < marginSectors; j++) {
        outHeadOverlap.push_back(std::move(readBuf[j]));
    }
    return true;
}

// Read the pregap region [pregapStart, pregapEnd] (inclusive LBAs) from the
// source disc and return its sectors, with the drive's read offset applied so
// the captured audio is at "true" sample positions — matching the offset-
// corrected WAVs from the rip.  Reads a small margin on each end so the
// offset shift doesn't push samples past the boundaries.
//
// Returns false on any read failure; the caller should fall back to silence
// for the pregap audio AND attempt ReadBoundaryOverlap separately to still get
// the head-overlap data needed for the boundary repair.
bool ReadPregapAudio(OpticalDrive& copier, DWORD pregapStart, DWORD pregapEnd,
    int driveReadOffset, DWORD marginSectors,
    std::vector<std::vector<BYTE>>& outSectors,
    std::vector<std::vector<BYTE>>& outHeadOverlap) {
    outHeadOverlap.clear();
    if (pregapEnd < pregapStart) { outSectors.clear(); return true; }

    DWORD readStart = (pregapStart >= marginSectors) ? pregapStart - marginSectors : 0;
    DWORD readEnd = pregapEnd + marginSectors;
    DWORD readCount = readEnd - readStart + 1;

    std::vector<std::vector<BYTE>> readBuf(readCount,
        std::vector<BYTE>(AUDIO_SECTOR_SIZE, 0));
    auto& drive = copier.GetDriveRef();
    for (DWORD j = 0; j < readCount; j++) {
        if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) return false;
        if (!drive.ReadSectorAudioOnly(readStart + j, readBuf[j].data())) {
            return false;
        }
    }

    if (driveReadOffset != 0) {
        copier.ApplySampleOffset(readBuf, driveReadOffset);
    }

    DWORD pregapCount = pregapEnd - pregapStart + 1;
    DWORD startIdx = pregapStart - readStart;

    outHeadOverlap.reserve(startIdx);
    for (DWORD j = 0; j < startIdx; j++) {
        outHeadOverlap.push_back(std::move(readBuf[j]));
    }

    outSectors.clear();
    outSectors.reserve(pregapCount);
    for (DWORD j = 0; j < pregapCount; j++) {
        outSectors.push_back(std::move(readBuf[startIdx + j]));
    }

    return true;
}

bool WriteSourcesToBin(const std::wstring& binPath, std::vector<TrackSource>& sources) {
    std::ofstream bin(binPath, std::ios::binary | std::ios::trunc);
    if (!bin) return false;

    std::vector<BYTE> sector(AUDIO_SECTOR_SIZE, 0);
    auto writeSector = [&](const std::vector<BYTE>* source) {
        std::fill(sector.begin(), sector.end(), 0);
        if (source && !source->empty()) {
            memcpy(sector.data(), source->data(),
                std::min<size_t>(source->size(), AUDIO_SECTOR_SIZE));
        }
        bin.write(reinterpret_cast<const char*>(sector.data()), sector.size());
        return bin.good();
    };

    for (auto& source : sources) {
        if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) return false;

        const bool hasPregapAudio =
            source.pregapAudio.size() == source.pregapSectors;
        for (DWORD i = 0; i < source.pregapSectors; ++i) {
            if ((i & 63u) == 0 &&
                (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey())) return false;
            if (!writeSector(hasPregapAudio ? &source.pregapAudio[i] : nullptr)) return false;
        }
        source.pregapAudio.clear();
        source.pregapAudio.shrink_to_fit();

        std::ifstream wav(source.wavPath, std::ios::binary);
        if (!wav) return false;
        wav.seekg(source.dataOffset);
        DWORD remaining = source.dataBytes;
        for (DWORD i = 0; i < source.sectorCount; ++i) {
            if ((i & 63u) == 0 &&
                (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey())) return false;
            std::fill(sector.begin(), sector.end(), 0);
            const DWORD request = (std::min)(remaining, static_cast<DWORD>(AUDIO_SECTOR_SIZE));
            if (request > 0) {
                wav.read(reinterpret_cast<char*>(sector.data()), request);
                if (static_cast<DWORD>(wav.gcount()) != request) return false;
                remaining -= request;
            }
            bin.write(reinterpret_cast<const char*>(sector.data()), sector.size());
            if (!bin) return false;
        }
    }

    bin.close();
    return bin.good();
}

bool PatchBoundaryOverlaps(const std::wstring& binPath,
    std::vector<TrackSource>& sources, int& repaired) {
    repaired = 0;
    std::fstream bin(binPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!bin) return false;
    std::vector<BYTE> sector(AUDIO_SECTOR_SIZE, 0);

    for (size_t i = 1; i < sources.size(); ++i) {
        auto& overlap = sources[i].headOverlap;
        if (overlap.empty()) continue;
        const DWORD pos = sources[i].binPregapLBA;
        const DWORD count = static_cast<DWORD>(overlap.size());
        if (pos < count) { overlap.clear(); continue; }

        const uint64_t bytePos = static_cast<uint64_t>(pos - count) * AUDIO_SECTOR_SIZE;
        bin.seekp(static_cast<std::streamoff>(bytePos));
        if (!bin) return false;
        for (const auto& source : overlap) {
            std::fill(sector.begin(), sector.end(), 0);
            if (!source.empty()) {
                memcpy(sector.data(), source.data(),
                    std::min<size_t>(source.size(), AUDIO_SECTOR_SIZE));
            }
            bin.write(reinterpret_cast<const char*>(sector.data()), sector.size());
            if (!bin) return false;
        }
        overlap.clear();
        ++repaired;
    }
    bin.close();
    return bin.good();
}

bool ApplyFileSampleOffset(const std::wstring& binPath, int offsetSamples,
    uint64_t totalBytes) {
    if (offsetSamples == 0) return true;
    const int64_t signedOffset = static_cast<int64_t>(offsetSamples) * 4;
    const uint64_t shift = static_cast<uint64_t>(signedOffset < 0 ? -signedOffset : signedOffset);
    if (shift >= totalBytes) return false;

    const std::wstring shiftedPath = binPath + L".offset";
    std::ifstream input(binPath, std::ios::binary);
    std::ofstream output(shiftedPath, std::ios::binary | std::ios::trunc);
    if (!input || !output) return false;

    std::vector<char> buffer(1024 * 1024, 0);
    auto writeZeros = [&](uint64_t count) {
        while (count > 0) {
            const size_t chunk = static_cast<size_t>((std::min<uint64_t>)(count, buffer.size()));
            std::fill(buffer.begin(), buffer.begin() + chunk, 0);
            output.write(buffer.data(), chunk);
            if (!output) return false;
            count -= chunk;
        }
        return true;
    };
    auto copyBytes = [&](uint64_t count) {
        while (count > 0) {
            if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) return false;
            const size_t chunk = static_cast<size_t>((std::min<uint64_t>)(count, buffer.size()));
            input.read(buffer.data(), chunk);
            if (static_cast<size_t>(input.gcount()) != chunk) return false;
            output.write(buffer.data(), chunk);
            if (!output) return false;
            count -= chunk;
        }
        return true;
    };

    bool ok = false;
    if (signedOffset > 0) {
        input.seekg(static_cast<std::streamoff>(shift));
        ok = input.good() && copyBytes(totalBytes - shift) && writeZeros(shift);
    }
    else {
        ok = writeZeros(shift) && copyBytes(totalBytes - shift);
    }
    input.close();
    output.close();
    ok = ok && output.good();
    if (!ok || !MoveFileExW(shiftedPath.c_str(), binPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(shiftedPath.c_str());
        return false;
    }
    return true;
}

// Sentinel returned by SelectWriteOffset to mean "user chose Back".
// Picked far outside any plausible drive offset (typically ±1000 samples max).
constexpr int WRITE_OFFSET_BACK = INT_MIN;

bool LookupDriveReadOffset(wchar_t driveLetter, int& readOffset,
    std::string& source) {
    readOffset = 0;
    source.clear();

    ScsiDrive probe;
    if (!probe.Open(driveLetter)) return false;

    DriveOffsetInfo info;
    if (!probe.LookupAccurateRipOffset(info)) return false;

    readOffset = info.readOffset;
    source = info.source;
    return true;
}

// Prompt for the write-offset compensation value to apply before burning.
// `burnerReadOffsetKnown` is deliberately separate from the source-disc read
// offset: a cross-drive burn must never derive its recommendation from the
// reader. Returns WRITE_OFFSET_BACK for back/cancel, otherwise the sample shift.
int SelectWriteOffset(int burnerReadOffset, bool burnerReadOffsetKnown) {
    std::cout << "\n=== Write-Offset Compensation ===\n";
    std::cout << "AccurateRip-correct burns require pre-shifting the audio by the\n";
    std::cout << "burner's write offset. Without this, the burned disc's track CRCs\n";
    std::cout << "will be shifted and AccurateRip verification will fail.\n";

    auto promptManualOffset = []() {
        bool manualOk = false;
        const int value = GuiInput::PromptInt("Write offset",
            "Enter the selected burner's write offset in samples "
            "(positive or negative):",
            -10000, 10000, 0, &manualOk);
        return manualOk ? value : WRITE_OFFSET_BACK;
    };

    int chosen = 0;
    if (burnerReadOffsetKnown) {
        std::cout << "\n  Selected burner's read offset: "
            << burnerReadOffset << " samples\n\n";
        std::cout << "0. Back to menu\n";
        std::cout << "1. Apply -(burner read offset) = " << (-burnerReadOffset)
            << " samples (typical where write offset = -read offset)\n";
        std::cout << "2. Apply +(burner read offset) = " << burnerReadOffset << " samples\n";
        std::cout << "3. Enter burner write offset manually (samples)\n";
        std::cout << "4. No compensation (offset = 0)\n";
        std::cout << "Choice: ";

        std::string offsetMsg =
            "0. Back to menu\n"
            "1. Apply -(burner read offset) = " + std::to_string(-burnerReadOffset) +
            " samples (typical: write offset = -read offset)\n"
            "2. Apply +(burner read offset) = " + std::to_string(burnerReadOffset) +
            " samples\n"
            "3. Enter burner write offset manually (samples)\n"
            "4. No compensation (offset = 0)";
        bool choiceOk = false;
        const int c = GetMenuChoice("Write Offset Compensation", offsetMsg.c_str(),
            0, 4, 4, &choiceOk);
        if (!choiceOk || c == 0) return WRITE_OFFSET_BACK;
        switch (c) {
        case 1: chosen = -burnerReadOffset; break;
        case 2: chosen = burnerReadOffset; break;
        case 3: chosen = promptManualOffset(); break;
        case 4: chosen = 0; break;
        default: return WRITE_OFFSET_BACK;
        }
    }
    else {
        Console::Warning("The selected burner's read offset is unknown. "
            "No automatic write-offset recommendation is safe.\n");
        std::cout << "\n0. Back to menu\n";
        std::cout << "1. Enter burner write offset manually (samples)\n";
        std::cout << "2. No compensation (offset = 0)\n";
        std::cout << "Choice: ";
        bool choiceOk = false;
        const int c = GetMenuChoice("Write Offset Compensation",
            "The selected burner's offset is unknown.\n\n"
            "0. Back to menu\n"
            "1. Enter burner write offset manually (samples)\n"
            "2. No compensation (offset = 0)",
            0, 2, 2, &choiceOk);
        if (!choiceOk || c == 0) return WRITE_OFFSET_BACK;
        chosen = (c == 1) ? promptManualOffset() : 0;
    }

    if (chosen == WRITE_OFFSET_BACK) return WRITE_OFFSET_BACK;
    std::cout << "Will apply " << chosen << " samples ("
              << (chosen * 4) << " bytes) before burning.\n";
    if (chosen == 0) {
        std::cout << "Note: AccurateRip verification of the burned disc will likely fail\n";
        std::cout << "      unless your drive happens to have a zero combined offset.\n";
    }
    return chosen;
}

bool WriteTempCue(const std::wstring& cuePath, const std::wstring& binFileName,
    const std::vector<TrackSource>& sources, const DiscInfo& disc,
    const std::vector<int>& audioTrackIdx) {

    std::ofstream cue(cuePath);
    if (!cue) return false;

    if (!disc.cdText.albumTitle.empty())
        cue << "TITLE \"" << disc.cdText.albumTitle << "\"\n";
    if (!disc.cdText.albumArtist.empty())
        cue << "PERFORMER \"" << disc.cdText.albumArtist << "\"\n";

    // Use just the filename in the FILE directive — the .cue and .bin live
    // side-by-side in the temp folder.
    std::string narrowBin;
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, binFileName.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
			narrowBin.resize(len);
			if (WideCharToMultiByte(CP_UTF8, 0, binFileName.c_str(), -1,
				narrowBin.data(), len, nullptr, nullptr) != len)
				return false;
			narrowBin.pop_back();
        }
    }
    cue << "FILE \"" << narrowBin << "\" BINARY\n";

    for (size_t i = 0; i < sources.size(); i++) {
        const auto& ts = sources[i];
        const auto& tr = disc.tracks[audioTrackIdx[i]];

        cue << "  TRACK " << std::setfill('0') << std::setw(2) << tr.trackNumber << " AUDIO\n";
        cue << std::setfill(' ');

        // Per-track CD-Text
        if (tr.trackNumber > 0) {
            int idx0 = tr.trackNumber - 1;
            if (static_cast<size_t>(idx0) < disc.cdText.trackTitles.size() &&
                !disc.cdText.trackTitles[idx0].empty()) {
                cue << "    TITLE \"" << disc.cdText.trackTitles[idx0] << "\"\n";
            }
            if (static_cast<size_t>(idx0) < disc.cdText.trackArtists.size() &&
                !disc.cdText.trackArtists[idx0].empty()) {
                cue << "    PERFORMER \"" << disc.cdText.trackArtists[idx0] << "\"\n";
            }
        }
        if (!tr.isrc.empty()) {
            cue << "    ISRC " << tr.isrc << "\n";
        }

        if (ts.pregapSectors > 0) {
            std::ostringstream m;
            FormatMSF(ts.binPregapLBA, m);
            cue << "    INDEX 00 " << m.str() << "\n";
        }
        std::ostringstream m;
        FormatMSF(ts.binStartLBA, m);
        cue << "    INDEX 01 " << m.str() << "\n";
    }

    return cue.good();
}

// Wait until the drive reports media is ready (after disc swap).
bool WaitForDiscReady(OpticalDrive& copier, int timeoutSec) {
    DiscInfo probe;
    for (int i = 0; i < timeoutSec; i++) {
        if (InterruptHandler::Instance().IsInterrupted() ||
            InterruptHandler::Instance().CheckEscapeKey()) {
            return false;
        }
        // ReadTOC succeeds even on blank media when the drive reports no TOC
        // — for our purposes we just want to confirm the drive has settled.
        BYTE testCmd[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        BYTE sk = 0, asc = 0, ascq = 0;
        if (copier.GetDriveRef().SendSCSIWithSense(testCmd, sizeof(testCmd),
            nullptr, 0, &sk, &asc, &ascq, true)) {
            return true;
        }
        Sleep(1000);
    }
    return false;
}

void CleanupSources(std::vector<TrackSource>& sources) {
    for (auto& ts : sources) {
        if (!ts.tempWavPath.empty()) {
            DeleteFileW(ts.tempWavPath.c_str());
        }
    }
}

}  // namespace

void RunWriteTracksWorkflow(OpticalDrive& copier, DiscInfo& disc,
	const std::wstring& workDir, wchar_t& audioDrive, bool* outCompleted,
	bool* outDriveOrMediaChanged) {
	if (outCompleted) *outCompleted = false;
	if (outDriveOrMediaChanged) *outDriveOrMediaChanged = false;

    Console::Info("\n(Enter 0 at any prompt to go back to menu)\n");
    Console::BoxHeading("How this workflow uses discs");
    {
        char msg[320];
        std::snprintf(msg, sizeof(msg),
            "  1. SOURCE disc (the audio CD you want to copy) must be in drive %c: NOW.\n"
            "     Pregap timing and pregap audio are read from it.\n"
            "  2. After the source-disc reads finish, you'll be asked to insert a\n"
            "     BLANK CD-R or CD-RW (same drive, or a different burner drive).\n",
            static_cast<char>(audioDrive));
        Console::Info(msg);
    }

    {
        std::vector<std::string> preflightWarnings;
        if (!copier.RunPreflightChecks(disc, preflightWarnings)) {
            std::string msg = "Source disc preflight reported issues:\n";
            for (const auto& w : preflightWarnings) {
                msg += "  - " + w + "\n";
            }
            msg += "\nContinue anyway?";
            if (!GuiInput::PromptYesNo("Disc preflight", msg.c_str())) {
                Console::Info("Write cancelled.\n");
                return;
            }
        }
    }

    // ── 1. Validate disc has audio tracks ──────────────────────────────
    std::vector<int> audioTrackIdx;
    for (int i = 0; i < static_cast<int>(disc.tracks.size()); i++) {
        if (disc.tracks[i].isAudio) audioTrackIdx.push_back(i);
    }
    if (audioTrackIdx.empty()) {
        Console::Error("No audio tracks on this disc - pregap layout cannot be derived.\n");
        return;
    }

    // ── 2. Pick burner drive up front ──────────────────────────────────
    // Mirrors the Write Disc (option 3) flow: surface the drive selector now
    // so the user knows which drive will burn before sitting through the long
    // source-disc reads. The actual swap (close source / open burner) is
    // deferred until after the source disc has been fully read.
    wchar_t burnerDrive = audioDrive;
    {
        std::vector<wchar_t> audioDrives;
        // Quiet scan — SelectWriterDrive prints the numbered list itself.
        std::vector<wchar_t> cdDrives = ScanDrives(audioDrives, /*verbose=*/false);
        if (cdDrives.empty()) {
            Console::Error("No CD/DVD drives detected.\n");
            return;
        }
        if (cdDrives.size() > 1) {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                "\nSource disc is in drive %c:. Pick the BURNER drive next "
                "(it can be the same drive — you'll swap discs later — or a "
                "different drive on this system).\n",
                static_cast<char>(audioDrive));
            Console::Info(msg);
            burnerDrive = SelectWriterDrive(cdDrives, audioDrive);
            if (!burnerDrive) {
                Console::Info("Write cancelled.\n");
                return;
            }
            if (burnerDrive == audioDrive) {
                Console::Info("Same drive selected — you'll be prompted to "
                    "eject the source and insert a blank after the reads finish.\n");
            }
            else {
                char m2[160];
                std::snprintf(m2, sizeof(m2),
                    "Burner drive %c: selected — the source disc can stay in "
                    "drive %c: while you load a blank into %c:.\n",
                    static_cast<char>(burnerDrive),
                    static_cast<char>(audioDrive),
                    static_cast<char>(burnerDrive));
                Console::Info(m2);
            }
        }
        PrintDriveIdentity(burnerDrive, "Selected burner drive");
        if (burnerDrive == audioDrive) {
            // Same drive — confirm it actually supports writing before we
            // commit to all the source-disc reads. (Cross-drive: write-
            // capability is checked after the swap, when we're open on the
            // burner.)
            DriveCapabilities caps;
            if (copier.DetectDriveCapabilities(caps) &&
				!(caps.writesCDR || caps.writesCDRW)) {
				Console::Error("Drive does not support CD-R/CD-RW writing.\n");
                return;
            }
        }
    }

    // ── 3. Show pregap layout from source TOC ──────────────────────────
    Console::BoxHeading("Source disc pregap layout");
    int totalPregapFrames = 0;
    int tracksWithPregap = 0;
    for (size_t i = 0; i < audioTrackIdx.size(); i++) {
        const auto& tr = disc.tracks[audioTrackIdx[i]];
        DWORD pregap = 0;
        if (i > 0 && tr.pregapLBA > 0 && tr.pregapLBA < tr.startLBA) {
            pregap = tr.startLBA - tr.pregapLBA;
        }
        DWORD sectors = (tr.endLBA >= tr.startLBA) ? (tr.endLBA - tr.startLBA + 1) : 0;
        int sec = static_cast<int>(sectors / 75);
        std::cout << "  Track " << std::setw(2) << tr.trackNumber
            << "  " << std::setw(2) << sec / 60 << ":"
            << std::setfill('0') << std::setw(2) << sec % 60 << std::setfill(' ');
        if (pregap > 0) {
            std::cout << "  Pregap " << std::setw(2) << pregap / 75 << "s "
                << std::setw(2) << pregap % 75 << "f";
            totalPregapFrames += static_cast<int>(pregap);
            tracksWithPregap++;
        }
        std::cout << "\n";
    }
    Console::Info("Total: ");
    std::cout << audioTrackIdx.size() << " audio track(s), "
        << tracksWithPregap << " with internal pregap ("
        << totalPregapFrames << " frames silence to insert)\n";

    // ── 4. Prompt for input folder via native folder picker ────────────
    Console::Info("\nChoose the folder containing the ripped track files (.wav / .flac)...\n");
    Console::Info("Files are matched to audio tracks in alphabetical order.\n");
    std::wstring folder = GuiInput::PromptForFolder(
        L"Choose folder with track files (.wav / .flac)", workDir);
    if (folder.empty()) {
        Console::Info("Cancelled (no source folder selected).\n");
        return;
    }
    folder = NormalizePath(folder);
    while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();

    std::vector<std::wstring> files = ScanAudioFiles(folder);
    if (files.empty()) {
        Console::Error("No .wav or .flac files found in folder.\n");
        return;
    }
    if (files.size() != audioTrackIdx.size()) {
        Console::Error("File count mismatch: ");
        std::cout << files.size() << " audio file(s) in folder, but disc has "
            << audioTrackIdx.size() << " audio track(s).\n";
        return;
    }

    Console::Success("Matched files:\n");
    for (size_t i = 0; i < files.size(); i++) {
        std::cout << "  Track " << std::setw(2)
            << disc.tracks[audioTrackIdx[i]].trackNumber << " <- ";
        size_t slash = files[i].find_last_of(L"\\/");
        std::wcout << (slash == std::wstring::npos ? files[i] : files[i].substr(slash + 1)) << L"\n";
    }

    // ── 5. Probe / decode each file ────────────────────────────────────
    std::vector<TrackSource> sources(files.size());
    bool anyMismatch = false;
    for (size_t i = 0; i < files.size(); i++) {
        TrackSource& ts = sources[i];
        ts.originalPath = files[i];

        if (EndsWithLower(files[i], L".flac")) {
            std::wstring tempWav = workDir + L"\\_writetracks_in_" +
                std::to_wstring(i) + L".wav";
            Console::Info("Decoding ");
            size_t slash = files[i].find_last_of(L"\\/");
            std::wcout << (slash == std::wstring::npos ? files[i] : files[i].substr(slash + 1));
            std::cout << "...\n";
            ts.tempWavPath = tempWav;
            if (!DecodeFlacToWav(files[i], tempWav)) {
                Console::Error("FLAC decode failed (is flac.exe on PATH?). Aborting.\n");
                CleanupSources(sources);
                return;
            }
            ts.wavPath = tempWav;
        }
        else {
            ts.wavPath = files[i];
        }

        std::string err;
        if (!ProbeWavFile(ts.wavPath, ts, err)) {
            Console::Error("Track ");
            std::cout << (i + 1) << ": " << err << "\n";
            CleanupSources(sources);
            return;
        }

		const uint64_t sectorCount =
			(static_cast<uint64_t>(ts.dataBytes) + AUDIO_SECTOR_SIZE - 1)
			/ AUDIO_SECTOR_SIZE;
		if (sectorCount > MAXDWORD) {
			Console::Error("Track is too large to represent as CD sectors.\n");
			CleanupSources(sources);
			return;
		}
		ts.sectorCount = static_cast<DWORD>(sectorCount);

        const auto& tr = disc.tracks[audioTrackIdx[i]];
        DWORD expectedSectors = (tr.endLBA >= tr.startLBA) ? (tr.endLBA - tr.startLBA + 1) : 0;
        if (expectedSectors != 0 && ts.sectorCount != expectedSectors) {
            anyMismatch = true;
            Console::Warning("Track ");
            std::cout << tr.trackNumber << ": expected " << expectedSectors
                << " sectors from TOC, file has " << ts.sectorCount << "\n";
        }
    }

    if (anyMismatch) {
        Console::Warning("Track lengths differ from the source TOC - the new disc's track\n");
        Console::Info("boundaries will follow the FILE lengths, not the source TOC.\n");
        if (!GuiInput::PromptYesNo("Continue?",
            "Track lengths differ from the source TOC. Continue?")) {
            CleanupSources(sources);
            return;
        }
    }

    // ── 6. Drive read offset + read pregap audio from source disc ──────
    // The rip workflow uses PregapMode::Skip, so the WAV files don't contain
    // the audio that lived between INDEX 00 and INDEX 01 of each track.  For
    // discs with non-silent pregaps (live albums, continuous mixes), filling
    // those regions with silence shifts the *previous* track's AccurateRip
    // CRC range — AR's CRC for track i covers [startLBA[i], startLBA[i+1]-1],
    // which includes track i+1's pregap.  We capture that audio fresh from
    // the source disc here, with read-offset correction so it aligns with
    // the WAV samples (which were also offset-corrected at rip time).
    int sourceReadOffset = 0;
    bool sourceReadOffsetKnown = false;
    {
    PioneerVendor pioneerProbe(copier.GetDriveRef());
    const bool isPioneer = pioneerProbe.IsPioneerDrive();
    PioneerPureReadModeGuard sourceReadPureRead(copier.GetDriveRef(), isPioneer,
        PureReadMode::Perfect, /*requestRealTime=*/true);
    if (isPioneer) {
        if (sourceReadPureRead.engaged())
            Console::Success("Pioneer PureRead Perfect confirmed for source pregap reads.\n");
        else
            Console::Warning("Pioneer PureRead Perfect could not be enabled for source pregap reads.\n");
    }

    Console::Info("\nDetecting drive read offset...\n");
    OffsetDetectionResult sourceOffsetResult;
    sourceReadOffsetKnown = copier.DetectDriveOffset(sourceOffsetResult);
    sourceReadOffset = sourceOffsetResult.offset;
    if (sourceReadOffsetKnown) {
        Console::Success("Source-drive read offset detected: ");
        std::cout << sourceReadOffset << " samples ("
            << sourceOffsetResult.details << ")\n";
    }
    else {
        Console::Warning("Source-drive read offset is unknown; using 0 samples "
            "for pregap alignment.\n");
    }
    if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
        Console::Warning("\n*** Cancelled by user ***\n");
        CleanupSources(sources);
        return;
    }

    int pregapTracks = 0;
    for (size_t i = 1; i < sources.size(); i++) {
        const auto& tr = disc.tracks[audioTrackIdx[i]];
        if (tr.pregapLBA > 0 && tr.pregapLBA < tr.startLBA) pregapTracks++;
    }

    DWORD marginSectors = ComputeMarginSectors(sourceReadOffset);
    int boundaryReadFallbacks = 0;
    int boundaryReadFailures = 0;
    if (pregapTracks > 0) {
        Console::Info("Reading pregap audio from source disc (");
        std::cout << pregapTracks << " track(s) with pregap)...\n";
        int processed = 0;
        for (size_t i = 1; i < sources.size(); i++) {
            const auto& tr = disc.tracks[audioTrackIdx[i]];
            if (!(tr.pregapLBA > 0 && tr.pregapLBA < tr.startLBA)) continue;
            processed++;

            std::cout << "  [" << processed << "/" << pregapTracks << "] track "
                << tr.trackNumber << " (" << (tr.startLBA - tr.pregapLBA)
                << " sectors)... " << std::flush;

            bool pregapOk = ReadPregapAudio(copier, tr.pregapLBA, tr.startLBA - 1,
                sourceReadOffset, marginSectors,
                sources[i].pregapAudio, sources[i].headOverlap);
            if (pregapOk) {
                std::cout << "ok\n";
            }
            else {
                if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
                    std::cout << "cancelled\n";
                    Console::Warning("\n*** Cancelled by user ***\n");
                    CleanupSources(sources);
                    return;
                }
                // Pregap audio itself unreadable (some drives refuse INDEX 00
                // sectors) — fall back to silence pregap, but still try to grab
                // just the boundary-overlap sectors so we can repair the
                // previous track's gap-corrupted last WAV sector.
                sources[i].pregapAudio.clear();
                if (ReadBoundaryOverlap(copier, tr.pregapLBA, sourceReadOffset,
                    marginSectors, sources[i].headOverlap)) {
                    Console::Warning("pregap read failed - silence pregap, "
                        "boundary repair OK\n");
                    boundaryReadFallbacks++;
                }
                else {
                    if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
                        Console::Warning("\n*** Cancelled by user ***\n");
                        CleanupSources(sources);
                        return;
                    }
                    Console::Error("pregap + boundary read failed\n");
                    boundaryReadFailures++;
                }
            }
        }
        if (boundaryReadFallbacks > 0) {
            Console::Info("");
            std::cout << boundaryReadFallbacks
                << " track(s) used silence-pregap + boundary repair (drive "
                "refused INDEX 00 sectors).\n";
        }
        if (boundaryReadFailures > 0) {
            Console::Warning("");
            std::cout << boundaryReadFailures
                << " track(s) had unreadable boundary sectors - burned disc "
                "may have CRC mismatches at those track boundaries.\n";
        }
    }

    // ── 7. Write-offset compensation ───────────────────────────────────
    if (isPioneer && !sourceReadPureRead.Restore()) {
        Console::Error("Failed to restore the source drive's previous PureRead "
            "state. Burn aborted.\n");
        CleanupSources(sources);
        return;
    }
    } // Source-drive PureRead is restored before querying or opening the burner.

    int burnerReadOffset = 0;
    bool burnerReadOffsetKnown = false;
    if (burnerDrive == audioDrive) {
        burnerReadOffset = sourceReadOffset;
        burnerReadOffsetKnown = sourceReadOffsetKnown;
    }
    else {
        Console::Info("Looking up the selected burner's AccurateRip read offset...\n");
        std::string burnerOffsetSource;
        burnerReadOffsetKnown = LookupDriveReadOffset(burnerDrive,
            burnerReadOffset, burnerOffsetSource);
        if (burnerReadOffsetKnown) {
            Console::Success("Selected-burner read offset: ");
            std::cout << burnerReadOffset << " samples (" << burnerOffsetSource << ")\n";
        }
        else {
            Console::Warning("No AccurateRip offset entry was found for the selected burner.\n");
        }
    }

    int writeOffsetCompensation = SelectWriteOffset(burnerReadOffset,
        burnerReadOffsetKnown);
    if (writeOffsetCompensation == WRITE_OFFSET_BACK) {
        CleanupSources(sources);
        return;
    }

    // ── 8. Compute BIN layout ──────────────────────────────────────────
    DWORD cursor = 0;
    for (size_t i = 0; i < sources.size(); i++) {
        const auto& tr = disc.tracks[audioTrackIdx[i]];
        DWORD pregap = 0;
        // Track 1 has no internal pregap in the BIN — the standard 150-frame
        // pregap is added by WriteAudioSectors.
        if (i > 0 && tr.pregapLBA > 0 && tr.pregapLBA < tr.startLBA) {
            pregap = tr.startLBA - tr.pregapLBA;
        }
        sources[i].pregapSectors = pregap;
        if (pregap > 0) {
            sources[i].binPregapLBA = cursor;
            cursor += pregap;
        }
        sources[i].binStartLBA = cursor;
        cursor += sources[i].sectorCount;
    }
    DWORD totalBinSectors = cursor;

    Console::Info("Building temp image (");
    std::cout << totalBinSectors << " sectors, "
        << (static_cast<long long>(totalBinSectors) * AUDIO_SECTOR_SIZE / (1024 * 1024))
        << " MB)...\n";

    // ── 9. Build BIN sectors in memory, apply offset, write to disk ────
    std::wstring binPath = workDir + L"\\_writetracks_temp.bin";
    std::wstring cuePath = workDir + L"\\_writetracks_temp.cue";

    if (!WriteSourcesToBin(binPath, sources)) {
        Console::Error("Failed writing temp BIN file.\n");
        DeleteFileW(binPath.c_str());
        CleanupSources(sources);
        return;
    }

    int streamedBoundaryRepairs = 0;
    if (!PatchBoundaryOverlaps(binPath, sources, streamedBoundaryRepairs)) {
        Console::Error("Failed repairing track boundaries in temp BIN.\n");
        DeleteFileW(binPath.c_str());
        CleanupSources(sources);
        return;
    }
    if (streamedBoundaryRepairs > 0) {
        std::cout << "Repaired " << streamedBoundaryRepairs
            << " track boundary(ies) (offset-correction gap fix).\n";
    }

    if (writeOffsetCompensation != 0) {
        Console::Info("Applying write-offset compensation: ");
        std::cout << writeOffsetCompensation << " samples ("
            << (writeOffsetCompensation * 4) << " bytes)...\n";
        const uint64_t totalBytes = static_cast<uint64_t>(totalBinSectors) * AUDIO_SECTOR_SIZE;
        if (!ApplyFileSampleOffset(binPath, writeOffsetCompensation, totalBytes)) {
            Console::Error("Failed applying write-offset compensation.\n");
            DeleteFileW(binPath.c_str());
            CleanupSources(sources);
            return;
        }
    }

#if 0 // Replaced by bounded file streaming above.
    {
        std::vector<std::vector<BYTE>> binSectors;
        binSectors.reserve(totalBinSectors);

        for (size_t i = 0; i < sources.size(); i++) {
            if (sources[i].pregapSectors > 0) {
                if (sources[i].pregapAudio.size() == sources[i].pregapSectors) {
                    // Use the audio captured fresh from the source disc.
                    for (auto& sec : sources[i].pregapAudio) {
                        binSectors.push_back(std::move(sec));
                    }
                    sources[i].pregapAudio.clear();
                }
                else {
                    // Fallback: silence (read failed, or never attempted).
                    AppendSilenceSectors(binSectors, sources[i].pregapSectors);
                }
            }
            if (!AppendWavToSectors(binSectors, sources[i])) {
                Console::Error("Failed reading WAV for track ");
                std::cout << disc.tracks[audioTrackIdx[i]].trackNumber << "\n";
                CleanupSources(sources);
                return;
            }
        }

        // Fix boundary sectors corrupted by offset correction across gaps.
        // The WAV rip used PregapMode::Skip, so the rawSectors stream had
        // gaps at each pregap.  ApplyOffsetCorrection treated it as contiguous,
        // bleeding samples across those gaps — the very last sector of each
        // pregap-preceded WAV got its trailing `byteOffset` bytes filled from
        // the wrong disc location (track i+1's INDEX 01 audio instead of the
        // pregap's first sample).  headOverlap holds the cleanly offset-
        // corrected version of those sectors, read CONTIGUOUSLY across the
        // boundary so no bleed occurs.
        //
        // We do NOT touch the WAV's first sectors: track i+1's first WAV
        // sector is clean (no gap before it in the rip stream), so replacing
        // it with re-read data would only introduce read-pass-to-read-pass
        // variance, not fix anything.
        int boundaryRepairs = 0;
        for (size_t i = 1; i < sources.size(); i++) {
            if (sources[i].headOverlap.empty()) continue;
            DWORD pos = sources[i].binPregapLBA;
            DWORD count = static_cast<DWORD>(sources[i].headOverlap.size());
            if (pos < count) {
                sources[i].headOverlap.clear();
                continue;
            }
            DWORD startPos = pos - count;
            for (DWORD j = 0; j < count; j++) {
                if (startPos + j < binSectors.size()) {
                    binSectors[startPos + j] = std::move(sources[i].headOverlap[j]);
                }
            }
            sources[i].headOverlap.clear();
            boundaryRepairs++;
        }
        if (boundaryRepairs > 0) {
            Console::Info("");
            std::cout << "Repaired " << boundaryRepairs
                << " track boundary(ies) (offset-correction gap fix).\n";
        }

        if (writeOffsetCompensation != 0) {
            Console::Info("Applying write-offset compensation: ");
            std::cout << writeOffsetCompensation << " samples ("
                << (writeOffsetCompensation * 4) << " bytes)...\n";
            copier.ApplySampleOffset(binSectors, writeOffsetCompensation);
        }

        if (!WriteSectorsToBin(binPath, binSectors)) {
            Console::Error("Failed writing temp BIN file.\n");
            DeleteFileW(binPath.c_str());
            CleanupSources(sources);
            return;
        }
    }  // binSectors freed here
#endif

    if (!WriteTempCue(cuePath, L"_writetracks_temp.bin", sources, disc, audioTrackIdx)) {
        Console::Error("Failed writing temp CUE file.\n");
        DeleteFileW(binPath.c_str());
        DeleteFileW(cuePath.c_str());
        CleanupSources(sources);
        return;
    }

    Console::Success("Temp image ready.\n");

    // The decoded WAV temp files for FLAC inputs are no longer needed once
    // the BIN has been built.
    CleanupSources(sources);

    auto removeTemps = [&]() {
        DeleteFileW(binPath.c_str());
        DeleteFileW(cuePath.c_str());
    };

    // ── 10. Load blank into the burner drive (chosen at step 2) ────────
    if (burnerDrive == audioDrive) {
        // Same drive — eject the source disc and wait for the user to insert a blank.
        Console::BoxHeading("Time to swap discs");
        char info[200];
        std::snprintf(info, sizeof(info),
            "Drive %c: will eject. Remove the SOURCE disc and load a BLANK "
            "CD-R or CD-RW into the same drive, then click Yes.\n",
            static_cast<char>(audioDrive));
        Console::Info(info);
        const bool sourceEjected = copier.Eject();
        if (sourceEjected && outDriveOrMediaChanged) {
            *outDriveOrMediaChanged = true;
        }

        char prompt[200];
        std::snprintf(prompt, sizeof(prompt),
            "Tray ejected. Remove the source disc and insert a blank CD-R/CD-RW into "
            "drive %c:, then click Yes to continue (No = cancel).",
            static_cast<char>(audioDrive));
        if (!GuiInput::PromptYesNo("Insert blank disc", prompt)) {
            Console::Info("Cancelled.\n");
            removeTemps();
            return;
        }
        if (outDriveOrMediaChanged) *outDriveOrMediaChanged = true;

        // Reopen so the drive re-detects the new media.
        copier.Close();
        Sleep(2000);
        if (!copier.Open(audioDrive)) {
            Console::Error("Failed to reopen drive after disc swap.\n");
            removeTemps();
            return;
        }
        if (!WaitForDiscReady(copier, 30)) {
            Console::Error("Drive did not become ready with the new disc.\n");
            removeTemps();
            return;
        }
        PrintDriveIdentity(audioDrive, "Using burner drive");
    }
    else {
        // Different drive — close source drive, open burner, prompt for blank
        // if the burner has no media loaded yet.
        wchar_t sourceDrive = audioDrive;
        Console::BoxHeading("Switching to burner drive");
        char switchMsg[240];
        std::snprintf(switchMsg, sizeof(switchMsg),
            "The source disc can stay in drive %c: (it's been fully read).\n"
            "Load a BLANK CD-R or CD-RW into drive %c: for burning.\n",
            static_cast<char>(sourceDrive),
            static_cast<char>(burnerDrive));
        Console::Info(switchMsg);

        copier.Close();
        if (!copier.Open(burnerDrive)) {
            Console::Error("Failed to open burner drive.\n");
            // Reopen the original audio drive so the rest of the program still has one.
            if (!copier.Open(audioDrive) && outDriveOrMediaChanged) {
                *outDriveOrMediaChanged = true;
            }
            removeTemps();
            return;
        }
        if (outDriveOrMediaChanged) *outDriveOrMediaChanged = true;
        audioDrive = burnerDrive;
        PrintDriveIdentity(audioDrive, "Using burner drive");

        if (!copier.GetDriveRef().TestUnitReady()) {
            char prompt[200];
            std::snprintf(prompt, sizeof(prompt),
                "Insert a blank CD-R or CD-RW into drive %c: (the burner) and click OK. "
                "Leave the source disc in drive %c: alone.",
                static_cast<char>(burnerDrive),
                static_cast<char>(sourceDrive));
            if (!GuiInput::PromptYesNo("Insert disc", prompt)) {
                Console::Info("Cancelled.\n");
                removeTemps();
                return;
            }
            if (!copier.GetDriveRef().WaitForDriveReady(30)) {
                Console::Error("Drive did not become ready with a disc.\n");
                removeTemps();
                return;
            }
        }

        // Now that we're open on the burner, verify it actually supports writing.
        DriveCapabilities caps;
        if (copier.DetectDriveCapabilities(caps) &&
			!(caps.writesCDR || caps.writesCDRW)) {
			Console::Error("Selected burner drive does not support CD-R/CD-RW writing.\n");
            removeTemps();
            return;
        }
    }

    // ── 11. Detect rewritable / blank if necessary ─────────────────────
    // Quiet: WriteDisc re-reports the media type just before the burn, so
    // suppress the duplicate readout here.
    bool isFull = false, isRewritable = false;
    if (!copier.CheckRewritableDisk(isFull, isRewritable, /*quiet=*/true)) {
        Console::Error("Cannot determine inserted disc type.\n");
        removeTemps();
        return;
    }
    if (isFull && !isRewritable) {
        Console::Error("Inserted disc is full and not rewritable.\n");
        removeTemps();
        return;
    }

    int eraseSpeed = -1;
    bool wasBlanked = false;

    if (isRewritable && isFull) {
        Console::Warning("CD-RW is full. Erase first?\n");
        std::cout << "1. Quick erase\n2. Full erase\n3. Cancel\nChoice: ";
        bool ok = false;
        int c = GetMenuChoice("Erase full CD-RW before writing?",
            "1. Quick erase\n"
            "2. Full erase\n"
            "3. Cancel",
            1, 3, 1, &ok);
        if (!ok || c == 3) { Console::Info("Cancelled.\n"); removeTemps(); return; }
		eraseSpeed = copier.SelectWriteSpeed();
		if (!copier.BlankRewritableDisk(eraseSpeed, c == 1)) { removeTemps(); return; }
        wasBlanked = true;
    }
    else if (isRewritable && !isFull) {
        Console::Info("CD-RW with available space.\n");
        std::cout << "1. Write directly\n2. Quick erase first\n3. Full erase first\nChoice: ";
        bool ok = false;
        int c = GetMenuChoice("CD-RW with free space - what now?",
            "1. Write directly\n"
            "2. Quick erase first\n"
            "3. Full erase first",
            1, 3, 1, &ok);
        if (!ok) { Console::Info("Cancelled.\n"); removeTemps(); return; }
        if (c == 2 || c == 3) {
			eraseSpeed = copier.SelectWriteSpeed();
			if (!copier.BlankRewritableDisk(eraseSpeed, c == 2)) { removeTemps(); return; }
            wasBlanked = true;
        }
    }

    int speed;
    if (wasBlanked) {
        Console::Info("Reusing previously selected write speed (");
        std::cout << eraseSpeed << "x)\n";
        speed = eraseSpeed;
    }
    else {
        speed = copier.SelectWriteSpeed();
    }

    Console::Info("\nUse power calibration?\n1. Yes (recommended)\n2. No\nChoice: ");
    bool calibrationOk = false;
    int calibChoice = GetMenuChoice("Use power calibration?",
        "Optical Power Calibration tunes laser power to the loaded disc.\n\n"
        "1. Yes (recommended)\n"
        "2. No",
        1, 2, 1, &calibrationOk);
    if (!calibrationOk) {
        Console::Info("Write cancelled.\n");
        removeTemps();
        return;
    }
    bool useCal = (calibChoice == 1);

    // ── 12. Burn ───────────────────────────────────────────────────────
	bool ok = copier.WriteDisc(binPath, cuePath, L"", speed, useCal, wasBlanked);
	if (ok) {
		if (outCompleted) *outCompleted = true;
        Console::Success("Disc write completed successfully.\n");
        Console::Info("Pregap durations from the source disc were preserved (silence).\n");
    }
    else {
        Console::Error("Disc write failed.\n");
    }

    removeTemps();
}
