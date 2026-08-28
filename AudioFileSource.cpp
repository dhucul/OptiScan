// ============================================================================
// AudioFileSource.cpp - Shared WAV/FLAC input handling for the write workflows.
// See AudioFileSource.h. Bodies here were lifted verbatim out of
// WriteTracksWorkflow.cpp's anonymous namespace when the CUE-sheet write
// workflow needed the same input handling.
// ============================================================================
#define NOMINMAX
#include "AudioFileSource.h"
#include "ConsoleColors.h"
#include "Constants.h"
#include "GuiInput.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include "ScsiDrive.h"
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iostream>

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
bool ProbeWavFile(const std::wstring& path, WavPayload& ts, std::string& err) {
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

// ============================================================================
// FLAC decoder discovery
// ============================================================================
bool FlacDecoderAvailable() {
    wchar_t resolved[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"flac.exe", nullptr, MAX_PATH, resolved, nullptr) > 0)
        return true;

    // Also accept a copy sitting beside OptiScan.exe, which is how a portable
    // install ships it. SearchPathW already covers the current directory, but
    // the working directory is not the module directory once the user has
    // picked an output folder.
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    std::wstring beside(modulePath, len);
    const size_t slash = beside.find_last_of(L"\/");
    if (slash == std::wstring::npos) return false;
    beside.replace(slash + 1, std::wstring::npos, L"flac.exe");
    return GetFileAttributesW(beside.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ============================================================================
// PrepareAudioFile - decode if needed, probe, and size in sectors
// ============================================================================
bool PrepareAudioFile(const std::wstring& path, const std::wstring& tempWavPath,
    WavPayload& out, std::string& err) {
    err.clear();
    out = WavPayload{};
    out.originalPath = path;

    if (EndsWithLower(path, L".flac")) {
        if (!DecodeFlacToWav(path, tempWavPath)) {
            // Distinguish "no decoder" from "decoder rejected this file" so the
            // user isn't told to install FLAC when FLAC is already installed.
            err = FlacDecoderAvailable()
                ? "FLAC decode failed (corrupt file, or not a 16-bit 44.1 kHz stereo stream)"
                : "flac.exe was not found on PATH";
            return false;
        }
        out.tempWavPath = tempWavPath;
        out.wavPath = tempWavPath;
    }
    else if (EndsWithLower(path, L".wav")) {
        out.wavPath = path;
    }
    else {
        err = "unsupported audio format (only .wav and .flac are accepted)";
        return false;
    }

    if (!ProbeWavFile(out.wavPath, out, err)) return false;

    // ceil(dataBytes / 2352) without overflowing: dataBytes is a DWORD, and a
    // full 99-minute disc is only ~450k sectors, so the sum cannot wrap.
    out.sectorCount = (out.dataBytes + AUDIO_SECTOR_SIZE - 1) / AUDIO_SECTOR_SIZE;
    if (out.sectorCount == 0) {
        err = "file contains no audio";
        return false;
    }
    return true;
}

void CleanupTempWavs(std::vector<WavPayload>& payloads) {
    for (auto& p : payloads) {
        if (p.tempWavPath.empty()) continue;
        DeleteFileW(p.tempWavPath.c_str());
        // The payload is no longer readable, so make that unmistakable rather
        // than leaving wavPath pointing at a file we just deleted.
        if (p.wavPath == p.tempWavPath) p.wavPath.clear();
        p.tempWavPath.clear();
    }
}
