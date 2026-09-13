#define NOMINMAX
#include "../PregapDetection.h"
#include "../QPosition.h"
#include "../CueSheetImport.h"
#include "../ImagePregapPreparation.h"
#include "../OpticalDrive.h"
#include "../TrackReadContext.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

// The unmodified offset-correction implementation is linked below; these
// unused transport entries ensure no physical reads can occur in its tests.
bool ScsiDrive::SeekToLBA(DWORD) { throw std::runtime_error("Unexpected physical seek in offset test"); }
bool ScsiDrive::ReadSectorAudioOnly(DWORD, BYTE*) { throw std::runtime_error("Unexpected physical read in offset test"); }

namespace {
int failures = 0;
void Check(bool ok, const std::string& name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
    if (!ok) ++failures;
}
std::vector<BYTE> Contents(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Text(const std::filesystem::path& file, const std::string& text) {
    std::ofstream output(file, std::ios::binary); output << text;
}
}

int RunPregapTests() {
    {
        TrackInfo first; first.trackNumber=1; first.startLBA=750;
        Pregaps::SetFirstTrackBoundary(first,true,[](DWORD,int&,int&){return false;});
        Check(first.pregapVerified && first.pregapLBA==0 && first.startLBA==750,
            "Trusted first-track TOC preserves leading audio without Q data");
        Pregaps::SetFirstTrackBoundary(first,true,[](DWORD,int& t,int& i){t=1;i=0;return true;});
        Check(!first.pregapVerified && first.pregapLBA==0 && first.startLBA==750,
            "Contradictory first-track Q is unknown without changing the TOC or dropping audio");
        Pregaps::SetFirstTrackBoundary(first,false,[](DWORD,int& t,int& i){t=1;i=1;return true;});
        Check(!first.pregapVerified,"Unrecovered corrupt first-track TOC is not certified");
        const auto forward=Pregaps::FindIndex01(0,1000,1,
            [](DWORD at,int& t,int& i){if(at==300)return false;t=1;i=at<750?0:1;return true;},[] {return false;});
        Check(forward.verified && forward.start==750,
            "TOC-less forward pregap detection crosses interior metadata packets accurately");
        const auto uncertain=Pregaps::FindIndex01(0,1000,1,
            [](DWORD at,int& t,int& i){if(at==749)return false;t=1;i=at<750?0:1;return true;},[] {return false;});
        Check(!uncertain.verified,"TOC-less INDEX01 remains unknown when its transition is unreadable");
    }
    {
        DiscInfo source;
        for (const auto& range : {std::array<DWORD,3>{0,0,9}, {12,10,21}, {22,22,31}, {34,32,43}}) {
            TrackInfo track; track.trackNumber=static_cast<int>(source.tracks.size()+1);
            track.startLBA=range[0];track.pregapLBA=range[1];track.endLBA=range[2];track.pregapVerified=true;
            source.tracks.push_back(track);
        }
        std::vector<BYTE> reference(44ULL*AUDIO_SECTOR_SIZE);
        for (size_t i=0;i<reference.size();++i) reference[i]=static_cast<BYTE>((i*131+i/AUDIO_SECTOR_SIZE*17)%251);
        for (const auto& selection : {std::pair<size_t,size_t>{0,1},{1,2},{2,2},{0,0},{3,3}}) {
            for (int offset : {-667,-6,0,6,667}) {
                DiscInfo read;
                read.tracks.assign(source.tracks.begin()+selection.first,source.tracks.begin()+selection.second+1);
                read.pregapMode=PregapMode::Skip;read.driveOffset=offset;
                const auto bodies=read.tracks;
                AddTrackOffsetContext(read,source,selection.first,selection.second);
                for (const auto& track:read.tracks) {
                    const auto start=read.pregapMode==PregapMode::Skip?track.startLBA:track.pregapLBA;
                    for (DWORD lba=start;lba<=track.endLBA;++lba) {
                        const auto begin=reference.begin()+static_cast<size_t>(lba)*AUDIO_SECTOR_SIZE;
                        read.rawSectors.emplace_back(begin,begin+AUDIO_SECTOR_SIZE);
                    }
                }
                OpticalDrive drive;
                if(offset)drive.ApplyOffsetCorrection(read); // unchanged production arithmetic
                std::vector<TrackOutputSlice> slices;
                bool matches=BuildTrackOutputSlices(read,bodies,slices);
                if(matches)for(size_t t=0;t<slices.size() && matches;++t) {
                    for(size_t byte=0;byte<slices[t].count*AUDIO_SECTOR_SIZE;++byte) {
                        const int64_t original=static_cast<int64_t>(bodies[t].startLBA)*AUDIO_SECTOR_SIZE+
                            static_cast<int64_t>(byte)+static_cast<int64_t>(offset)*4;
                        const BYTE expected=original>=0 && static_cast<uint64_t>(original)<reference.size()
                            ?reference[static_cast<size_t>(original)]:0;
                        if(read.rawSectors[slices[t].start+byte/AUDIO_SECTOR_SIZE][byte%AUDIO_SECTOR_SIZE]!=expected) {
                            matches=false;break;
                        }
                    }
                }
                Check(matches,"Track read context preserves exact boundary bytes for offset "+std::to_string(offset)+
                    " and selection "+std::to_string(selection.first)+"-"+std::to_string(selection.second));
                Check(RemoveTrackOffsetContext(read,bodies) && read.tracks.front().pregapLBA==bodies.front().pregapLBA &&
                    read.tracks.back().endLBA==bodies.back().endLBA,
                    "Temporary offset context does not change the real track or pregap metadata");
            }
        }
    }
    for (DWORD gap : {0UL, 1UL, 2UL, 3UL, 150UL, 151UL, 449UL, 450UL, 451UL, 465UL, 600UL, 1500UL}) {
        const auto boundary = Pregaps::FindBoundary(3000, 0, 2, 1,
            [gap](DWORD at, int& track, int& index) {
                track = at < 3000-gap ? 1 : 2;
                index = at < 3000 && at >= 3000-gap ? 0 : 1;
                return true;
            }, [] { return false; });
        Check(boundary.verified && boundary.start == 3000-gap,
            "Pregap boundary is exact for " + std::to_string(gap) + " frames");
    }
    auto missing = Pregaps::FindBoundary(3000, 0, 2, 1,
        [](DWORD, int&, int&) { return false; }, [] { return false; });
    Check(!missing.verified, "Unreadable Q is unknown, not a confirmed zero gap");
    auto interrupted = Pregaps::FindBoundary(3000, 0, 2, 1,
        [](DWORD, int&, int&) { return true; }, [] { return true; });
    Check(!interrupted.verified, "Timed-out or cancelled pregap probing never certifies a boundary");
    auto hole = Pregaps::FindBoundary(3000, 0, 2, 1,
        [](DWORD at, int& track, int& index) {
            if (at == 2850) return false;
            track = at < 2850 ? 1 : 2; index = at >= 2850 && at < 3000 ? 0 : 1; return true;
        }, [] { return false; });
    Check(!hole.verified, "A missing position at the gap boundary remains unknown");
    auto metadata = Pregaps::FindBoundary(3000, 0, 2, 1,
        [](DWORD at, int& track, int& index) {
            if (at == 2900) return false; // MCN/ISRC packet has no position
            track = at < 2850 ? 1 : 2; index = at >= 2850 && at < 3000 ? 0 : 1; return true;
        }, [] { return false; });
    Check(metadata.verified && metadata.start == 2850,
        "An interior metadata packet does not hide an otherwise fully bounded pregap");
    auto anchor = Pregaps::FindBoundary(3000, 0, 2, 1,
        [](DWORD at, int& track, int& index) {
            if (at == 3000) return false;
            track = 1; index = 1; return true;
        }, [] { return false; });
    Check(anchor.verified && anchor.start == 3000,
        "Trusted TOC and adjacent position prove zero gap when INDEX01 carries metadata");
    auto unrelated = Pregaps::FindBoundary(3000, 0, 2, 1,
        [](DWORD at, int& track, int& index) {
            track = at == 3000 ? 2 : (at == 2550 ? 2 : 1);
            index = at == 2550 ? 0 : 1; return true;
        }, [] { return false; });
    Check(unrelated.verified && unrelated.start == 3000,
        "A false INDEX00 far from the actual transition cannot create a gap");

    BYTE q[12]{};
    BuildPositionQ(q, 0, 2, 0, 0, 0, 1, 0, 15, 25); // LBA 1000
    int track = 0, index = -1;
    Check(DecodePositionQ(q, 1000, track, index) && track == 2 && index == 0,
        "Q position accepts an address- and CRC-valid frame");
    Check(!DecodePositionQ(q, 1005, track, index), "Stale Q absolute address is rejected");
    q[10] ^= 1;
    Check(!DecodePositionQ(q, 1000, track, index), "Bad raw-Q CRC is rejected");
    BYTE formatted[16]{};
    BuildPositionQ(formatted, 0, 2, 0, 0, 0, 1, 0, 15, 25);
    formatted[10] = formatted[11] = 0;
    Check(DecodeFormattedPositionQ(formatted, 16, 1000, track, index),
        "MMC formatted Q permits omitted CRC while still validating position");
    Check(!DecodeFormattedPositionQ(formatted, 16, 1005, track, index),
        "Formatted Q without CRC still rejects stale addresses");
    int reads = 0;
    Check(!ReadPositionMajority(1000, track, index, [&](DWORD, int& t, int& i) {
        t = ++reads; i = 0; return true;
    }), "Conflicting Q observations cannot produce a majority");
    reads = 0;
    Check(!ReadPositionMajority(1000, track, index, [&](DWORD, int& t, int& i) {
        t = 2; i = 0; return ++reads == 1;
    }), "A single valid Q observation cannot certify a position");
    reads = 0;
    Check(ReadPositionMajority(1000, track, index, [&](DWORD, int& t, int& i) {
        t = ++reads == 2 ? 1 : 2; i = 0; return true;
    }) && track == 2, "Two matching Q observations establish a majority");

    const auto directory = std::filesystem::temp_directory_path() /
        (L"OptiScanPregapTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory);
    {
        CueImportSheet sheet;
        sheet.files.push_back({L"album.wav",L"WAVE"});
        CueImportTrack first; first.number=1; first.index00File=0; first.index01File=0; first.index01Frames=750;
        CueImportTrack second; second.number=2; second.index01File=0; second.index01Frames=2000;
        sheet.tracks={first,second};
        std::vector<BinSegment> segments; std::vector<TrackPlacement> placements;
        uint64_t length=0; std::string error;
        Check(BuildBinLayout(sheet,{5000},segments,placements,length,error) && length==5000 &&
            placements[0].binIndex01==750 && placements[0].hasIndex00 && placements[0].binIndex00==0,
            "WAV/FLAC CUE import retains all 750 leading HTOA sectors");
        sheet.tracks[0].index00File=kCueNoFile; sheet.tracks[0].index01Frames=0; sheet.tracks[0].pregapFrames=600;
        Check(BuildBinLayout(sheet,{5000},segments,placements,length,error) && length==5450 && placements[0].binIndex01==450,
            "Extended first PREGAP adds only the portion beyond the writer's mandatory 150 frames");
        sheet.tracks[0].pregapFrames=75;
        Check(!BuildBinLayout(sheet,{5000},segments,placements,length,error),
            "An impossible first pause is rejected rather than silently lengthened");
    }
    {
        OpticalDrive drive;
        for (auto mode : {PregapMode::Include, PregapMode::Separate, PregapMode::Skip}) {
            DiscInfo disc; disc.includeSubchannel = false; disc.pregapMode = mode;
            TrackInfo a; a.trackNumber = 1; a.startLBA = 75; a.pregapLBA = 0; a.endLBA = 849; a.pregapVerified = true;
            TrackInfo b; b.trackNumber = 2; b.startLBA = 1000; b.pregapLBA = 850; b.endLBA = 1999; b.pregapVerified = true;
            disc.tracks = {a,b}; disc.leadOutLBA = 2000;
            disc.cdText.albumTitle = "Pregap album";
            disc.cdText.albumArtist = "Test artist";
            disc.cdText.trackTitles = {"Hidden lead", "Second track"};
            disc.mcn = "1234567890123";
            std::vector<BYTE> expected;
            for (const auto& t : disc.tracks) {
                const DWORD begin = mode == PregapMode::Skip ? t.startLBA : t.pregapLBA;
                for (DWORD sector = begin; sector <= t.endLBA; ++sector) {
                    disc.rawSectors.emplace_back(AUDIO_SECTOR_SIZE, static_cast<BYTE>(sector % 251));
                    expected.insert(expected.end(), disc.rawSectors.back().begin(), disc.rawSectors.back().end());
                }
            }
            const auto base = directory / (mode == PregapMode::Include ? L"include" : mode == PregapMode::Separate ? L"separate" : L"skip");
            Check(drive.SaveToFile(disc, base.wstring()), "Pregap fixture saves its requested mode");
            PreparedImageSource prepared; std::string error;
            const bool ready = PrepareImagePregaps(base.wstring()+L".cue", prepared, error);
            if (mode == PregapMode::Skip) {
                Check(!ready && error.find("discarded") != std::string::npos,
                    "Discarded pregap audio blocks an exact-copy write rather than fabricating silence");
                continue;
            }
            Check(ready, "Saved pregap image is prepared for the unchanged SAO writer: " + error);
            if (!ready) continue;
            Check(Contents(prepared.binFile) == expected,
                "Program-area HTOA and internal pregap audio survive byte-exact image preparation");
            std::vector<OpticalDrive::TrackWriteInfo> tracks;
            const bool parsed = drive.ParseCueSheet(prepared.cueFile, tracks);
            Check(parsed && tracks.size() == 2 && tracks[0].startLBA == 75 &&
                tracks[1].startLBA == 1000 && tracks[1].pregapLBA == 850,
                "Prepared CUE preserves both Track1 INDEX01 and the internal INDEX00/01");
            std::string title, artist, catalog;
            Check(drive.ParseCueSheet(prepared.cueFile, tracks, title, artist, catalog) &&
                title == disc.cdText.albumTitle && artist == disc.cdText.albumArtist &&
                catalog == disc.mcn && tracks[0].title == "Hidden lead" && tracks[1].title == "Second track",
                "Pregap image preparation preserves CD-Text and catalog metadata");
        }
        DiscInfo unknown; TrackInfo trackInfo; trackInfo.trackNumber=1;
        unknown.tracks={trackInfo};unknown.includeSubchannel=false;unknown.rawSectors.emplace_back(AUDIO_SECTOR_SIZE,static_cast<BYTE>(0));
        unknown.leadOutLBA=1;
        const auto base=directory/L"unknown";
        Check(drive.SaveToFile(unknown,base.wstring()), "Unknown-pregap audio may still be archived in Include mode");
        PreparedImageSource prepared;std::string error;
        Check(!PrepareImagePregaps(base.wstring()+L".cue",prepared,error),
            "An unknown pregap marker prevents a false exact-layout write");
        unknown.pregapMode=PregapMode::Separate;
        Check(!drive.SaveToFile(unknown,(directory/L"unknown-split").wstring()),
            "Unverified pregaps cannot be silently separated or discarded");
        Text(directory/L"broken.cue", "FILE \"include.bin\" BINARY\nREM OPTISCAN_PREGAP_FILE 2 \"absent.bin\"\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nPREGAP 00:02:00\nINDEX 01 00:11:25\n");
        Check(!PrepareImagePregaps((directory/L"broken.cue").wstring(),prepared,error),
            "Missing separate pregap audio fails before disc preparation");
        for (const std::string& invalid : {
            "INDEX 00 00/02/00\nINDEX 01 00:03:00\n",
            "INDEX 00 00:04:00\nINDEX 01 00:03:00\n",
            "INDEX 00 00:00:00\n",
            "INDEX 01 00:00:00\nINDEX 01 00:01:00\n"}) {
            Text(directory/L"invalid.cue", "FILE \"include.bin\" BINARY\nTRACK 01 AUDIO\n" + invalid);
            std::vector<OpticalDrive::TrackWriteInfo> tracks;
            Check(!drive.ParseCueSheet((directory/L"invalid.cue").wstring(), tracks),
                "Malformed, inverted, absent or duplicate INDEX data cannot silently change a pregap");
        }
        Text(directory/L"generated.cue", "FILE \"include.bin\" BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nPREGAP 00:02:00\nINDEX 01 00:11:25\n");
        Check(PrepareImagePregaps((directory/L"generated.cue").wstring(),prepared,error),
            "Explicit generated-silence PREGAP is materialized for generic image CUEs");
        if (!prepared.binFile.empty() && prepared.normalized) {
            const auto audio=Contents(prepared.binFile);
            Check(audio.size()==2150ULL*AUDIO_SECTOR_SIZE &&
                std::all_of(audio.begin()+850ULL*AUDIO_SECTOR_SIZE,audio.begin()+1000ULL*AUDIO_SECTOR_SIZE,[](BYTE b){return b==0;}),
                "Generated PREGAP inserts precisely 150 sectors of silence at the requested boundary");
        }
        Text(directory/L"absolute.cue", "FILE \"" + (directory/L"include.bin").string() +
            "\" BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nPREGAP 00:02:00\nINDEX 01 00:11:25\n");
        Check(PrepareImagePregaps((directory/L"absolute.cue").wstring(),prepared,error),
            "Pregap image preparation preserves Windows backslashes in quoted FILE paths");
    }
    std::filesystem::remove_all(directory);
    return failures;
}
