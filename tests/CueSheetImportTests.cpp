// ============================================================================
// CueSheetImportTests.cpp - coverage for the CUE-sheet import parser and its
// absolute-layout resolver.
//
// The resolver is the part worth testing hard: it claims one formula covers
// single-FILE sheets and all three of EAC's gap modes. Cases 2-4 below burn
// that claim in by asserting that the appended and prepended forms of the same
// album resolve to the same LBAs.
//
// Layouts are checked against synthetic sector counts rather than real audio,
// so BuildBinLayout never needs a WAV on disk; only the parser cases write
// fixture files, because the parser stats every FILE it reads.
// ============================================================================
#define NOMINMAX
#include "../CueSheetImport.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int cueFailures = 0;

void Check(bool condition, const char* message) {
	if (condition) {
		std::cout << "[PASS] " << message << "\n";
	}
	else {
		std::cerr << "[FAIL] " << message << "\n";
		cueFailures++;
	}
}

std::filesystem::path FixtureDir() {
	static std::filesystem::path dir = [] {
		std::filesystem::path d =
			std::filesystem::temp_directory_path() / L"OptiScanCueTests";
		std::error_code ec;
		std::filesystem::create_directories(d, ec);
		return d;
	}();
	return dir;
}

// Touch a placeholder for every FILE a fixture names. The parser only stats
// them; sector counts are supplied to BuildBinLayout directly.
void TouchFile(const std::wstring& name) {
	std::ofstream f(FixtureDir() / name, std::ios::binary | std::ios::trunc);
	f.put('\0');
}

// Write `text` verbatim (already the bytes we want on disk, encoding included).
std::wstring WriteCue(const std::wstring& name, const std::string& bytes) {
	const std::filesystem::path p = FixtureDir() / name;
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	f.close();
	return p.wstring();
}

bool Parse(const std::wstring& name, const std::string& bytes,
	CueImportSheet& sheet, std::string& err) {
	return ParseCueSheetForImport(WriteCue(name, bytes), sheet, err);
}

// ── Fixtures ────────────────────────────────────────────────────────────
// One album, three tracks, expressed four ways. Track lengths in sectors:
// 9000 (2:00), 13500 (3:00), 7500 (1:40). Gaps: 150 frames before tracks 2
// and 3.

const char* kSingleFileCue =
	"PERFORMER \"Test Artist\"\n"
	"TITLE \"Test Album\"\n"
	"FILE \"album.wav\" WAVE\n"
	"  TRACK 01 AUDIO\n"
	"    TITLE \"One\"\n"
	"    INDEX 01 00:00:00\n"
	"  TRACK 02 AUDIO\n"
	"    TITLE \"Two\"\n"
	"    INDEX 00 02:00:00\n"
	"    INDEX 01 02:02:00\n"
	"  TRACK 03 AUDIO\n"
	"    TITLE \"Three\"\n"
	"    INDEX 00 05:02:00\n"
	"    INDEX 01 05:04:00\n";

// EAC default: each track's gap sits at the TAIL of the previous track's file,
// so TRACK 02 is declared twice - INDEX 00 under file 1, INDEX 01 under file 2.
const char* kGapsAppendedCue =
	"FILE \"t1.wav\" WAVE\n"
	"  TRACK 01 AUDIO\n"
	"    INDEX 01 00:00:00\n"
	"  TRACK 02 AUDIO\n"
	"    INDEX 00 02:00:00\n"
	"FILE \"t2.wav\" WAVE\n"
	"  TRACK 02 AUDIO\n"
	"    INDEX 01 00:00:00\n"
	"  TRACK 03 AUDIO\n"
	"    INDEX 00 03:00:00\n"
	"FILE \"t3.wav\" WAVE\n"
	"  TRACK 03 AUDIO\n"
	"    INDEX 01 00:00:00\n";

// Same album, gaps at the HEAD of each track's own file.
const char* kGapsPrependedCue =
	"FILE \"p1.wav\" WAVE\n"
	"  TRACK 01 AUDIO\n"
	"    INDEX 01 00:00:00\n"
	"FILE \"p2.wav\" WAVE\n"
	"  TRACK 02 AUDIO\n"
	"    INDEX 00 00:00:00\n"
	"    INDEX 01 00:02:00\n"
	"FILE \"p3.wav\" WAVE\n"
	"  TRACK 03 AUDIO\n"
	"    INDEX 00 00:00:00\n"
	"    INDEX 01 00:02:00\n";

const char* kGapsDiscardedCue =
	"FILE \"d1.wav\" WAVE\n"
	"  TRACK 01 AUDIO\n"
	"    INDEX 01 00:00:00\n"
	"FILE \"d2.wav\" WAVE\n"
	"  TRACK 02 AUDIO\n"
	"    INDEX 01 00:00:00\n"
	"FILE \"d3.wav\" WAVE\n"
	"  TRACK 03 AUDIO\n"
	"    INDEX 01 00:00:00\n";

uint64_t SilenceSectors(const std::vector<BinSegment>& segments) {
	uint64_t total = 0;
	for (const auto& s : segments)
		if (s.kind == BinSegment::Kind::Silence) total += s.sectorCount;
	return total;
}

size_t CountKind(const std::vector<BinSegment>& segments, BinSegment::Kind kind) {
	size_t n = 0;
	for (const auto& s : segments) if (s.kind == kind) n++;
	return n;
}

}  // namespace

int RunCueSheetImportTests() {
	cueFailures = 0;
	std::cout << "\n--- CUE sheet import ---\n";

	for (const wchar_t* name : { L"album.wav", L"t1.wav", L"t2.wav", L"t3.wav",
		L"p1.wav", L"p2.wav", L"p3.wav", L"d1.wav", L"d2.wav", L"d3.wav",
		L"data.bin" })
		TouchFile(name);

	CueImportSheet sheet;
	std::string err;
	std::vector<BinSegment> segments;
	std::vector<TrackPlacement> placements;
	uint64_t total = 0;

	// ── 1. Single-FILE image ────────────────────────────────────────────
	Check(Parse(L"single.cue", kSingleFileCue, sheet, err),
		"single-FILE CUE parses");
	Check(sheet.singleFileLayout, "single-FILE CUE is flagged as a single-file layout");
	Check(sheet.files.size() == 1 && sheet.tracks.size() == 3,
		"single-FILE CUE yields one file and three tracks");
	Check(sheet.title == "Test Album" && sheet.performer == "Test Artist",
		"disc-level CD-Text is captured");
	Check(sheet.tracks[1].title == "Two", "per-track CD-Text is captured");

	Check(BuildBinLayout(sheet, { 30150 }, segments, placements, total, err),
		"single-FILE layout resolves");
	Check(placements.size() == 3, "single-FILE layout places three tracks");
	Check(placements[0].binIndex01 == 0 && !placements[0].hasIndex00,
		"track 1 starts at LBA 0 with no pregap");
	Check(placements[1].binIndex00 == 9000 && placements[1].binIndex01 == 9150,
		"single-FILE track 2 keeps its INDEX 00/01 verbatim");
	Check(placements[2].binIndex00 == 22650 && placements[2].binIndex01 == 22800,
		"single-FILE track 3 keeps its INDEX 00/01 verbatim");
	Check(total == 30150, "single-FILE image is the file's own length");
	Check(segments.size() == 1 &&
		segments[0].kind == BinSegment::Kind::FileRange &&
		segments[0].startSector == 0 && segments[0].sectorCount == 30150,
		"single-FILE image streams as one unsplit range");

	// ── 2. Multi-FILE, gaps appended ────────────────────────────────────
	CueImportSheet appended;
	Check(Parse(L"appended.cue", kGapsAppendedCue, appended, err),
		"gaps-appended CUE parses (track declared under two FILEs)");
	Check(appended.files.size() == 3 && appended.tracks.size() == 3,
		"gaps-appended CUE collapses the repeated TRACK lines");
	Check(ClassifyGapMode(appended) == CueGapMode::Appended,
		"gaps-appended CUE is classified as appended");
	Check(appended.tracks[1].index00File == 0 && appended.tracks[1].index01File == 1,
		"an appended gap keeps INDEX 00 in the previous track's file");

	std::vector<BinSegment> appendedSegments;
	std::vector<TrackPlacement> appendedPlacements;
	uint64_t appendedTotal = 0;
	Check(BuildBinLayout(appended, { 9150, 13650, 7500 }, appendedSegments,
		appendedPlacements, appendedTotal, err),
		"gaps-appended layout resolves");
	Check(appendedPlacements[1].hasIndex00 &&
		appendedPlacements[1].binIndex00 == 9000 &&
		appendedPlacements[1].binIndex01 == 9150,
		"an appended gap straddles the file boundary at the right LBAs");
	Check(appendedPlacements[2].binIndex00 == 22650 &&
		appendedPlacements[2].binIndex01 == 22800,
		"the second appended gap lands at the right LBAs");
	Check(appendedTotal == 30300, "gaps-appended image is the sum of its files");
	Check(SilenceSectors(appendedSegments) == 0,
		"an appended gap generates no silence - the audio is in the files");

	// ── 3. Multi-FILE, gaps prepended - must match case 2 ───────────────
	CueImportSheet prepended;
	Check(Parse(L"prepended.cue", kGapsPrependedCue, prepended, err),
		"gaps-prepended CUE parses");
	Check(ClassifyGapMode(prepended) == CueGapMode::Prepended,
		"gaps-prepended CUE is classified as prepended");

	std::vector<BinSegment> prependedSegments;
	std::vector<TrackPlacement> prependedPlacements;
	uint64_t prependedTotal = 0;
	Check(BuildBinLayout(prepended, { 9000, 13800, 7500 }, prependedSegments,
		prependedPlacements, prependedTotal, err),
		"gaps-prepended layout resolves");
	Check(prependedPlacements[1].binIndex00 == 9000 &&
		prependedPlacements[1].binIndex01 == 9150,
		"a prepended gap resolves to the same LBAs as the appended form");
	Check(prependedPlacements[2].binIndex00 == 22800 &&
		prependedPlacements[2].binIndex01 == 22950,
		"the second prepended gap resolves against its own file base");
	Check(prependedTotal == 30300,
		"gaps-prepended image is the same length as the appended form");

	// ── 4. Multi-FILE, gaps discarded ───────────────────────────────────
	CueImportSheet discarded;
	Check(Parse(L"discarded.cue", kGapsDiscardedCue, discarded, err),
		"gaps-discarded CUE parses");
	Check(ClassifyGapMode(discarded) == CueGapMode::None,
		"gaps-discarded CUE reports no gaps");
	std::vector<BinSegment> discardedSegments;
	std::vector<TrackPlacement> discardedPlacements;
	uint64_t discardedTotal = 0;
	Check(BuildBinLayout(discarded, { 9000, 13500, 7500 }, discardedSegments,
		discardedPlacements, discardedTotal, err),
		"gaps-discarded layout resolves");
	Check(!discardedPlacements[1].hasIndex00 && !discardedPlacements[2].hasIndex00,
		"gaps-discarded tracks carry no INDEX 00");
	Check(discardedPlacements[1].binIndex01 == 9000 &&
		discardedPlacements[2].binIndex01 == 22500,
		"gaps-discarded tracks abut at the cumulative file bases");
	Check(discardedTotal == 30000,
		"gaps-discarded image is shorter by the dropped gaps");
	Check(CountKind(discardedSegments, BinSegment::Kind::FileRange) == 3,
		"gaps-discarded image is three unsplit file ranges");

	// ── 5. PREGAP between files generates silence ───────────────────────
	{
		const char* cue =
			"FILE \"d1.wav\" WAVE\n"
			"  TRACK 01 AUDIO\n"
			"    INDEX 01 00:00:00\n"
			"FILE \"d2.wav\" WAVE\n"
			"  TRACK 02 AUDIO\n"
			"    INDEX 01 00:00:00\n"
			"FILE \"d3.wav\" WAVE\n"
			"  TRACK 03 AUDIO\n"
			"    PREGAP 00:02:00\n"
			"    INDEX 01 00:00:00\n";
		CueImportSheet s;
		Check(Parse(L"pregap_multi.cue", cue, s, err), "PREGAP CUE parses");
		Check(s.tracks[2].pregapFrames == 150, "PREGAP is honoured, not ignored");
		std::vector<BinSegment> seg;
		std::vector<TrackPlacement> pl;
		uint64_t t = 0;
		Check(BuildBinLayout(s, { 9000, 13500, 7500 }, seg, pl, t, err),
			"PREGAP layout resolves");
		Check(SilenceSectors(seg) == 150, "PREGAP emits exactly its silence");
		Check(pl[1].binIndex01 == 9000, "tracks before a PREGAP are unmoved");
		Check(pl[2].hasIndex00 && pl[2].binIndex00 == 22500 &&
			pl[2].binIndex01 == 22650,
			"a PREGAP track gains an INDEX 00 marking the generated silence");
		Check(t == 30150, "a PREGAP lengthens the image by its own size");
	}

	// ── 6. PREGAP mid-file splits the enclosing range ───────────────────
	{
		const char* cue =
			"FILE \"album.wav\" WAVE\n"
			"  TRACK 01 AUDIO\n"
			"    INDEX 01 00:00:00\n"
			"  TRACK 02 AUDIO\n"
			"    PREGAP 00:02:00\n"
			"    INDEX 01 02:00:00\n";
		CueImportSheet s;
		Check(Parse(L"pregap_single.cue", cue, s, err),
			"mid-file PREGAP CUE parses");
		std::vector<BinSegment> seg;
		std::vector<TrackPlacement> pl;
		uint64_t t = 0;
		Check(BuildBinLayout(s, { 22500 }, seg, pl, t, err),
			"mid-file PREGAP layout resolves");
		Check(seg.size() == 3 &&
			seg[0].kind == BinSegment::Kind::FileRange && seg[0].sectorCount == 9000 &&
			seg[1].kind == BinSegment::Kind::Silence && seg[1].sectorCount == 150 &&
			seg[2].kind == BinSegment::Kind::FileRange &&
			seg[2].startSector == 9000 && seg[2].sectorCount == 13500,
			"a mid-file PREGAP splits the file range around the silence");
		Check(pl[1].binIndex00 == 9000 && pl[1].binIndex01 == 9150,
			"a mid-file PREGAP shifts the following track by its length");
		Check(t == 22650, "a mid-file PREGAP lengthens the image");
	}

	// ── 7. Track 1 PREGAP is dropped, not materialized ──────────────────
	{
		const char* cue =
			"FILE \"album.wav\" WAVE\n"
			"  TRACK 01 AUDIO\n"
			"    PREGAP 00:02:00\n"
			"    INDEX 01 00:00:00\n"
			"  TRACK 02 AUDIO\n"
			"    INDEX 01 02:00:00\n";
		CueImportSheet s;
		Check(Parse(L"leadin.cue", cue, s, err), "track 1 PREGAP CUE parses");
		std::vector<BinSegment> seg;
		std::vector<TrackPlacement> pl;
		uint64_t t = 0;
		Check(BuildBinLayout(s, { 22500 }, seg, pl, t, err),
			"track 1 PREGAP layout resolves");
		Check(SilenceSectors(seg) == 0,
			"track 1 PREGAP emits no silence - the writer generates the lead-in");
		Check(pl[0].binIndex01 == 0 && !pl[0].hasIndex00,
			"track 1 still starts at LBA 0 with no INDEX 00");
		Check(pl[1].binIndex01 == 9000, "track 2 is unshifted by a track 1 PREGAP");
		Check(t == 22500, "a track 1 PREGAP does not lengthen the image");
	}

	// ── 8. ANSI-encoded sheet ───────────────────────────────────────────
	{
		// "Sigur Ros" with an o-acute in Windows-1252 (0xF3) - not valid UTF-8,
		// so this only parses if the ANSI fallback is working.
		std::string cue =
			"PERFORMER \"Sigur R\xF3s\"\n"
			"FILE \"album.wav\" WAVE\n"
			"  TRACK 01 AUDIO\n"
			"    INDEX 01 00:00:00\n";
		CueImportSheet s;
		Check(Parse(L"ansi.cue", cue, s, err), "an ANSI CUE parses via the fallback");
		Check(s.encoding == CueSourceEncoding::AnsiFallback,
			"the ANSI fallback is reported as such");
		Check(!s.performer.empty() && s.performer != "Sigur R",
			"the ANSI performer survives as UTF-8");
	}

	// ── 9. UTF-16 LE sheet ──────────────────────────────────────────────
	{
		const std::wstring wide =
			L"FILE \"album.wav\" WAVE\r\n"
			L"  TRACK 01 AUDIO\r\n"
			L"    INDEX 01 00:00:00\r\n";
		std::string bytes("\xFF\xFE", 2);
		bytes.append(reinterpret_cast<const char*>(wide.data()), wide.size() * 2);
		CueImportSheet s;
		Check(Parse(L"utf16.cue", bytes, s, err), "a UTF-16 LE CUE parses");
		Check(s.encoding == CueSourceEncoding::Utf16Le,
			"UTF-16 LE is detected from the BOM");
		Check(s.tracks.size() == 1, "the UTF-16 CUE's track is found");
	}

	// ── 10. Missing FILE is caught before anything else happens ─────────
	{
		const char* cue =
			"FILE \"not_here_at_all.wav\" WAVE\n"
			"  TRACK 01 AUDIO\n"
			"    INDEX 01 00:00:00\n";
		CueImportSheet s;
		std::string missingErr;
		Check(!Parse(L"missing.cue", cue, s, missingErr),
			"a CUE naming a missing file is rejected");
		Check(missingErr.find("does not exist") != std::string::npos,
			"the missing-file error says what is missing");
	}

	// ── 11. INDEX past the end of its file ──────────────────────────────
	{
		CueImportSheet s;
		Check(Parse(L"short.cue", kSingleFileCue, s, err), "short-file CUE parses");
		std::vector<BinSegment> seg;
		std::vector<TrackPlacement> pl;
		uint64_t t = 0;
		std::string shortErr;
		Check(!BuildBinLayout(s, { 9000 }, seg, pl, t, shortErr),
			"an INDEX past the end of its file is rejected");
		Check(shortErr.find("shorter than the CUE says") != std::string::npos,
			"the short-file error explains the mismatch");
	}

	// ── 12. Data tracks ─────────────────────────────────────────────────
	{
		const char* leadingData =
			"FILE \"data.bin\" BINARY\n"
			"  TRACK 01 MODE1/2352\n"
			"    INDEX 01 00:00:00\n"
			"FILE \"d2.wav\" WAVE\n"
			"  TRACK 02 AUDIO\n"
			"    INDEX 01 00:00:00\n";
		CueImportSheet s;
		Check(Parse(L"leading_data.cue", leadingData, s, err),
			"a leading-data CUE parses");
		std::vector<BinSegment> seg;
		std::vector<TrackPlacement> pl;
		uint64_t t = 0;
		std::string dataErr;
		Check(!BuildBinLayout(s, { 1000, 9000 }, seg, pl, t, dataErr),
			"a CUE beginning with a data track is rejected");

		const char* trailingData =
			"FILE \"d1.wav\" WAVE\n"
			"  TRACK 01 AUDIO\n"
			"    INDEX 01 00:00:00\n"
			"FILE \"d2.wav\" WAVE\n"
			"  TRACK 02 AUDIO\n"
			"    INDEX 01 00:00:00\n"
			"FILE \"data.bin\" BINARY\n"
			"  TRACK 03 MODE1/2352\n"
			"    INDEX 01 00:00:00\n";
		CueImportSheet s2;
		Check(Parse(L"trailing_data.cue", trailingData, s2, err),
			"a trailing-data CUE parses");
		std::vector<BinSegment> seg2;
		std::vector<TrackPlacement> pl2;
		uint64_t t2 = 0;
		Check(BuildBinLayout(s2, { 9000, 13500, 5000 }, seg2, pl2, t2, err),
			"a trailing data track is dropped rather than rejected");
		Check(pl2.size() == 2, "only the audio tracks survive");
		Check(t2 == 22500, "the dropped data track's sectors leave the image");

		const char* sandwiched =
			"FILE \"d1.wav\" WAVE\n"
			"  TRACK 01 AUDIO\n"
			"    INDEX 01 00:00:00\n"
			"FILE \"data.bin\" BINARY\n"
			"  TRACK 02 MODE1/2352\n"
			"    INDEX 01 00:00:00\n"
			"FILE \"d3.wav\" WAVE\n"
			"  TRACK 03 AUDIO\n"
			"    INDEX 01 00:00:00\n";
		CueImportSheet s3;
		Check(Parse(L"sandwiched.cue", sandwiched, s3, err),
			"a sandwiched-data CUE parses");
		std::vector<BinSegment> seg3;
		std::vector<TrackPlacement> pl3;
		uint64_t t3 = 0;
		std::string sandwichErr;
		Check(!BuildBinLayout(s3, { 9000, 5000, 7500 }, seg3, pl3, t3, sandwichErr),
			"a data track between audio tracks is rejected");
	}

	// ── 13. Malformed input ─────────────────────────────────────────────
	{
		CueImportSheet s;
		std::string e;
		Check(!Parse(L"nofile.cue", "TRACK 01 AUDIO\n  INDEX 01 00:00:00\n", s, e),
			"a CUE with no FILE is rejected");
		Check(!Parse(L"notrack.cue", "FILE \"album.wav\" WAVE\n", s, e),
			"a CUE with no TRACK is rejected");
		Check(!Parse(L"noindex.cue",
			"FILE \"album.wav\" WAVE\n  TRACK 01 AUDIO\n", s, e),
			"a track with no INDEX 01 is rejected");
		Check(!Parse(L"badmode.cue",
			"FILE \"album.wav\" WAVE\n  TRACK 01 MODE2/2336\n"
			"    INDEX 01 00:00:00\n", s, e),
			"an unwritable track mode is rejected");
		Check(!Parse(L"badmsf.cue",
			"FILE \"album.wav\" WAVE\n  TRACK 01 AUDIO\n"
			"    INDEX 01 00:99:00\n", s, e),
			"an out-of-range MM:SS:FF time is rejected");
	}

	// ── 14. Quotes in CD-Text are stripped for the temp sheet ───────────
	{
		// The consumer's extractQuoted stops at the first closing quote, so a
		// title carrying one would truncate the sheet we hand it.
		std::string cue =
			"FILE \"album.wav\" WAVE\n"
			"  TRACK 01 AUDIO\n"
			"    TITLE \"He said \"\"hi\"\" loudly\"\n"
			"    INDEX 01 00:00:00\n";
		CueImportSheet s;
		Check(Parse(L"quotes.cue", cue, s, err), "a quoted-title CUE parses");
		Check(s.tracks[0].title.find('"') == std::string::npos,
			"embedded quotes are stripped from CD-Text");
	}

	// ── 15. Unquoted FILE paths with spaces ─────────────────────────────
	{
		std::string cue =
			"FILE album.wav WAVE\n"
			"  TRACK 01 AUDIO\n"
			"    INDEX 01 00:00:00\n";
		CueImportSheet s;
		Check(Parse(L"unquoted.cue", cue, s, err), "an unquoted FILE path parses");
		Check(s.files.size() == 1 && s.files[0].typeKeyword == L"WAVE",
			"the type keyword is split off an unquoted path");
	}


	// ── 16. The emitted temp CUE round-trips ────────────────────────────
	// WriteImportedCue's output is consumed by OpticalDrive::ParseCueSheet just
	// before the burn. That parser is stricter than this one - one FILE only,
	// CATALOG ahead of the first TRACK, disc-absolute increasing INDEX values -
	// so an emitter regression would only surface at burn time. Re-reading our
	// own output catches the structural half of that here.
	{
		CueImportSheet source;
		Check(Parse(L"roundtrip_src.cue", kSingleFileCue, source, err),
			"round-trip source CUE parses");
		source.catalog = "0123456789012";
		std::vector<BinSegment> seg;
		std::vector<TrackPlacement> pl;
		uint64_t t = 0;
		Check(BuildBinLayout(source, { 30150 }, seg, pl, t, err),
			"round-trip source layout resolves");
		pl[0].isrc = "GBAYE0000001";

		TouchFile(L"_roundtrip.bin");
		const std::wstring outPath =
			(FixtureDir() / L"roundtrip_out.cue").wstring();
		Check(WriteImportedCue(outPath, L"_roundtrip.bin", source, pl),
			"the temp CUE is emitted");

		std::ifstream emitted(outPath, std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(emitted)),
			std::istreambuf_iterator<char>());
		emitted.close();
		Check(text.compare(0, 3, "ï»¿") != 0,
			"the temp CUE carries no BOM (the consumer rejects one it cannot decode)");
		Check(text.find("CATALOG 0123456789012") < text.find("TRACK"),
			"CATALOG precedes the first TRACK, as the consumer requires");
		Check(text.find("ISRC GBAYE0000001") != std::string::npos,
			"a well-formed ISRC is written");
		size_t fileCount = 0, at = 0;
		while ((at = text.find("FILE ", at)) != std::string::npos) { fileCount++; at += 5; }
		Check(fileCount == 1,
			"the temp CUE names exactly one FILE (the consumer rejects more)");

		// Re-parse and confirm the layout survived the trip unchanged.
		CueImportSheet back;
		TouchFile(L"_roundtrip.bin");
		Check(ParseCueSheetForImport(outPath, back, err),
			"the emitted CUE parses back");
		Check(back.tracks.size() == pl.size(),
			"the emitted CUE keeps every track");
		Check(back.catalog == "0123456789012", "CATALOG survives the round trip");
		bool indicesMatch = true;
		for (size_t i = 0; i < back.tracks.size() && i < pl.size(); i++) {
			if (back.tracks[i].index01Frames != pl[i].binIndex01) indicesMatch = false;
			if (pl[i].hasIndex00 &&
				back.tracks[i].index00Frames != pl[i].binIndex00) indicesMatch = false;
			if (back.tracks[i].hasIndex00() != pl[i].hasIndex00) indicesMatch = false;
		}
		Check(indicesMatch, "every INDEX survives the round trip unchanged");

		bool increasing = true;
		for (size_t i = 1; i < back.tracks.size(); i++)
			if (back.tracks[i].index01Frames <= back.tracks[i - 1].index01Frames)
				increasing = false;
		Check(increasing && back.tracks[0].index01Frames == 0,
			"the emitted INDEX values start at zero and strictly increase");
	}

	return cueFailures;
}
