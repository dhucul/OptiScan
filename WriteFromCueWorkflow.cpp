// ============================================================================
// WriteFromCueWorkflow.cpp - Write an audio CD from a CUE sheet plus the WAV or
// FLAC files it references.
//
// Workflow:
//   1. Pick a CUE sheet. Parse it, resolving every FILE against the sheet's
//      own directory.
//   2. Probe each referenced file (decoding FLAC to a temp WAV first) so its
//      true length in sectors is known.
//   3. Resolve the sheet into an absolute BIN layout - a segment list of file
//      ranges and generated silence - and show the user the resulting track
//      table before anything touches a disc.
//   4. Pick the burner and, if its offset is known, the write compensation.
//   5. Materialize the temp .bin/.cue, then reuse WriteDisc() for the burn.
//
// Nothing here reads a source disc. The ordering above is deliberate: every
// way this can fail on bad input happens before the drive is asked to blank
// anything, so a malformed CUE can never cost the user a CD-RW.
// ============================================================================
#define NOMINMAX
#include "WriteFromCueWorkflow.h"
#include "AudioFileSource.h"
#include "ConsoleColors.h"
#include "Constants.h"
#include "CueSheetImport.h"
#include "Drive.h"
#include "DriveSelection.h"
#include "FileUtils.h"
#include "GuiInput.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include "Progress.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace {

// Deletes everything it was handed, on every path out of the workflow. The
// sibling write-tracks workflow uses a removeTemps() lambda called at each
// early return instead, which is one forgotten `return` away from leaving a
// 700 MB image behind.
struct TempFiles {
	std::vector<std::wstring> paths;
	void Track(const std::wstring& p) { paths.push_back(p); }
	~TempFiles() {
		for (const auto& p : paths) DeleteFileW(p.c_str());
	}
};

bool Cancelled() {
	return g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey();
}

std::wstring FileNameOf(const std::wstring& path) {
	const size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::string MsfText(uint64_t frames) {
	std::ostringstream out;
	out << std::setfill('0') << std::setw(2) << (frames / 4500) << ':'
		<< std::setw(2) << ((frames / 75) % 60) << ':'
		<< std::setw(2) << (frames % 75);
	return out.str();
}

// ============================================================================
// WriteSegmentsToBin - materialize the resolved layout as a flat 2352-byte
// image. WriteSourcesToBin in the write-tracks workflow can't serve here: it
// assumes one file per track laid out as pregap-then-body, whereas a single-
// FILE CUE is one file spanning every track and an appended-gap CUE has a
// track straddling two files.
// ============================================================================
bool WriteSegmentsToBin(const std::wstring& binPath,
	const std::vector<BinSegment>& segments,
	const std::vector<WavPayload>& files,
	uint64_t totalSectors) {

	std::ofstream bin(binPath, std::ios::binary | std::ios::trunc);
	if (!bin) {
		Console::Error("Cannot create the temporary image file.\n");
		return false;
	}

	constexpr uint32_t kBatchSectors = 64;
	std::vector<char> buffer(static_cast<size_t>(kBatchSectors) * AUDIO_SECTOR_SIZE, 0);
	const std::vector<char> silence(buffer.size(), 0);

	ProgressIndicator progress;
	progress.SetLabel("Building image");
	progress.Start();

	uint64_t written = 0;
	auto report = [&]() {
		// The indicator takes ints; a full disc is ~360k sectors, so this is
		// well inside range and the cast can't wrap.
		progress.Update(static_cast<int>(written), static_cast<int>(totalSectors));
	};

	// One open handle per file, kept across consecutive segments: a single-FILE
	// CUE with a mid-image PREGAP re-enters the same file after the silence.
	std::vector<std::unique_ptr<std::ifstream>> handles(files.size());

	for (const auto& segment : segments) {
		if (Cancelled()) { progress.Finish(false); return false; }

		if (segment.kind == BinSegment::Kind::Silence) {
			uint32_t remaining = segment.sectorCount;
			while (remaining > 0) {
				const uint32_t batch = (std::min)(remaining, kBatchSectors);
				bin.write(silence.data(),
					static_cast<std::streamsize>(batch) * AUDIO_SECTOR_SIZE);
				if (!bin) { progress.Finish(false); return false; }
				remaining -= batch;
				written += batch;
				report();
				if (Cancelled()) { progress.Finish(false); return false; }
			}
			continue;
		}

		const size_t fileIndex = segment.fileIndex;
		if (fileIndex >= files.size()) { progress.Finish(false); return false; }
		const WavPayload& payload = files[fileIndex];

		if (!handles[fileIndex]) {
			handles[fileIndex] =
				std::make_unique<std::ifstream>(payload.wavPath, std::ios::binary);
			if (!*handles[fileIndex]) {
				progress.Finish(false);
				Console::Error("Cannot reopen an audio file the CUE names.\n");
				return false;
			}
		}
		std::ifstream& in = *handles[fileIndex];
		in.clear();
		in.seekg(static_cast<std::streamoff>(payload.dataOffset) +
			static_cast<std::streamoff>(segment.startSector) * AUDIO_SECTOR_SIZE);
		if (!in) { progress.Finish(false); return false; }

		// Bytes of real payload still available from this segment's start. Only
		// the file's genuine tail may fall short of a whole sector.
		const uint64_t segmentStartByte =
			static_cast<uint64_t>(segment.startSector) * AUDIO_SECTOR_SIZE;
		uint64_t available = payload.dataBytes > segmentStartByte
			? payload.dataBytes - segmentStartByte : 0;
		const bool reachesFileTail =
			(segment.startSector + segment.sectorCount) >= payload.sectorCount;

		uint32_t remaining = segment.sectorCount;
		while (remaining > 0) {
			const uint32_t batch = (std::min)(remaining, kBatchSectors);
			const uint64_t want = static_cast<uint64_t>(batch) * AUDIO_SECTOR_SIZE;
			const uint64_t request = (std::min)(want, available);

			std::fill(buffer.begin(), buffer.begin() + static_cast<size_t>(want), 0);
			if (request > 0) {
				in.read(buffer.data(), static_cast<std::streamsize>(request));
				if (static_cast<uint64_t>(in.gcount()) != request) {
					progress.Finish(false);
					Console::Error("An audio file ended earlier than its header promised.\n");
					std::wcout << L"  " << payload.originalPath << L"\n";
					return false;
				}
				available -= request;
			}
			if (request < want && !reachesFileTail) {
				// Padding here would silently insert silence in the middle of
				// the image and shift every following track, so refuse instead.
				progress.Finish(false);
				Console::Error("An audio file is shorter than the CUE's layout requires.\n");
				std::wcout << L"  " << payload.originalPath << L"\n";
				return false;
			}

			bin.write(buffer.data(), static_cast<std::streamsize>(want));
			if (!bin) { progress.Finish(false); return false; }
			remaining -= batch;
			written += batch;
			report();
			if (Cancelled()) { progress.Finish(false); return false; }
		}
	}

	handles.clear();
	bin.close();
	const bool ok = bin.good() && written == totalSectors;
	progress.Finish(ok, static_cast<int>(totalSectors));
	if (!ok) Console::Error("The temporary image was not written completely.\n");
	return ok;
}

// Print the resolved layout so the user can sanity-check it against the album
// they think they picked, before a blank is committed to anything.
void PrintTrackTable(const std::vector<TrackPlacement>& placements,
	uint64_t totalSectors) {
	Console::Info("\nResolved disc layout:\n");
	for (size_t i = 0; i < placements.size(); i++) {
		const auto& p = placements[i];
		const uint64_t end = (i + 1 < placements.size())
			? (placements[i + 1].hasIndex00 ? placements[i + 1].binIndex00
			                                : placements[i + 1].binIndex01)
			: totalSectors;
		std::cout << "  Track " << std::setw(2) << p.trackNumber
			<< "  start " << MsfText(p.binIndex01)
			<< "  length " << MsfText(end > p.binIndex01 ? end - p.binIndex01 : 0);
		if (p.hasIndex00) {
			std::cout << "  gap " << std::setw(3)
				<< (p.binIndex01 - p.binIndex00) << "f";
		}
		else {
			std::cout << "          ";
		}
		if (!p.title.empty()) std::cout << "  \"" << p.title << "\"";
		std::cout << "\n";
	}
	Console::Info("  Total: ");
	std::cout << placements.size() << " tracks, " << MsfText(totalSectors)
		<< " (" << totalSectors << " sectors)\n";
}

}  // namespace

// ============================================================================
// RunWriteFromCueWorkflow
// ============================================================================
void RunWriteFromCueWorkflow(OpticalDrive& copier, const std::wstring& workDir,
	wchar_t& audioDrive, bool* outCompleted) {

	if (outCompleted) *outCompleted = false;
	TempFiles temps;

	Console::BoxHeading("Write Disc from CUE Sheet");
	Console::Info("Burns an audio CD from a CUE sheet and the WAV/FLAC files it\n"
		"references. No source disc is needed -- the track layout, gaps and\n"
		"CD-Text all come from the CUE.\n\n");

	// ── 1. Pick and parse the CUE ───────────────────────────────────────
	// A file picker rather than a folder picker: a rip folder routinely holds
	// several CUE sheets (one per format, or one per gap mode) beside a dozen
	// WAVs, so "the first .cue we find" would be a coin flip.
	const std::wstring cuePath = GuiInput::PromptForFile(
		L"Choose a CUE sheet", L"CUE sheets", L"*.cue", workDir);
	if (cuePath.empty()) {
		Console::Info("Cancelled (no CUE sheet selected).\n");
		return;
	}
	std::wcout << L"CUE: " << cuePath << L"\n";

	CueImportSheet sheet;
	std::string err;
	if (!ParseCueSheetForImport(cuePath, sheet, err)) {
		Console::Error("Could not read the CUE sheet.\n");
		Console::Info("  ");
		std::cout << err << "\n";
		return;
	}

	if (sheet.encoding == CueSourceEncoding::AnsiFallback) {
		Console::Warning("The CUE sheet is not UTF-8; it was decoded as the system "
			"ANSI code page. Check any accented CD-Text below.\n");
	}
	Console::Success("Parsed CUE sheet: ");
	std::cout << sheet.tracks.size() << " tracks across " << sheet.files.size()
		<< (sheet.files.size() == 1 ? " file" : " files")
		<< " (" << CueEncodingName(sheet.encoding) << ")\n";
	Console::Info("  Layout: ");
	std::cout << (sheet.singleFileLayout ? "single image split by INDEX offsets"
	                                     : "one audio file per track")
		<< "; " << CueGapModeName(ClassifyGapMode(sheet)) << "\n";
	if (!sheet.performer.empty() || !sheet.title.empty()) {
		Console::Info("  CD-Text: ");
		if (!sheet.performer.empty()) std::cout << sheet.performer;
		if (!sheet.performer.empty() && !sheet.title.empty()) std::cout << " - ";
		if (!sheet.title.empty()) std::cout << sheet.title;
		std::cout << "\n";
	}

	// ── 2. Probe every referenced file ──────────────────────────────────
	bool needsFlac = false;
	for (const auto& f : sheet.files)
		if (EndsWithLower(f.path, L".flac")) needsFlac = true;
	if (needsFlac && !FlacDecoderAvailable()) {
		Console::Error("This CUE references FLAC files but flac.exe was not found.\n");
		Console::Info("Install FLAC (and put flac.exe on PATH), or supply WAV files.\n");
		return;
	}

	if (needsFlac) {
		// Decoded WAVs plus the image can be ~1.5 GB for a full single-FILE
		// FLAC rip, and running out mid-build wastes the whole decode.
		ULARGE_INTEGER freeBytes{};
		if (GetDiskFreeSpaceExW(workDir.c_str(), &freeBytes, nullptr, nullptr)) {
			constexpr ULONGLONG kNeeded = 1500ull * 1024 * 1024;
			if (freeBytes.QuadPart < kNeeded) {
				Console::Warning("The output folder has under 1.5 GB free. "
					"Decoding FLAC and building the image may run out of space.\n");
				if (!GuiInput::PromptYesNo("Low disk space",
					"The output folder may not have room for the decoded audio "
					"and the disc image.\n\nContinue anyway?")) {
					Console::Info("Write cancelled.\n");
					return;
				}
			}
		}
	}

	Console::Info("\nReading the audio files the CUE names...\n");
	std::vector<WavPayload> payloads(sheet.files.size());
	std::vector<uint32_t> sectorCounts(sheet.files.size(), 0);
	for (size_t i = 0; i < sheet.files.size(); i++) {
		if (Cancelled()) { Console::Warning("Cancelled.\n"); return; }

		wchar_t tempName[64];
		swprintf_s(tempName, L"\\_writecue_in_%zu.wav", i);
		const std::wstring tempWav = workDir + tempName;

		// Register the decode target BEFORE decoding. flac.exe creates the
		// output file as it runs, so a decode that fails or is cancelled --
		// and a successful decode whose WAV then fails the format probe --
		// leaves a partial file behind. Tracking it only on success leaked
		// that file, which for a single-FILE album is several hundred MB.
		if (EndsWithLower(sheet.files[i].path, L".flac")) temps.Track(tempWav);

		std::string fileErr;
		if (!PrepareAudioFile(sheet.files[i].path, tempWav, payloads[i], fileErr)) {
			Console::Error("Cannot use an audio file the CUE names:\n");
			std::wcout << L"  " << sheet.files[i].path << L"\n";
			Console::Info("  ");
			std::cout << fileErr << "\n";
			return;
		}
		sectorCounts[i] = payloads[i].sectorCount;

		std::cout << "  " << std::setw(2) << (i + 1) << ". ";
		std::wcout << FileNameOf(sheet.files[i].path);
		std::cout << "  (" << MsfText(payloads[i].sectorCount) << ")\n";
	}

	// ── 3. Resolve the absolute layout ──────────────────────────────────
	std::vector<BinSegment> segments;
	std::vector<TrackPlacement> placements;
	uint64_t totalSectors = 0;
	if (!BuildBinLayout(sheet, sectorCounts, segments, placements, totalSectors, err)) {
		Console::Error("The CUE sheet and the audio files do not agree.\n");
		Console::Info("  ");
		std::cout << err << "\n";
		return;
	}
	// A file whose payload is not a whole number of sectors gets its tail
	// zero-padded. That only matters when something follows it in the image:
	// at the very end it is inaudible, but in the middle it shifts every later
	// track by up to a sector. Driven by the resolved segments rather than the
	// file list, so a file the layout trimmed away or never reached cannot draw
	// a warning about a shift it does not cause.
	for (size_t s = 0; s + 1 < segments.size(); s++) {
		if (segments[s].kind != BinSegment::Kind::FileRange) continue;
		const WavPayload& padded = payloads[segments[s].fileIndex];
		if ((padded.dataBytes % AUDIO_SECTOR_SIZE) == 0) continue;
		// Only the segment running to the file's own tail carries the padding;
		// a split range that stops earlier is exact.
		if (segments[s].startSector + segments[s].sectorCount != padded.sectorCount)
			continue;
		Console::Warning("\"");
		std::wcout << FileNameOf(padded.originalPath);
		std::cout << "\" does not end on a sector boundary; its tail is padded "
			"and every later track shifts by up to one sector\n";
	}

	PrintTrackTable(placements, totalSectors);

	// Stop here rather than at WriteDisc's capacity check: that one runs only
	// after the image exists, and a CUE with oversized PREGAP values can
	// resolve to tens of gigabytes of generated silence. Writing all of it to
	// disk just to have the drive reject it spends a lot of the user's time
	// and free space on a foregone conclusion.
	if (totalSectors > kUnburnableSectors) {
		Console::Error("This CUE resolves to more audio than any CD can hold.\n");
		Console::Info("  ");
		std::cout << totalSectors << " sectors (" << MsfText(totalSectors)
			<< ") against a hard ceiling of " << kUnburnableSectors << ".\n";
		Console::Info("Check the CUE's PREGAP values and the lengths of the "
			"files it names.\n");
		return;
	}

	if (!GuiInput::PromptYesNo("Write this disc?",
		"The layout above was resolved from the CUE sheet.\n\n"
		"Continue to burner selection?")) {
		Console::Info("Write cancelled.\n");
		return;
	}

	// ── 4. Burner selection ─────────────────────────────────────────────
	{
		std::vector<wchar_t> audioDrives;
		std::vector<wchar_t> cdDrives = ScanDrives(audioDrives, /*verbose=*/false);
		if (cdDrives.empty()) {
			Console::Error("No CD/DVD drives detected.\n");
			return;
		}

		wchar_t pick = audioDrive;
		if (cdDrives.size() > 1) {
			pick = SelectWriterDrive(cdDrives, audioDrive);
			if (!pick) { Console::Info("Write cancelled.\n"); return; }
		}
		else {
			pick = cdDrives.front();
		}

		if (pick != audioDrive) {
			copier.Close();
			if (!copier.Open(pick)) {
				Console::Error("Failed to open selected drive.\n");
				// Leave the caller with a usable handle rather than a closed one.
				copier.Open(audioDrive);
				return;
			}
			audioDrive = pick;
			PrintDriveIdentity(audioDrive);
		}

		if (!copier.GetDriveRef().TestUnitReady()) {
			char prompt[160];
			std::snprintf(prompt, sizeof(prompt),
				"Insert a blank CD-R or CD-RW into drive %c: and click OK.",
				static_cast<char>(audioDrive));
			if (!GuiInput::PromptYesNo("Insert disc", prompt)) {
				Console::Info("Write cancelled.\n");
				return;
			}
			if (!copier.GetDriveRef().WaitForDriveReady(30)) {
				Console::Error("Drive did not become ready with a disc.\n");
				return;
			}
		}
	}

	DriveCapabilities caps;
	if (copier.DetectDriveCapabilities(caps)) {
		if (!(caps.writesCDR || caps.writesCDRW)) {
			Console::Error("Drive does not support CD-R/CD-RW writing\n");
			return;
		}
	}

	// ── 5. Write-offset compensation ────────────────────────────────────
	// Available here because it needs no disc read - the burner's offset comes
	// from the AccurateRip database, not from anything in the tray.
	int burnerReadOffset = 0;
	std::string offsetSource;
	const bool offsetKnown =
		LookupDriveReadOffset(audioDrive, burnerReadOffset, offsetSource);
	if (offsetKnown && !offsetSource.empty()) {
		Console::Info("Burner read offset from ");
		std::cout << offsetSource << ": " << burnerReadOffset << " samples\n";
	}
	const int writeOffset = SelectWriteOffset(burnerReadOffset, offsetKnown);
	if (writeOffset == WRITE_OFFSET_BACK) {
		Console::Info("Write cancelled.\n");
		return;
	}

	// ── 6. Materialize the temp image and cue ───────────────────────────
	const std::wstring binPath = workDir + L"\\_writecue_temp.bin";
	const std::wstring cueOutPath = workDir + L"\\_writecue_temp.cue";
	temps.Track(binPath);
	temps.Track(cueOutPath);
	temps.Track(binPath + L".offset");   // ApplyFileSampleOffset's scratch file

	if (!WriteSegmentsToBin(binPath, segments, payloads, totalSectors)) {
		if (Cancelled()) Console::Warning("Cancelled.\n");
		return;
	}
	if (writeOffset != 0 &&
		!ApplyFileSampleOffset(binPath, writeOffset,
			totalSectors * AUDIO_SECTOR_SIZE)) {
		Console::Error("Failed to apply the write-offset compensation.\n");
		return;
	}
	if (!WriteImportedCue(cueOutPath, FileNameOf(binPath), sheet, placements)) {
		Console::Error("Failed to write the temporary CUE sheet.\n");
		return;
	}

	// The decoded WAVs are dead weight now that the image exists, and for a
	// single-FILE FLAC that is ~700 MB sitting idle through the whole burn.
	CleanupTempWavs(payloads);

	// ── 7. Media state and blanking ─────────────────────────────────────
	bool isFull = false, isRewritable = false;
	if (!copier.CheckRewritableDisk(isFull, isRewritable, /*quiet=*/true)) {
		Console::Error("Cannot determine disc type\n");
		return;
	}
	if (isFull && !isRewritable) {
		Console::Error("Disc is full and not rewritable - cannot write\n");
		return;
	}

	int eraseSpeed = -1;
	bool wasBlanked = false;
	if (isRewritable && isFull) {
		Console::Warning("CD-RW disc is full. Erase it first?\n");
		bool ok = false;
		const int choice = GetMenuChoice("Erase full CD-RW before writing?",
			"1. Quick erase (fast, recommended)\n"
			"2. Full erase (thorough, slower)\n"
			"3. Cancel",
			1, 3, 1, &ok);
		if (!ok || choice == 3) { Console::Info("Write cancelled\n"); return; }
		eraseSpeed = copier.SelectWriteSpeed();
		if (!copier.BlankRewritableDisk(eraseSpeed, choice == 1)) return;
		wasBlanked = true;
	}
	else if (isRewritable && !isFull) {
		Console::Info("CD-RW disc detected with available space.\n");
		bool ok = false;
		const int choice = GetMenuChoice("CD-RW with free space - what now?",
			"1. Write directly\n"
			"2. Quick erase first\n"
			"3. Full erase first",
			1, 3, 1, &ok);
		if (!ok) { Console::Info("Write cancelled\n"); return; }
		if (choice == 2 || choice == 3) {
			eraseSpeed = copier.SelectWriteSpeed();
			if (!copier.BlankRewritableDisk(eraseSpeed, choice == 2)) return;
			wasBlanked = true;
		}
	}

	// ── 8. Speed, calibration, Plextor options ──────────────────────────
	int speed;
	if (wasBlanked) {
		Console::Info("Using previously selected write speed (");
		std::cout << eraseSpeed << "x)\n";
		speed = eraseSpeed;
	}
	else {
		speed = copier.SelectWriteSpeed();
	}

	bool calibrationOk = false;
	const int calibChoice = GetMenuChoice("Use power calibration?",
		"Optical Power Calibration tunes laser power to the loaded disc.\n\n"
		"1. Yes (recommended)\n"
		"2. No",
		1, 2, 1, &calibrationOk);
	if (!calibrationOk) { Console::Info("Write cancelled.\n"); return; }
	const bool useCal = (calibChoice == 1);

	bool plxTestWrite = false;
	bool plxVariRecOn = false;
	int  plxVariRecOff = 0;
	if (copier.SelectPlextorWriteOptions(plxTestWrite, plxVariRecOn, plxVariRecOff) == -1)
		return;

	if (plxTestWrite) {
		if (copier.GetDriveRef().SetPlextorTestWrite(true)) {
			Console::Warning("TEST WRITE MODE - laser will stay at read power; "
				"nothing will be burned.\n");
		}
		else {
			Console::Warning("Test write: drive rejected the request - proceeding "
				"with a real burn.\n");
			plxTestWrite = false;
		}
	}
	if (plxVariRecOn) {
		if (copier.GetDriveRef().SetVariRecCD(true, plxVariRecOff)) {
			Console::Info("VariRec applied (offset ");
			std::cout << plxVariRecOff << ")\n";
		}
		else {
			Console::Warning("VariRec: drive rejected the request - using factory "
				"strategy.\n");
			plxVariRecOn = false;
		}
	}

	// ── 9. Burn ─────────────────────────────────────────────────────────
	const bool writeOk =
		copier.WriteDisc(binPath, cueOutPath, L"", speed, useCal, wasBlanked);

	if (plxTestWrite) copier.GetDriveRef().SetPlextorTestWrite(false);
	if (plxVariRecOn) copier.GetDriveRef().SetVariRecCD(false, 0);

	if (writeOk) {
		if (outCompleted) *outCompleted = true;
		Console::Success(plxTestWrite
			? "Test write completed successfully (no data burned)\n"
			: "Disc write completed successfully\n");
	}
	else {
		Console::Error("Disc write failed\n");
	}
}
