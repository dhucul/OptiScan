#define NOMINMAX
#include "ImagePregapPreparation.h"
#include "ImageSource.h"
#include "CueSheetImport.h"
#include "Constants.h"
#include "InterruptHandler.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>

namespace {
bool Utf8Path(const std::string& text, std::wstring& wide) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return false;
    wide.resize(length);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), wide.data(), length) == length;
}
bool SectorCount(const std::filesystem::path& path, uint32_t& sectors) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes == 0 || bytes % AUDIO_SECTOR_SIZE || bytes / AUDIO_SECTOR_SIZE >= kUnburnableSectors) return false;
    sectors = static_cast<uint32_t>(bytes / AUDIO_SECTOR_SIZE);
    return true;
}
}

bool PrepareImagePregaps(const std::wstring& cuePath, PreparedImageSource& output, std::string& error) {
    output = PreparedImageSource{};
    if (g_interrupt.IsInterrupted()) { error = "Image preparation cancelled."; return false; }
    if (!ResolveCueImage(cuePath, output.sourceBin, error)) return false;
    output.binFile = output.sourceBin;
    output.cueFile = cuePath;
    auto companion = std::filesystem::path(output.sourceBin).replace_extension(L".sub");
    std::error_code ec;
    if (std::filesystem::is_regular_file(companion, ec)) output.subFile = companion.wstring();

    std::ifstream cue(std::filesystem::path(cuePath), std::ios::binary);
    std::map<int, std::filesystem::path> savedGaps;
    bool generated = false, archival = false;
    std::string line;
    while (std::getline(cue, line)) {
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        std::istringstream row(line);
        std::string command, marker;
        row >> command;
        if (command == "PREGAP" || command == "POSTGAP") generated = true;
        if (command != "REM") continue;
        row >> marker;
        if (marker == "ACCURATERIPID") archival = true;
        if (marker == "OPTISCAN_PREGAP_UNKNOWN") {
            error = "The source has undetermined pregaps. Rescan with usable Q data before an exact-layout write.";
            return false;
        }
        if (marker == "OPTISCAN_GAPS_DISCARDED") {
            error = "Original gap audio was discarded. Copy again with Include or Separate; silence cannot restore missing audio.";
            return false;
        }
        if (marker == "OPTISCAN_PREGAP_FILE") {
            int track = 0;
            std::string name;
            std::wstring wide;
            if (!(row >> track >> std::quoted(name, '"', '\0')) || track < 1 || track > 99 ||
                savedGaps.count(track) || !Utf8Path(name, wide)) {
                error = "Invalid separate-pregap file reference."; return false;
            }
            savedGaps.emplace(track, std::filesystem::path(cuePath).parent_path() / wide);
        }
    }
    if (cue.bad()) { error = "Could not read the CUE completely."; return false; }
    if (!generated && savedGaps.empty()) return true;
    if (archival && savedGaps.empty()) {
        error = "This older image omits gap audio and does not explicitly link separate gap files. Use an Include image or re-save as Separate.";
        return false;
    }

    CueImportSheet sheet;
    if (!ParseCueSheetForImport(cuePath, sheet, error)) return false;
    if (sheet.files.size() != 1 || sheet.files.front().typeKeyword != L"BINARY") {
        error = "Image pregap preparation requires one BINARY source."; return false;
    }
    uint32_t sourceSectors = 0;
    if (!SectorCount(output.sourceBin, sourceSectors)) {
        error = "Source BIN size is not a valid whole-sector image."; return false;
    }
    std::map<int, uint32_t> gapLengths;
    for (const auto& [number, path] : savedGaps) {
        auto track = std::find_if(sheet.tracks.begin(), sheet.tracks.end(),
            [number](const auto& value) { return value.number == number; });
        uint32_t length = 0;
        if (track == sheet.tracks.end() || !track->isAudio || track->hasIndex00() ||
            !SectorCount(path, length)) {
            error = "A separate gap is missing, malformed, or incompatible with the CUE."; return false;
        }
        if (track == sheet.tracks.begin()) {
            // Separate-mode sidecars contain program-area audio, in addition
            // to the 150 negative-LBA sectors the unchanged SAO writer emits.
            track->pregapFrames = length + 150;
        } else if (track->pregapFrames != length) {
            error = "Separate gap length does not match its CUE declaration."; return false;
        }
        gapLengths[number] = length;
    }
    if (!savedGaps.empty() && std::any_of(sheet.tracks.begin(), sheet.tracks.end(),
        [](const auto& track) { return track.postgapFrames != 0; })) {
        error = "Separate gap files cannot be combined with POSTGAP in the same image."; return false;
    }
    if (archival) {
        for (const auto& track : sheet.tracks)
            if (track.pregapFrames && !savedGaps.count(track.number)) {
                error = "An archival pregap has no explicitly linked audio file."; return false;
            }
    }

    std::vector<BinSegment> segments;
    std::vector<TrackPlacement> placements;
    uint64_t totalSectors = 0;
    if (!BuildBinLayout(sheet, {sourceSectors}, segments, placements, totalSectors, error)) return false;
    struct Gap { std::filesystem::path path; uint32_t sectors; };
    std::map<uint64_t, Gap> gapAt;
    for (size_t i = 0; i < placements.size(); ++i) {
        const int number = sheet.tracks[i].number;
        if (savedGaps.count(number)) gapAt.emplace(placements[i].binIndex00,
            Gap{savedGaps.at(number), gapLengths.at(number)});
    }
    const auto tempRoot = std::filesystem::temp_directory_path(ec);
    if (ec) { error = "Temporary directory unavailable."; return false; }
    output.temporary = std::make_unique<ArtifactTransaction>(tempRoot / L"OptiScan-pregaps");
    const auto binPath = output.temporary->ScratchFile(L"image.bin");
    const auto cueOutput = output.temporary->ScratchFile(L"image.cue");
    if (binPath.empty() || cueOutput.empty()) { error = "Cannot stage the pregap image."; return false; }
    std::ofstream binary(binPath, std::ios::binary);
    std::ifstream source(output.sourceBin, std::ios::binary);
    if (!binary || !source) { error = "Cannot open image streams."; return false; }
    std::vector<char> buffer(64 * AUDIO_SECTOR_SIZE, 0);
    uint64_t written = 0;
    for (const auto& segment : segments) {
        if (g_interrupt.IsInterrupted()) { error = "Image preparation cancelled."; return false; }
        std::ifstream gap;
        std::istream* input = nullptr;
        if (segment.kind == BinSegment::Kind::FileRange) {
            source.clear();
            source.seekg(static_cast<std::streamoff>(segment.startSector) * AUDIO_SECTOR_SIZE);
            if (!source) { error = "Cannot seek the BIN source."; return false; }
            input = &source;
        } else if (auto found = gapAt.find(written); found != gapAt.end()) {
            if (found->second.sectors != segment.sectorCount) {
                error = "Separate gap does not match the resolved layout."; return false;
            }
            gap.open(found->second.path, std::ios::binary);
            if (!gap) { error = "Cannot open separate pregap audio."; return false; }
            input = &gap;
            gapAt.erase(found);
        }
        uint64_t bytes = static_cast<uint64_t>(segment.sectorCount) * AUDIO_SECTOR_SIZE;
        while (bytes) {
            if (g_interrupt.IsInterrupted()) { error = "Image preparation cancelled."; return false; }
            const auto count = static_cast<size_t>(std::min<uint64_t>(bytes, buffer.size()));
            if (input) {
                input->read(buffer.data(), count);
                if (static_cast<size_t>(input->gcount()) != count) {
                    error = "Gap or image source ended unexpectedly."; return false;
                }
            } else std::fill_n(buffer.begin(), count, char(0));
            binary.write(buffer.data(), count);
            if (!binary) { error = "Cannot write the prepared image."; return false; }
            bytes -= count;
        }
        written += segment.sectorCount;
    }
    binary.close();
    if (!binary.good() || written != totalSectors || !gapAt.empty()) {
        error = "Prepared image is incomplete."; return false;
    }
    if (!WriteImportedCue(cueOutput.wstring(), binPath.filename().wstring(), sheet, placements)) {
        error = "Could not write the prepared CUE."; return false;
    }
    output.binFile = binPath.wstring();
    output.cueFile = cueOutput.wstring();
    output.subFile.clear(); // SAO continues to generate its own subchannel.
    output.normalized = true;
    return true;
}
