// ============================================================================
// CueSheetImport.h - Parse a CUE sheet that references WAV/FLAC audio files
// and resolve it into an absolute BIN layout.
//
// This is deliberately NOT OpticalDrive::ParseCueSheet. That parser exists to
// describe a flat .bin image that already has the disc's exact sector layout,
// so it rejects multi-FILE sheets and throws the FILE paths away. Here the
// audio files ARE the input, their names matter, and index times are relative
// to whichever file they appear under.
//
// The resolver's one insight is that multi-FILE and single-FILE sheets need no
// separate handling: a single-FILE sheet is just the case where every index
// sits under one file whose base is 0. So EAC's three gap modes (appended,
// prepended, discarded) and the single-image layout all fall out of
//
//     absoluteLBA(index) = fileBase[index.file] + index.frames + silenceBefore
//
// with no mode detection at all. GapMode is reported only so the user can
// confirm the sheet matches the files they picked.
//
// Depends on <windows.h> and the header-only ConsoleColors.h and nothing else,
// so the test project can compile it without dragging in the drive layer.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// "This index does not name a file" - an index that never appeared.
constexpr size_t kCueNoFile = static_cast<size_t>(-1);

// Red Book maximum addressable audio, 79:57:74 inclusive. Past this a burn
// needs an overburn-capable drive and blank, so it is a warning, not a limit.
constexpr uint64_t kMaxAudioSectors = 359850;

// No CD, overburned or not, addresses beyond 100 minutes. A layout past this
// can never be written, so callers should refuse it rather than spend the time
// and disk space materializing an image the drive is guaranteed to reject.
constexpr uint64_t kUnburnableSectors = 450000;

struct CueFileRef {
    std::wstring path;          // absolute, resolved against the CUE's directory
    std::wstring typeKeyword;   // WAVE / BINARY / MP3 / AIFF, as written
};

struct CueImportTrack {
    int  number = 0;
    bool isAudio = true;
    int  dataMode = 0;                  // 0 audio, 1 MODE1/2352, 2 MODE2/2352

    // Each index carries its OWN file. EAC's default "gaps appended to previous
    // track" declares a track twice: its INDEX 00 under the previous track's
    // FILE, its INDEX 01 under its own. One file index per track cannot express
    // that; two can, and then no gap-mode detection is needed anywhere.
    size_t   index00File = kCueNoFile;
    uint32_t index00Frames = 0;
    size_t   index01File = kCueNoFile;
    uint32_t index01Frames = 0;

    uint32_t pregapFrames = 0;          // PREGAP  -> silence we generate
    uint32_t postgapFrames = 0;         // POSTGAP -> silence we generate

    std::string title, performer, isrc; // UTF-8
    bool flagPre = false, flagDcp = false, flag4ch = false, flagScms = false;

    bool hasIndex00() const { return index00File != kCueNoFile; }
};

enum class CueSourceEncoding { Utf8Bom, Utf8, Utf16Le, Utf16Be, AnsiFallback };

// Reported for the summary only; never used to steer the layout.
enum class CueGapMode { None, Appended, Prepended, Mixed };

struct CueImportSheet {
    std::wstring cuePath, baseDir;
    std::vector<CueFileRef>     files;
    std::vector<CueImportTrack> tracks;
    std::string title, performer, catalog;   // UTF-8
    CueSourceEncoding encoding = CueSourceEncoding::Utf8;
    bool singleFileLayout = false;
};

// One contiguous run of the output image.
struct BinSegment {
    enum class Kind { Silence, FileRange };
    Kind     kind = Kind::Silence;
    size_t   fileIndex = 0;     // FileRange only
    uint32_t startSector = 0;   // FileRange only: sector offset within the file
    uint32_t sectorCount = 0;
};

struct TrackPlacement {
    int   trackNumber = 0;      // renumbered 1..N
    bool  hasIndex00 = false;
    uint32_t binIndex00 = 0, binIndex01 = 0;
    std::string title, performer, isrc;
};

const char* CueEncodingName(CueSourceEncoding e);
const char* CueGapModeName(CueGapMode m);

// Parse a CUE sheet. Returns false with a reason in `err`; warnings for
// recoverable oddities go to the console as they are found.
bool ParseCueSheetForImport(const std::wstring& cuePath, CueImportSheet& out,
                            std::string& err);

// Resolve the parsed sheet plus the measured length of each referenced file
// into an absolute BIN layout. `fileSectorCounts` is parallel to sheet.files.
bool BuildBinLayout(const CueImportSheet& sheet,
                    const std::vector<uint32_t>& fileSectorCounts,
                    std::vector<BinSegment>& segments,
                    std::vector<TrackPlacement>& placements,
                    uint64_t& totalSectors,
                    std::string& err);

// Emit the temporary .cue that OpticalDrive::ParseCueSheet reads back before
// the burn. That consumer's constraints drive the format: UTF-8 with no BOM,
// CATALOG and disc-level CD-Text ahead of the first TRACK, a single FILE, and
// strictly increasing disc-absolute INDEX values.
bool WriteImportedCue(const std::wstring& cuePath, const std::wstring& binFileName,
                      const CueImportSheet& sheet,
                      const std::vector<TrackPlacement>& placements);

// Classify the sheet's gap style for the on-screen summary.
CueGapMode ClassifyGapMode(const CueImportSheet& sheet);
