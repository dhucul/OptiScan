// ============================================================================
// CueSheetImport.cpp - see CueSheetImport.h
// ============================================================================
#define NOMINMAX
#include "CueSheetImport.h"
#include "ConsoleColors.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <utility>
#include <windows.h>

namespace {

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string utf8(static_cast<size_t>(need), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        utf8.data(), need, nullptr, nullptr) != need) return std::string();
    return utf8;
}

void TrimEnds(std::wstring& s) {
    const size_t first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) { s.clear(); return; }
    const size_t last = s.find_last_not_of(L" \t\r\n");
    s = s.substr(first, last - first + 1);
}

// Strip characters the downstream CUE writer cannot round-trip. A double quote
// ends a quoted field early in OpticalDrive::ParseCueSheet's extractQuoted, so
// a title carrying one would silently truncate or corrupt the temp sheet.
std::string SanitizeCueText(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c == '"') continue;
        if (c < 0x20 || c == 0x7F) continue;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

bool StartsWithKeyword(const std::wstring& line, const wchar_t* keyword,
                       std::wstring& rest) {
    const size_t len = wcslen(keyword);
    if (line.size() < len) return false;
    if (_wcsnicmp(line.c_str(), keyword, len) != 0) return false;
    // Require a delimiter so PERFORMER never matches as PERFORM.
    if (line.size() > len && line[len] != L' ' && line[len] != L'\t') return false;
    rest = line.substr(len);
    TrimEnds(rest);
    return true;
}

// Pull the first argument off a line: a quoted string, or the first
// whitespace-delimited token. `remainder` receives whatever followed it.
std::wstring TakeArgument(const std::wstring& in, std::wstring& remainder) {
    remainder.clear();
    if (in.empty()) return std::wstring();
    if (in.front() == L'"') {
        const size_t close = in.find(L'"', 1);
        if (close == std::wstring::npos) return in.substr(1);
        remainder = in.substr(close + 1);
        TrimEnds(remainder);
        return in.substr(1, close - 1);
    }
    const size_t space = in.find_first_of(L" \t");
    if (space == std::wstring::npos) return in;
    remainder = in.substr(space + 1);
    TrimEnds(remainder);
    return in.substr(0, space);
}

// An unquoted FILE path may contain spaces, with the type keyword as the last
// token: FILE some album.wav WAVE. Split on the LAST whitespace run instead.
std::wstring TakeFilePath(const std::wstring& in, std::wstring& typeKeyword) {
    typeKeyword.clear();
    if (in.empty()) return std::wstring();
    if (in.front() == L'"') {
        std::wstring rest;
        std::wstring path = TakeArgument(in, rest);
        typeKeyword = rest;
        return path;
    }
    const size_t lastSpace = in.find_last_of(L" \t");
    if (lastSpace == std::wstring::npos) return in;
    typeKeyword = in.substr(lastSpace + 1);
    std::wstring path = in.substr(0, lastSpace);
    TrimEnds(path);
    return path;
}

bool ParseMsf(const std::wstring& text, uint32_t& frames, std::string& err) {
    int mm = 0, ss = 0, ff = 0;
    wchar_t c1 = 0, c2 = 0;
    std::wistringstream iss(text);
    iss >> mm >> c1 >> ss >> c2 >> ff;
    if (!iss || c1 != L':' || c2 != L':') { err = "malformed MM:SS:FF time"; return false; }
    if (mm < 0 || ss < 0 || ff < 0) { err = "negative MM:SS:FF time"; return false; }
    if (ss > 59) { err = "seconds field exceeds 59"; return false; }
    if (ff > 74) { err = "frames field exceeds 74"; return false; }
    if (mm > 99) { err = "minutes field exceeds 99"; return false; }
    frames = static_cast<uint32_t>(mm) * 4500u + static_cast<uint32_t>(ss) * 75u
           + static_cast<uint32_t>(ff);
    return true;
}

bool IsAbsolutePath(const std::wstring& p) {
    if (p.size() >= 2 && p[1] == L':') return true;                   // C:\...
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') return true; // UNC
    if (!p.empty() && (p[0] == L'\\' || p[0] == L'/')) return true;   // root-relative
    return false;
}

std::wstring DirectoryOf(const std::wstring& filePath) {
    const size_t slash = filePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    if (slash == 0) return L"\\";
    return filePath.substr(0, slash);
}

void FramesToMsf(uint64_t frames, std::ostream& out) {
    const uint64_t m = frames / 4500;
    const uint64_t s = (frames / 75) % 60;
    const uint64_t f = frames % 75;
    const char fill = out.fill('0');
    out.width(2); out << m << ':';
    out.width(2); out << s << ':';
    out.width(2); out << f;
    out.fill(fill);
}

// Read the CUE's bytes and decode them, tolerating the encodings real rippers
// emit. EAC writes ANSI by default, so refusing anything but UTF-8 (as the
// writer-side parser does) would reject most sheets in the wild. Every keyword
// and number is ASCII in all of these, so only CD-Text can be misread, and only
// when the sheet carries no BOM and is not valid UTF-8.
bool ReadCueText(const std::wstring& path, std::wstring& text,
                 CueSourceEncoding& encoding, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open the CUE sheet"; return false; }
    std::string bytes((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    if (bytes.empty()) { err = "the CUE sheet is empty"; return false; }

    auto byteAt = [&](size_t i) { return static_cast<unsigned char>(bytes[i]); };

    if (bytes.size() >= 2 && byteAt(0) == 0xFF && byteAt(1) == 0xFE) {
        encoding = CueSourceEncoding::Utf16Le;
        const size_t chars = (bytes.size() - 2) / 2;
        text.resize(chars);
        for (size_t i = 0; i < chars; i++)
            text[i] = static_cast<wchar_t>(byteAt(2 + i * 2) | (byteAt(3 + i * 2) << 8));
        return true;
    }
    if (bytes.size() >= 2 && byteAt(0) == 0xFE && byteAt(1) == 0xFF) {
        encoding = CueSourceEncoding::Utf16Be;
        const size_t chars = (bytes.size() - 2) / 2;
        text.resize(chars);
        for (size_t i = 0; i < chars; i++)
            text[i] = static_cast<wchar_t>((byteAt(2 + i * 2) << 8) | byteAt(3 + i * 2));
        return true;
    }

    bool hadBom = false;
    if (bytes.size() >= 3 && byteAt(0) == 0xEF && byteAt(1) == 0xBB && byteAt(2) == 0xBF) {
        bytes.erase(0, 3);
        hadBom = true;
    }

    auto decode = [&](UINT codePage, DWORD flags) -> bool {
        if (bytes.empty()) { text.clear(); return true; }
        const int need = MultiByteToWideChar(codePage, flags, bytes.data(),
            static_cast<int>(bytes.size()), nullptr, 0);
        if (need <= 0) return false;
        text.resize(static_cast<size_t>(need));
        return MultiByteToWideChar(codePage, flags, bytes.data(),
            static_cast<int>(bytes.size()), text.data(), need) == need;
    };

    if (decode(CP_UTF8, MB_ERR_INVALID_CHARS)) {
        encoding = hadBom ? CueSourceEncoding::Utf8Bom : CueSourceEncoding::Utf8;
        return true;
    }
    if (hadBom) { err = "the CUE sheet claims UTF-8 but is not valid UTF-8"; return false; }
    if (decode(CP_ACP, 0)) {
        encoding = CueSourceEncoding::AnsiFallback;
        return true;
    }
    err = "the CUE sheet's text encoding could not be determined";
    return false;
}

std::string MsfString(uint64_t frames) {
    std::ostringstream out;
    FramesToMsf(frames, out);
    return out.str();
}

// ============================================================================
// WriteImportedCue - emit the temp .cue that OpticalDrive::ParseCueSheet will
// read back. Its expectations drive every choice here: UTF-8 with no BOM,
// CATALOG and disc-level CD-Text before the first TRACK, one FILE, and
// strictly increasing disc-absolute INDEX values.
// ============================================================================
bool AllDigits(const std::string& s, size_t count) {
	if (s.size() != count) return false;
	for (unsigned char c : s) if (c < '0' || c > '9') return false;
	return true;
}

bool LooksLikeIsrc(const std::string& s) {
	if (s.size() != 12) return false;
	for (unsigned char c : s)
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
			  (c >= 'a' && c <= 'z'))) return false;
	return true;
}

}  // namespace

bool WriteImportedCue(const std::wstring& cuePath, const std::wstring& binFileName,
	const CueImportSheet& sheet,
	const std::vector<TrackPlacement>& placements) {

	std::ofstream cue(cuePath, std::ios::binary | std::ios::trunc);
	if (!cue) return false;

	// The consumer only accepts CATALOG and disc-level TITLE/PERFORMER while it
	// is outside a TRACK, so these have to lead.
	if (!sheet.catalog.empty()) {
		if (AllDigits(sheet.catalog, 13)) {
			cue << "CATALOG " << sheet.catalog << "\n";
		}
		else {
			// A malformed MCN is encoded into the Q channel verbatim, which
			// produces a disc players can misread. Dropping it is the safe half.
			Console::Warning("CATALOG \"");
			std::cout << sheet.catalog
				<< "\" is not a 13-digit media catalogue number -- not written\n";
		}
	}
	if (!sheet.title.empty())     cue << "TITLE \"" << sheet.title << "\"\n";
	if (!sheet.performer.empty()) cue << "PERFORMER \"" << sheet.performer << "\"\n";

	cue << "FILE \"" << WideToUtf8(binFileName) << "\" BINARY\n";

	for (const auto& p : placements) {
		cue << "  TRACK " << std::setfill('0') << std::setw(2) << p.trackNumber
			<< " AUDIO\n" << std::setfill(' ');
		if (!p.title.empty())     cue << "    TITLE \"" << p.title << "\"\n";
		if (!p.performer.empty()) cue << "    PERFORMER \"" << p.performer << "\"\n";
		if (!p.isrc.empty()) {
			if (LooksLikeIsrc(p.isrc)) cue << "    ISRC " << p.isrc << "\n";
			else {
				Console::Warning("Track ");
				std::cout << p.trackNumber << ": ISRC \"" << p.isrc
					<< "\" is not 12 alphanumeric characters -- not written\n";
			}
		}
		// PREGAP/POSTGAP are deliberately not re-emitted: the silence they
		// asked for is already in the image, and INDEX 00 now marks it.
		if (p.hasIndex00) cue << "    INDEX 00 " << MsfString(p.binIndex00) << "\n";
		cue << "    INDEX 01 " << MsfString(p.binIndex01) << "\n";
	}

	cue.flush();
	return cue.good();
}


const char* CueEncodingName(CueSourceEncoding e) {
    switch (e) {
    case CueSourceEncoding::Utf8Bom:      return "UTF-8 (BOM)";
    case CueSourceEncoding::Utf8:         return "UTF-8";
    case CueSourceEncoding::Utf16Le:      return "UTF-16 LE";
    case CueSourceEncoding::Utf16Be:      return "UTF-16 BE";
    case CueSourceEncoding::AnsiFallback: return "system ANSI code page";
    }
    return "unknown";
}

const char* CueGapModeName(CueGapMode m) {
    switch (m) {
    case CueGapMode::None:      return "no gaps (tracks abut)";
    case CueGapMode::Appended:  return "gaps appended to the previous track's file";
    case CueGapMode::Prepended: return "gaps prepended to each track's own file";
    case CueGapMode::Mixed:     return "mixed";
    }
    return "unknown";
}

CueGapMode ClassifyGapMode(const CueImportSheet& sheet) {
    bool appended = false, prepended = false;
    for (const auto& t : sheet.tracks) {
        if (!t.hasIndex00()) continue;
        if (t.index00File != t.index01File) appended = true;
        else if (t.index00Frames < t.index01Frames) prepended = true;
    }
    if (appended && prepended) return CueGapMode::Mixed;
    if (appended) return CueGapMode::Appended;
    if (prepended) return CueGapMode::Prepended;
    return CueGapMode::None;
}

// ============================================================================
// ParseCueSheetForImport
// ============================================================================
bool ParseCueSheetForImport(const std::wstring& cuePath, CueImportSheet& out,
                            std::string& err) {
    out = CueImportSheet{};
    err.clear();
    out.cuePath = cuePath;
    out.baseDir = DirectoryOf(cuePath);

    std::wstring text;
    if (!ReadCueText(cuePath, text, out.encoding, err)) return false;

    std::wistringstream stream(text);
    std::wstring rawLine;
    size_t lineNo = 0;
    size_t currentFile = kCueNoFile;
    size_t currentTrack = static_cast<size_t>(-1);

    auto fail = [&](const std::string& message) {
        err = "line " + std::to_string(lineNo) + ": " + message;
        return false;
    };
    // out.tracks reallocates as it grows, so an index is held rather than a
    // pointer: a stale CueImportTrack* would dangle the moment a later TRACK
    // line pushed the vector past its capacity.
    auto track = [&]() -> CueImportTrack& { return out.tracks[currentTrack]; };

    while (std::getline(stream, rawLine)) {
        ++lineNo;
        std::wstring line = rawLine;
        TrimEnds(line);
        if (line.empty()) continue;

        std::wstring rest;
        const bool inTrack = (currentTrack != static_cast<size_t>(-1));

        if (StartsWithKeyword(line, L"REM", rest)) continue;

        if (StartsWithKeyword(line, L"FILE", rest)) {
            std::wstring typeKeyword;
            std::wstring path = TakeFilePath(rest, typeKeyword);
            if (path.empty()) return fail("FILE directive has no path");

            for (auto& ch : path) if (ch == L'/') ch = L'\\';
            if (!IsAbsolutePath(path)) path = out.baseDir + L"\\" + path;

            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                return fail("the file this CUE references does not exist: " +
                            WideToUtf8(path));

            CueFileRef ref;
            ref.path = path;
            ref.typeKeyword = typeKeyword;
            out.files.push_back(ref);
            currentFile = out.files.size() - 1;
            continue;
        }

        if (StartsWithKeyword(line, L"TRACK", rest)) {
            std::wstring remainder;
            const std::wstring numberText = TakeArgument(rest, remainder);
            int number = 0;
            try { number = std::stoi(numberText); }
            catch (...) { return fail("TRACK has a non-numeric track number"); }
            if (number < 1 || number > 99)
                return fail("track number is outside 1..99");

            int dataMode = 0;
            std::wstring mode = remainder;
            std::transform(mode.begin(), mode.end(), mode.begin(), ::towupper);
            if (mode == L"AUDIO")           dataMode = 0;
            else if (mode == L"MODE1/2352") dataMode = 1;
            else if (mode == L"MODE2/2352") dataMode = 2;
            else return fail("unsupported track mode (only AUDIO, MODE1/2352 and "
                             "MODE2/2352 can be written)");

            // A track may legitimately be declared twice: EAC's default "gaps
            // appended" puts a track's INDEX 00 under the previous track's FILE
            // and its INDEX 01 under its own, repeating the TRACK line in each.
            size_t existing = static_cast<size_t>(-1);
            for (size_t i = 0; i < out.tracks.size(); i++)
                if (out.tracks[i].number == number) { existing = i; break; }

            if (existing != static_cast<size_t>(-1)) {
                if (out.tracks[existing].dataMode != dataMode)
                    return fail("track is declared twice with different modes");
                currentTrack = existing;
            }
            else {
                CueImportTrack t;
                t.number = number;
                t.isAudio = (dataMode == 0);
                t.dataMode = dataMode;
                out.tracks.push_back(t);
                currentTrack = out.tracks.size() - 1;
            }
            continue;
        }

        if (StartsWithKeyword(line, L"INDEX", rest)) {
            if (!inTrack) return fail("INDEX appears before any TRACK");
            if (currentFile == kCueNoFile) return fail("INDEX appears before any FILE");
            std::wstring remainder;
            const std::wstring numberText = TakeArgument(rest, remainder);
            int indexNumber = 0;
            try { indexNumber = std::stoi(numberText); }
            catch (...) { return fail("INDEX has a non-numeric index number"); }

            uint32_t frames = 0;
            std::string msfErr;
            if (!ParseMsf(remainder, frames, msfErr)) return fail("INDEX " + msfErr);

            if (indexNumber == 0) {
                track().index00File = currentFile;
                track().index00Frames = frames;
            }
            else if (indexNumber == 1) {
                track().index01File = currentFile;
                track().index01Frames = frames;
            }
            else {
                // Sub-indices have no representation in SEND CUE SHEET, which
                // carries INDEX 00 and INDEX 01 only.
                Console::Warning("Track ");
                std::cout << track().number << ": INDEX " << indexNumber
                          << " ignored (sub-indices cannot be written)\n";
            }
            continue;
        }

        if (StartsWithKeyword(line, L"PREGAP", rest)) {
            if (!inTrack) return fail("PREGAP appears before any TRACK");
            std::string msfErr;
            if (!ParseMsf(rest, track().pregapFrames, msfErr))
                return fail("PREGAP " + msfErr);
            continue;
        }
        if (StartsWithKeyword(line, L"POSTGAP", rest)) {
            if (!inTrack) return fail("POSTGAP appears before any TRACK");
            std::string msfErr;
            if (!ParseMsf(rest, track().postgapFrames, msfErr))
                return fail("POSTGAP " + msfErr);
            continue;
        }
        if (StartsWithKeyword(line, L"ISRC", rest)) {
            if (!inTrack) return fail("ISRC appears before any TRACK");
            std::wstring remainder;
            track().isrc = SanitizeCueText(WideToUtf8(TakeArgument(rest, remainder)));
            continue;
        }
        if (StartsWithKeyword(line, L"CATALOG", rest)) {
            std::wstring remainder;
            out.catalog = SanitizeCueText(WideToUtf8(TakeArgument(rest, remainder)));
            continue;
        }
        if (StartsWithKeyword(line, L"TITLE", rest)) {
            std::wstring remainder;
            const std::string value =
                SanitizeCueText(WideToUtf8(TakeArgument(rest, remainder)));
            if (inTrack) track().title = value; else out.title = value;
            continue;
        }
        if (StartsWithKeyword(line, L"PERFORMER", rest)) {
            std::wstring remainder;
            const std::string value =
                SanitizeCueText(WideToUtf8(TakeArgument(rest, remainder)));
            if (inTrack) track().performer = value; else out.performer = value;
            continue;
        }
        if (StartsWithKeyword(line, L"FLAGS", rest)) {
            if (!inTrack) continue;
            std::wstring flags = rest;
            std::transform(flags.begin(), flags.end(), flags.begin(), ::towupper);
            std::wistringstream fs(flags);
            std::wstring flag;
            while (fs >> flag) {
                if (flag == L"PRE")  track().flagPre = true;
                if (flag == L"DCP")  track().flagDcp = true;
                if (flag == L"4CH")  track().flag4ch = true;
                if (flag == L"SCMS") track().flagScms = true;
            }
            continue;
        }
        // SONGWRITER, CDTEXTFILE and anything else we cannot write are ignored.
    }

    if (out.files.empty())  { err = "no FILE directive found in the CUE sheet"; return false; }
    if (out.tracks.empty()) { err = "no TRACK found in the CUE sheet"; return false; }

    for (auto& t : out.tracks) {
        if (t.index01File == kCueNoFile) {
            err = "track " + std::to_string(t.number) + " has no INDEX 01";
            return false;
        }
        // A same-file INDEX 00 that comes after INDEX 01 is not a pregap at all.
        if (t.hasIndex00() && t.index00File == t.index01File &&
            t.index00Frames > t.index01Frames) {
            Console::Warning("Track ");
            std::cout << t.number
                      << ": INDEX 00 comes after INDEX 01 -- ignoring the pregap\n";
            t.index00File = kCueNoFile;
            t.index00Frames = 0;
        }
    }

    for (size_t i = 1; i < out.tracks.size(); i++) {
        if (out.tracks[i].number <= out.tracks[i - 1].number) {
            err = "track numbers are not strictly increasing (track " +
                  std::to_string(out.tracks[i].number) + " follows track " +
                  std::to_string(out.tracks[i - 1].number) + ")";
            return false;
        }
    }

    out.singleFileLayout = (out.files.size() == 1 && out.tracks.size() > 1);
    return true;
}

// ============================================================================
// BuildBinLayout
// ============================================================================
bool BuildBinLayout(const CueImportSheet& sheet,
                    const std::vector<uint32_t>& fileSectorCounts,
                    std::vector<BinSegment>& segments,
                    std::vector<TrackPlacement>& placements,
                    uint64_t& totalSectors,
                    std::string& err) {
    segments.clear();
    placements.clear();
    totalSectors = 0;
    err.clear();

    if (fileSectorCounts.size() != sheet.files.size()) {
        err = "internal error: measured file count does not match the CUE";
        return false;
    }

    std::vector<CueImportTrack> tracks = sheet.tracks;

    // ── Data tracks ─────────────────────────────────────────────────────
    // An audio-only burn cannot reproduce a data track's layout, so the only
    // shape we can honour is audio tracks followed by data tracks, which we
    // drop along with the sectors they alone occupy.
    if (!tracks.front().isAudio) {
        err = "the CUE begins with a data track; an audio-only write cannot "
              "preserve its layout";
        return false;
    }
    size_t firstData = tracks.size();
    for (size_t i = 0; i < tracks.size(); i++)
        if (!tracks[i].isAudio) { firstData = i; break; }
    for (size_t i = firstData; i < tracks.size(); i++) {
        if (tracks[i].isAudio) {
            err = "a data track sits between audio tracks; an audio-only write "
                  "cannot preserve the LBA layout that requires";
            return false;
        }
    }

    std::vector<uint32_t> fileStart(sheet.files.size(), 0);
    std::vector<uint32_t> fileEnd = fileSectorCounts;
    size_t lastFile = sheet.files.size() - 1;

    if (firstData < tracks.size()) {
        const CueImportTrack& d = tracks[firstData];
        const size_t   cutFile  = d.hasIndex00() ? d.index00File  : d.index01File;
        const uint32_t cutFrame = d.hasIndex00() ? d.index00Frames : d.index01Frames;
        if (cutFile >= fileEnd.size()) { err = "internal error: data track names an unknown file"; return false; }
        if (cutFrame > fileEnd[cutFile]) {
            err = "the data track starts past the end of the file it names";
            return false;
        }
        Console::Warning("Skipped ");
        std::cout << (tracks.size() - firstData)
                  << " data track(s) -- this is an audio-only write\n";
        lastFile = cutFile;
        fileEnd[cutFile] = cutFrame;
        tracks.erase(tracks.begin() + firstData, tracks.end());
        if (tracks.empty()) { err = "the CUE contains no audio tracks"; return false; }
    }

    // ── Track 1 lead-in ─────────────────────────────────────────────────
    // WriteAudioSectors synthesizes the mandatory 150-sector pregap itself
    // (it starts writing at LBA -150), and both the SEND CUE SHEET builder and
    // the sector writer gate INDEX 00 on "not the first track". So neither a
    // PREGAP nor an INDEX 00 on track 1 is representable here.
    {
        CueImportTrack& t1 = tracks.front();
        if (t1.pregapFrames != 0) {
            if (t1.pregapFrames != 150) {
                Console::Warning("Track 1 declares a ");
                std::cout << t1.pregapFrames
                          << "-frame PREGAP; the writer always emits the standard "
                             "150-frame lead-in instead\n";
            }
            t1.pregapFrames = 0;
        }
        if (t1.hasIndex00()) {
            Console::Info("Track 1 INDEX 00 dropped -- the lead-in is generated by "
                          "the writer, not taken from the audio files.\n");
            t1.index00File = kCueNoFile;
            t1.index00Frames = 0;
        }
    }

    const size_t firstFile = tracks.front().index01File;
    if (firstFile > lastFile) {
        err = "track 1 starts in a file that the layout does not reach";
        return false;
    }
    if (firstFile >= fileStart.size()) {
        err = "internal error: track 1 names an unknown file";
        return false;
    }
    // Audio before track 1's INDEX 01 is hidden-track audio (HTOA). A written
    // disc has no addressable space for it, so it is trimmed off the front.
    fileStart[firstFile] = tracks.front().index01Frames;
    if (tracks.front().index01Frames > 0 || firstFile > 0) {
        uint64_t skipped = tracks.front().index01Frames;
        for (size_t i = 0; i < firstFile; i++) skipped += fileSectorCounts[i];
        Console::Warning("Skipping ");
        std::cout << skipped << " sector(s) of audio before track 1 -- hidden "
                     "pre-track-1 audio cannot be reproduced by this writer\n";
    }

    for (size_t i = firstFile; i <= lastFile; i++) {
        if (fileEnd[i] < fileStart[i]) {
            err = "the CUE trims a file to a negative length";
            return false;
        }
    }

    // ── Validate every index against the files that were measured ───────
    auto checkIndex = [&](const CueImportTrack& t, size_t file, uint32_t frame,
                          const char* which) -> bool {
        if (file < firstFile || file > lastFile) {
            err = "track " + std::to_string(t.number) + " " + which +
                  " points at a file outside the writable range";
            return false;
        }
        if (frame < fileStart[file] || frame > fileEnd[file]) {
            std::ostringstream m;
            m << "track " << t.number << " " << which << " is at ";
            FramesToMsf(frame, m);
            m << " but \"" << WideToUtf8(sheet.files[file].path)
              << "\" only holds " << (fileEnd[file] - fileStart[file])
              << " sector(s) -- the audio file is shorter than the CUE says";
            err = m.str();
            return false;
        }
        return true;
    };
    for (const auto& t : tracks) {
        if (!checkIndex(t, t.index01File, t.index01Frames, "INDEX 01")) return false;
        if (t.hasIndex00() && !checkIndex(t, t.index00File, t.index00Frames, "INDEX 00"))
            return false;
    }

    // ── Silence cut points ──────────────────────────────────────────────
    // A generated gap is inserted immediately before the earliest position the
    // track owns: its INDEX 00 when the CUE supplies gap audio, otherwise its
    // INDEX 01. Keyed by (file, frame) and ordered, so walking the map in order
    // walks the image in order.
    std::map<std::pair<size_t, uint32_t>, uint64_t> silenceAt;
    uint64_t trailingSilence = 0;

    auto anchorOf = [](const CueImportTrack& t) {
        return t.hasIndex00() ? std::make_pair(t.index00File, t.index00Frames)
                              : std::make_pair(t.index01File, t.index01Frames);
    };

    for (size_t k = 0; k < tracks.size(); k++) {
        if (tracks[k].pregapFrames > 0)
            silenceAt[anchorOf(tracks[k])] += tracks[k].pregapFrames;
        if (tracks[k].postgapFrames > 0) {
            if (k + 1 < tracks.size())
                silenceAt[anchorOf(tracks[k + 1])] += tracks[k].postgapFrames;
            else
                trailingSilence += tracks[k].postgapFrames;
        }
    }
    for (const auto& kv : silenceAt) {
        const size_t f = kv.first.first;
        const uint32_t frame = kv.first.second;
        if (f < firstFile || f > lastFile || frame < fileStart[f] || frame > fileEnd[f]) {
            err = "a PREGAP or POSTGAP falls outside the audio files the CUE names";
            return false;
        }
    }

    // ── Absolute LBA resolution ─────────────────────────────────────────
    // Silence inserted at a cut point pushes everything at or after that point
    // forward, so a position's absolute LBA is its position in the plain
    // concatenation plus every silence inserted at or before it.
    std::vector<uint64_t> rawBase(sheet.files.size(), 0);
    uint64_t running = 0;
    for (size_t i = firstFile; i <= lastFile; i++) {
        rawBase[i] = running;
        running += (fileEnd[i] - fileStart[i]);
    }

    auto absOf = [&](size_t file, uint32_t frame) -> uint64_t {
        uint64_t silence = 0;
        for (const auto& kv : silenceAt) {
            const size_t f = kv.first.first;
            const uint32_t cf = kv.first.second;
            if (f > file || (f == file && cf > frame)) break;   // map is ordered
            silence += kv.second;
        }
        return rawBase[file] + (frame - fileStart[file]) + silence;
    };

    // ── Emit segments ───────────────────────────────────────────────────
    uint64_t cursor = 0;
    auto pushFileRange = [&](size_t file, uint32_t start, uint32_t count) {
        if (count == 0) return;
        BinSegment s;
        s.kind = BinSegment::Kind::FileRange;
        s.fileIndex = file;
        s.startSector = start;
        s.sectorCount = count;
        segments.push_back(s);
        cursor += count;
    };
    auto pushSilence = [&](uint64_t count) {
        if (count == 0) return;
        BinSegment s;
        s.kind = BinSegment::Kind::Silence;
        s.sectorCount = static_cast<uint32_t>(count);
        segments.push_back(s);
        cursor += count;
    };

    for (size_t i = firstFile; i <= lastFile; i++) {
        uint32_t prev = fileStart[i];
        for (const auto& kv : silenceAt) {
            if (kv.first.first != i) continue;
            const uint32_t cut = kv.first.second;
            pushFileRange(i, prev, cut - prev);
            pushSilence(kv.second);
            prev = cut;
        }
        pushFileRange(i, prev, fileEnd[i] - prev);
    }
    pushSilence(trailingSilence);

    totalSectors = cursor;
    if (totalSectors == 0) { err = "the CUE resolves to an empty disc"; return false; }

    // ── Placements ──────────────────────────────────────────────────────
    // Renumbered 1..N: a CUE that starts at track 2 would otherwise produce a
    // disc whose TOC starts at 2, which no player expects.
    if (tracks.front().number != 1) {
        Console::Warning("The CUE starts at track ");
        std::cout << tracks.front().number
                  << "; the burned disc will be renumbered from track 1\n";
    }
    for (size_t k = 0; k < tracks.size(); k++) {
        const CueImportTrack& t = tracks[k];
        TrackPlacement p;
        p.trackNumber = static_cast<int>(k + 1);
        p.title = t.title;
        p.performer = t.performer;
        p.isrc = t.isrc;

        const uint64_t index01 = absOf(t.index01File, t.index01Frames);
        p.binIndex01 = static_cast<uint32_t>(index01);

        // A gap exists when the CUE supplies gap audio (INDEX 00), when it asks
        // for generated silence (PREGAP), or both -- and the disc's INDEX 00
        // must mark the start of the whole gap, silence included.
        if (t.hasIndex00() || t.pregapFrames > 0) {
            const uint64_t gapAudioStart = t.hasIndex00()
                ? absOf(t.index00File, t.index00Frames)
                : index01;
            if (gapAudioStart < t.pregapFrames) {
                err = "track " + std::to_string(t.number) +
                      " has a pregap that starts before the beginning of the disc";
                return false;
            }
            const uint64_t gapStart = gapAudioStart - t.pregapFrames;
            if (gapStart >= index01) {
                err = "track " + std::to_string(t.number) +
                      " has a zero-length or inverted pregap";
                return false;
            }
            p.hasIndex00 = true;
            p.binIndex00 = static_cast<uint32_t>(gapStart);
        }
        placements.push_back(p);
    }

    if (placements.front().binIndex01 != 0) {
        err = "internal error: track 1 did not land at the start of the image";
        return false;
    }
    for (size_t k = 1; k < placements.size(); k++) {
        const uint32_t prevStart = placements[k - 1].binIndex01;
        const uint32_t thisStart = placements[k].hasIndex00 ? placements[k].binIndex00
                                                            : placements[k].binIndex01;
        if (thisStart <= prevStart) {
            err = "track " + std::to_string(placements[k].trackNumber) +
                  " does not start after the track before it";
            return false;
        }
    }

    if (totalSectors > kMaxAudioSectors) {
        std::ostringstream m;
        FramesToMsf(totalSectors, m);
        Console::Warning("The image is ");
        std::cout << m.str() << " long, past the " << kMaxAudioSectors
                  << "-sector Red Book maximum. The drive will reject it unless "
                     "the blank is overburn-capable.\n";
    }
    return true;
}
