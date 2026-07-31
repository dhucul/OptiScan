// ============================================================================
// TrackRipWorkflow.cpp - Rip individual tracks to WAV or FLAC
//
// Workflow: track selection → format (WAV/FLAC) → speed → burst/safe mode →
// verification prompt → read disc → save files → optional physical compare.
//
// FLAC output requires flac.exe on the system PATH.  If not found the track
// is saved as WAV and the user is notified.
// ============================================================================
#define NOMINMAX
#include "TrackRipWorkflow.h"
#include "ConsoleColors.h"
#include "FileUtils.h"
#include "GuiInput.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include "PioneerVendor.h"
#include "Preservation.h"
#include "Progress.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

// ═══════════════════════════════════════════════════════════════════════════
//  WAV / FLAC file writers
// ═══════════════════════════════════════════════════════════════════════════

static bool Utf8ToWide(const std::string& input, std::wstring& output)
{
	int wlen = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
	if (wlen <= 0) {
		output.clear();
		return false;
	}

	std::wstring wide(static_cast<size_t>(wlen), L'\0');
	int converted = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, wide.data(), wlen);
	if (converted <= 0) {
		output.clear();
		return false;
	}

	if (!wide.empty() && wide.back() == L'\0')
		wide.pop_back();

	output = std::move(wide);
	return true;
}

static bool TrackHadReadErrors(const DiscInfo& disc, const TrackInfo& track)
{
	DWORD readStart = (disc.pregapMode == PregapMode::Skip) ? track.startLBA : track.pregapLBA;
	for (DWORD lba : disc.badSectors) {
		if (lba >= readStart && lba <= track.endLBA)
			return true;
	}
	return false;
}

// Writes a standard 44-byte RIFF/WAVE file (16-bit stereo 44100 Hz PCM).
 // Sectors are batched into a write buffer to reduce I/O call overhead.
static bool WriteWavFile(const std::wstring& path,
	const std::vector<std::vector<BYTE>>& sectors,
	size_t startSector, size_t sectorCount)
{
	if (sectorCount == 0) return false;
	if (startSector >= sectors.size()) return false;
	if (sectorCount > (sectors.size() - startSector)) return false;

	for (size_t i = 0; i < sectorCount; i++) {
		if (sectors[startSector + i].size() < AUDIO_SECTOR_SIZE)
			return false;
	}

	unsigned long long dataSize64 =
		static_cast<unsigned long long>(sectorCount) * AUDIO_SECTOR_SIZE;
	if (dataSize64 > 0xFFFFFFFFull - 36ull)
		return false;

	std::ofstream out(path, std::ios::binary);
	if (!out) return false;

	uint32_t dataSize = static_cast<uint32_t>(dataSize64);
	uint32_t fileSize = dataSize + 36;
	uint16_t audioFmt = 1, ch = 2, bps = 16;
	uint32_t rate = 44100;
	uint16_t blockAlign = ch * (bps / 8);
	uint32_t byteRate = rate * blockAlign;
	uint32_t fmtSize = 16;

	out.write("RIFF", 4);
	out.write(reinterpret_cast<const char*>(&fileSize), 4);
	out.write("WAVE", 4);
	out.write("fmt ", 4);
	out.write(reinterpret_cast<const char*>(&fmtSize), 4);
	out.write(reinterpret_cast<const char*>(&audioFmt), 2);
	out.write(reinterpret_cast<const char*>(&ch), 2);
	out.write(reinterpret_cast<const char*>(&rate), 4);
	out.write(reinterpret_cast<const char*>(&byteRate), 4);
	out.write(reinterpret_cast<const char*>(&blockAlign), 2);
	out.write(reinterpret_cast<const char*>(&bps), 2);
	out.write("data", 4);
	out.write(reinterpret_cast<const char*>(&dataSize), 4);

	// Batch writes: ~150 KB per I/O call instead of one per sector
	constexpr size_t WRITE_BATCH = 64;
	std::vector<BYTE> buf(WRITE_BATCH * AUDIO_SECTOR_SIZE);
	size_t buffered = 0;

	for (size_t i = 0; i < sectorCount; i++) {
		size_t idx = startSector + i;
		memcpy(buf.data() + buffered * AUDIO_SECTOR_SIZE,
			sectors[idx].data(), AUDIO_SECTOR_SIZE);
		buffered++;
		if (buffered == WRITE_BATCH) {
			out.write(reinterpret_cast<const char*>(buf.data()),
				buffered * AUDIO_SECTOR_SIZE);
			buffered = 0;
		}
	}
	if (buffered > 0) {
		out.write(reinterpret_cast<const char*>(buf.data()),
			buffered * AUDIO_SECTOR_SIZE);
	}

	return out.good();
}

// Returns true if flac.exe is resolvable on the current PATH.
static bool IsFlacOnPath() {
	wchar_t buf[MAX_PATH];
	DWORD len = SearchPathW(nullptr, L"flac.exe", nullptr, MAX_PATH, buf, nullptr);
	return len > 0 && len < MAX_PATH;
}

// Attempts WAV → FLAC conversion via flac.exe (best compression, silent).
// Returns true if the FLAC file was created successfully.
static bool ConvertWavToFlac(const std::wstring& wavPath, const std::wstring& flacPath) {
	std::wstring cmdLine = L"flac --best --silent --force -o \"" + flacPath + L"\" \"" + wavPath + L"\"";

	STARTUPINFOW si = { sizeof(si) };
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};

	if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		return false;
	}

	WaitForSingleObject(pi.hProcess, INFINITE);

	DWORD exitCode = 1;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	return exitCode == 0;
}

// Writes a track file in the requested format.
// For FLAC: writes a temp WAV, converts via flac.exe, deletes the WAV on success.
// If FLAC encoding is unavailable or fails, keeps the WAV and returns the actual path used.
static bool WriteTrackFile(TrackOutputFormat format,
	const std::wstring& basePath,        // path without extension
	const std::vector<std::vector<BYTE>>& sectors,
	size_t startSector, size_t sectorCount,
	std::wstring& actualPath,            // [out] final file path
	bool& flacFallback)                  // [out] true if fell back to WAV
{
	flacFallback = false;

	if (format == TrackOutputFormat::WAV) {
		actualPath = basePath + L".wav";
		return WriteWavFile(actualPath, sectors, startSector, sectorCount);
	}

	// FLAC: write temp WAV → convert → delete WAV
	std::wstring wavPath = basePath + L".wav";
	std::wstring flacPath = basePath + L".flac";

	if (!WriteWavFile(wavPath, sectors, startSector, sectorCount))
		return false;

	if (ConvertWavToFlac(wavPath, flacPath)) {
		DeleteFileW(wavPath.c_str());
		actualPath = flacPath;
		return true;
	}

	// flac.exe not found or failed — keep the WAV
	flacFallback = true;
	actualPath = wavPath;
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Interactive menus
// ═══════════════════════════════════════════════════════════════════════════

// Track selection — returns indices into disc.tracks (audio only).
static std::vector<int> SelectTracks(const DiscInfo& disc) {
	std::vector<int> audioIdx;
	for (int i = 0; i < static_cast<int>(disc.tracks.size()); i++) {
		if (disc.tracks[i].isAudio)
			audioIdx.push_back(i);
	}
	if (audioIdx.empty()) return {};

	std::cout << "\n=== Track Selection ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. All audio tracks (" << audioIdx.size() << " tracks)\n";
	std::cout << "2. Select individual tracks\n";
	std::cout << "Choice: ";

	std::string trackSelMsg =
		"0. Back to menu\n"
		"1. All audio tracks (" + std::to_string(audioIdx.size()) + " tracks)\n"
		"2. Select individual tracks";
	int c = GetMenuChoice("Track Selection", trackSelMsg.c_str(), 0, 2, 1);

	if (c == 0) return {};
	if (c == 1) return audioIdx;

	// List tracks
	std::cout << "\nAudio tracks:\n";
	for (int idx : audioIdx) {
		const auto& t = disc.tracks[idx];
		DWORD sectors = t.endLBA - t.startLBA + 1;
		int secs = static_cast<int>(sectors / 75);
		std::cout << "  " << std::setw(2) << t.trackNumber << ". "
			<< std::setw(2) << secs / 60 << ":"
			<< std::setfill('0') << std::setw(2) << secs % 60
			<< std::setfill(' ');
		if (t.trackNumber > 0 &&
			static_cast<size_t>(t.trackNumber) <= disc.cdText.trackTitles.size() &&
			!disc.cdText.trackTitles[t.trackNumber - 1].empty()) {
			std::cout << "  " << disc.cdText.trackTitles[t.trackNumber - 1];
		}
		std::cout << "\n";
	}

	std::string line = GuiInput::PromptString("Track selection",
		"Enter track numbers separated by spaces (e.g. 1 3 5), or 0 to cancel:",
		std::string());
	if (line.empty() || line == "0") return {};

	std::istringstream iss(line);
	std::vector<int> selected;
	int num;
	while (iss >> num) {
		for (int idx : audioIdx) {
			if (disc.tracks[idx].trackNumber == num) {
				if (std::find(selected.begin(), selected.end(), idx) == selected.end())
					selected.push_back(idx);
				break;
			}
		}
	}

	if (selected.empty()) {
		Console::Warning("No valid tracks selected.\n");
	}
	else {
		std::cout << "Selected " << selected.size() << " track(s): ";
		for (size_t i = 0; i < selected.size(); i++) {
			if (i > 0) std::cout << ", ";
			std::cout << disc.tracks[selected[i]].trackNumber;
		}
		std::cout << "\n";
	}
	return selected;
}

static int SelectOutputFormat() {
	std::cout << "\n=== Output Format ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. WAV (uncompressed, maximum compatibility)\n";
	std::cout << "2. FLAC (lossless compressed - requires flac.exe on PATH)\n";
	std::cout << "Choice: ";
	int c = GetMenuChoice("Output Format",
		"0. Back to menu\n"
		"1. WAV (uncompressed, maximum compatibility)\n"
		"2. FLAC (lossless compressed - requires flac.exe on PATH)",
		0, 2, 1);
	if (c == 0) return -1;
	std::cout << (c == 1 ? "Output format: WAV\n" : "Output format: FLAC\n");
	return c - 1;  // 0 = WAV, 1 = FLAC
}

static int SelectRipMode(int selectedSpeed) {
	std::string speedLabel = (selectedSpeed == 0)
		? "maximum speed" : (std::to_string(selectedSpeed) + "x");
	std::cout << "\n=== Rip Mode ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Safe (re-reads on error, C2-guided, recommended)\n";
	std::cout << "2. Burst (" << speedLabel << ", no secure re-reads, fastest)\n";
	std::cout << "Choice: ";
	std::string ripModeMsg =
		"0. Back to menu\n"
		"1. Safe (re-reads on error, C2-guided, recommended)\n"
		"2. Burst (" + speedLabel + ", no secure re-reads, fastest)";
	int c = GetMenuChoice("Rip Mode", ripModeMsg.c_str(), 0, 2, 1);
	if (c == 0) return -1;
	std::cout << (c == 1 ? "Safe mode selected\n" : "Burst mode selected\n");
	return c;  // 1 = safe, 2 = burst
}

static int SelectVerifyMode() {
	std::cout << "\n=== Verification ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. None (rip only)\n";
	std::cout << "2. Verify ripped track(s) against physical disk (Test & Copy)\n";
	std::cout << "3. Verify with auto-retry on mismatch (up to 3 retries)\n";
	std::cout << "Choice: ";
	int c = GetMenuChoice("Verification",
		"0. Back to menu\n"
		"1. None (rip only)\n"
		"2. Verify ripped track(s) against physical disk (Test & Copy)\n"
		"3. Verify with auto-retry on mismatch (up to 3 retries)",
		0, 3, 1);
	if (c == 0) return -1;
	if (c == 1) std::cout << "No verification selected\n";
	else if (c == 2) std::cout << "Verify against disk selected\n";
	else std::cout << "Verify with auto-retry selected\n";
	return c;  // 1 = skip, 2 = verify once, 3 = verify with retry
}

// Pioneer-only: prompt for the PureRead mode, apply the audio-extraction preset,
// then CONFIRM PureRead actually engaged by reading the mode back from the drive.
// ApplyAudioExtractionPreset reports success if ANY preset setting took (Quiet or
// Fragile can succeed while the PureRead write silently fails), so the "applied"
// message alone is not proof — verify PureRead specifically against the drive.
// Caller has already probed IsPioneerDrive and read pc.
static void ApplyAndConfirmPioneerPreset(PioneerVendor& pioneer, const PioneerCapabilities& pc) {
	// Let the user pick how the drive should treat audio it cannot read cleanly
	// after exhausting retries. Only meaningful when PureRead is supported.
	//   Master  - interpolate the lost samples (gap-free, near-perfect guess)
	//   Perfect - report a read error instead of interpolating (nothing faked)
	PureReadMode chosenMode = PureReadMode::Master;
	if (pc.pureReadSupport) {
		std::cout << "\n=== Pioneer PureRead Mode ===\n";
		std::cout << "Controls how the drive handles audio it cannot read cleanly\n";
		std::cout << "after exhausting retries.\n\n";
		std::cout << "1. Master (recommended) - interpolate unreadable samples so the\n";
		std::cout << "   rip stays gap-free; a near-perfect estimate fills the gap.\n";
		std::cout << "2. Perfect (strictest)   - report a read error instead of\n";
		std::cout << "   interpolating; unrecoverable samples are never faked.\n";
		std::cout << "Choice: ";
		int c = GetMenuChoice("Pioneer PureRead Mode",
			"Controls how the drive handles audio it cannot read cleanly\n"
			"after exhausting retries.\n\n"
			"1. Master (recommended) - interpolate unreadable samples so the\n"
			"   rip stays gap-free; a near-perfect estimate fills the gap.\n"
			"2. Perfect (strictest)   - report a read error instead of\n"
			"   interpolating; unrecoverable samples are never faked.",
			1, 2, 1);
		chosenMode = (c == 2) ? PureReadMode::Perfect : PureReadMode::Master;
	}

	const char* modeName = (chosenMode == PureReadMode::Perfect) ? "Perfect" : "Master";

	if (pioneer.ApplyAudioExtractionPreset(/*persist=*/false, chosenMode)) {
		std::string applied = " Pioneer audio preset applied (PureRead ";
		applied += modeName;
		applied += " + Quiet + Fragile CD).\n";
		Console::Info(applied.c_str());
	}

	if (!pc.pureReadSupport) {
		Console::Warning(" This Pioneer drive does not support PureRead - extracting without it.\n");
		return;
	}

	PureReadMode mode = PureReadMode::Off;
	bool realTime = false;
	if (pioneer.GetPureReadMode(mode, realTime) && mode != PureReadMode::Off) {
		std::string msg = " PureRead confirmed engaged (mode=";
		msg += (mode == PureReadMode::Perfect) ? "Perfect" : "Master";
		if (realTime) msg += ", Real-Time";
		msg += ").\n";
		Console::Success(msg.c_str());
	}
	else {
		Console::Warning(" PureRead did NOT engage - the drive reports it is Off. "
			"Proceeding without PureRead error recovery.\n");
	}
}

// ═══════════════════════════════════════════════════════════════════════════
//  Main workflow
// ═══════════════════════════════════════════════════════════════════════════

bool RunTrackRipWorkflow(OpticalDrive& copier, DiscInfo& disc, const std::wstring& /*workDir*/) {
	Console::Info("\n(Enter 0 at any prompt to go back to menu)\n");

	{
		std::vector<std::string> preflightWarnings;
		if (!copier.RunPreflightChecks(disc, preflightWarnings)) {
			std::string msg = "Disc preflight reported issues:\n";
			for (const auto& w : preflightWarnings) {
				msg += "  - " + w + "\n";
			}
			msg += "\nContinue anyway?";
			if (!GuiInput::PromptYesNo("Disc preflight", msg.c_str())) {
				Console::Info("Rip cancelled.\n");
				return false;
			}
		}
	}

	// ── 1. Track selection ──────────────────────────────────────────────
	std::vector<int> selectedTracks = SelectTracks(disc);
	if (selectedTracks.empty()) return false;

	// Keep tracks in disc order so the drive reads sequentially (no seeks)
	std::sort(selectedTracks.begin(), selectedTracks.end());

	// ── 2. Output format ────────────────────────────────────────────────
	int fmtChoice = SelectOutputFormat();
	if (fmtChoice == -1) return false;
	TrackOutputFormat format = static_cast<TrackOutputFormat>(fmtChoice);

	bool flacUnavailable = (format == TrackOutputFormat::FLAC && !IsFlacOnPath());
	if (flacUnavailable) {
		Console::Warning("flac.exe was not found on your PATH - tracks will be saved as WAV.\n");
		Console::Info("Install FLAC command-line tools (https://xiph.org/flac/) and ensure\n");
		Console::Info("flac.exe is on your system PATH to enable FLAC output.\n");
	}

	// ── 3. Speed ────────────────────────────────────────────────────────
	int speed = copier.SelectSpeed();
	if (speed == -1) return false;

	// ── 4. Burst / Safe mode ────────────────────────────────────────────
	int ripMode = SelectRipMode(speed);
	if (ripMode == -1) return false;
	bool isBurst = (ripMode == 2);

	// ── 4b. Error handling (abort vs skip on unrecoverable sectors) ──────
	int errorMode = copier.SelectErrorHandling();
	if (errorMode == -1) return false;

	// ── 5. Verification Mode ────────────────────────────────────────────
	int verifyMode = SelectVerifyMode();
	if (verifyMode == -1) return false;
	bool verifyRip = (verifyMode == 2 || verifyMode == 3);
	bool autoRetry = (verifyMode == 3);

	// ── 6. Drive capabilities ───────────────────────────────────────────
	SecureRipConfig secureConfig{};
	DriveCapabilities driveCaps;
	bool haveDriveCaps = false;
	if (!isBurst) {
		std::cout << "\nDetecting drive capabilities..." << std::flush;
		if (copier.DetectDriveCapabilities(driveCaps)) {
			haveDriveCaps = true;
			disc.enableC2Detection = driveCaps.supportsC2ErrorReporting;
			secureConfig = copier.GetSecureRipConfig(SecureRipMode::Standard);
			secureConfig.useC2 = driveCaps.supportsC2ErrorReporting;
			secureConfig.c2Guided = driveCaps.supportsC2ErrorReporting;

			// Pioneer-only: prompt for PureRead mode (Master/Perfect) + Quiet + Fragile
			// CD mode for this session.
			{
				PioneerVendor pv(copier.GetDriveRef());
				if (pv.IsPioneerDrive()) {
					PioneerCapabilities pc;
					if (pv.ReadCapabilities(pc) && pc.valid) {
						ApplyAndConfirmPioneerPreset(pv, pc);
					}
					// Pioneer C2-enabled READ CD shifts audio off the AccurateRip-
					// calibrated offset. Disable C2 here so safe rips match AccurateRip.
					if (disc.enableC2Detection) {
						disc.enableC2Detection = false;
						secureConfig.useC2 = false;
						secureConfig.c2Guided = false;
						Console::Info(" Pioneer drive - disabling C2 for AccurateRip-compatible reads.\n");
					}
				}
			}
			if (driveCaps.supportsAccurateStream) {
				secureConfig.cacheDefeat = false;
				Console::Info(" Accurate Stream detected.\n");
			}
			else {
				secureConfig.cacheDefeat = true;
				std::cout << " done.\n";
			}
		}
		else {
			disc.enableC2Detection = false;
			std::cout << " skipped.\n";
		}
	}
	else {
		disc.enableC2Detection = false;

		// Pioneer-only: prompt for PureRead mode (Master/Perfect) + Quiet + Fragile CD
		// mode for this session. Matches the Copy workflow (option 1), which detects
		// capabilities and applies the preset for both burst and safe rips.
		std::cout << "\nDetecting drive capabilities..." << std::flush;
		if (copier.DetectDriveCapabilities(driveCaps)) {
			haveDriveCaps = true;
			std::cout << " done.\n";
			PioneerVendor pv(copier.GetDriveRef());
			if (pv.IsPioneerDrive()) {
				PioneerCapabilities pc;
				if (pv.ReadCapabilities(pc) && pc.valid) {
					ApplyAndConfirmPioneerPreset(pv, pc);
				}
			}
		}
		else {
			std::cout << " skipped.\n";
		}
	}

	// ── 7. Offset correction ────────────────────────────────────────────
	int offset = copier.SelectOffset();
	if (offset == -1) return false;
	disc.driveOffset = offset;

	// ── 8. Output directory ─────────────────────────────────────────────
	Console::Info("\nChoose the output directory for the ripped tracks...\n");
	std::wstring outputDir = GuiInput::PromptForFolder(L"Choose output directory for ripped tracks");
	if (outputDir.empty()) {
		Console::Info("Rip cancelled (no output directory selected).\n");
		return false;
	}
	outputDir = NormalizePath(outputDir);
	if (!outputDir.empty() && outputDir.back() != L'\\' && outputDir.back() != L'/') {
		outputDir += L"\\";
	}
	if (!CreateDirectoryRecursive(outputDir)) {
		Console::Error("Cannot create directory: ");
		std::wcout << outputDir << L"\n";
		return false;
	}
	Console::Success("Output directory: ");
	std::wcout << outputDir << L"\n";

	{
		DWORD estSectors = 0;
		for (int idx : selectedTracks) {
			const auto& t = disc.tracks[idx];
			if (t.endLBA >= t.startLBA) estSectors += (t.endLBA - t.startLBA + 1);
		}
		if (estSectors > 0 && !copier.CheckDiskSpace(outputDir, estSectors)) {
			char buf[160];
			std::snprintf(buf, sizeof(buf),
				"Insufficient free space at output path (need ~%llu MB). Continue anyway?",
				static_cast<unsigned long long>(estSectors) * AUDIO_SECTOR_SIZE / (1024 * 1024));
			if (!GuiInput::PromptYesNo("Low disk space", buf)) {
				Console::Info("Rip cancelled.\n");
				return false;
			}
		}
	}

	// ── 9. Configure disc state & build trimmed copy ────────────────────
	disc.pregapMode = PregapMode::Skip;
	disc.includeSubchannel = false;
	disc.enableCacheDefeat = !isBurst && secureConfig.cacheDefeat;

	if (disc.sessionCount > 1) {
		Console::Info("Multi-session disc - using session 1 (audio).\n");
	}

	// When offset correction is active and the user picked non-contiguous
	// tracks (e.g. 1, 5, 10), ApplyOffsetCorrection would shift samples
	// across the artificial gaps.  Fill those gaps so the sector stream
	// reaching the offset corrector is truly contiguous on disc.
	int firstIdx = selectedTracks.front();
	int lastIdx = selectedTracks.back();
	bool isContiguous = (lastIdx - firstIdx + 1 == static_cast<int>(selectedTracks.size()));
	bool needGapFill = (offset != 0 && !isContiguous);

	DiscInfo ripDisc = disc;
	ripDisc.rawSectors.clear();
	ripDisc.tracks.clear();

	// ripIndices[i] = index into ripDisc.tracks for selectedTracks[i]
	std::vector<int> ripIndices(selectedTracks.size());

	if (needGapFill) {
		// Include every track from first to last selected
		for (int i = firstIdx; i <= lastIdx; i++) {
			ripDisc.tracks.push_back(disc.tracks[i]);
		}
		for (size_t i = 0; i < selectedTracks.size(); i++) {
			ripIndices[i] = selectedTracks[i] - firstIdx;
		}
	}
	else {
		for (int idx : selectedTracks) {
			ripDisc.tracks.push_back(disc.tracks[idx]);
		}
		for (size_t i = 0; i < selectedTracks.size(); i++) {
			ripIndices[i] = static_cast<int>(i);
		}
	}
	ripDisc.selectedSession = 0;   // not needed — we already picked the tracks

	PioneerPureReadSession pureReadSession(copier.GetDriveRef());
	const bool pureReadMonitoring = pureReadSession.Begin();
	bool pureReadFinalized = false;
	bool pureReadLogSaved = false;
	auto finishPureReadMonitoring = [&](const char* readOutcome) {
		if (!pureReadMonitoring || pureReadFinalized) return;
		pureReadFinalized = true;

		PioneerPureReadSummary summary;
		if (!pureReadSession.Finish(summary)) {
			Console::Warning("Pioneer Real-Time PureRead counters could not be read after extraction.\n");
			return;
		}

		PrintPioneerPureReadSummary(summary);
		std::wstring reportPath = outputDir + L"PioneerPureRead.log";
		if (SavePioneerPureReadSummary(reportPath, summary, "Selected-track rip", readOutcome)) {
			pureReadLogSaved = true;
			Console::Success("PureRead diagnostic log saved to: ");
			std::wcout << reportPath << L"\n";
		}
		else {
			Console::Warning("Could not save the PureRead diagnostic log.\n");
		}
	};
	if (pureReadMonitoring) {
		Console::Info(" Pioneer Real-Time PureRead monitoring started for this read session.\n");
	}

	// ── 10. Read only the selected tracks ───────────────────────────────
	Console::Info("\nReading disc...\n");
	ProgressIndicator prog;
	prog.SetLabel("  Ripping");
	prog.Start();

	bool readOk = false;
	SecureRipResult secureResult;

	if (isBurst) {
		readOk = copier.ReadDiscBurst(ripDisc, MakeProgressCallback(&prog), speed, errorMode);
	}
	else {
		readOk = copier.ReadDiscSecure(ripDisc, secureConfig, secureResult,
			MakeProgressCallback(&prog));
	}

	if (!readOk) {
		prog.Finish(false);
		Console::Error("Disc read failed.\n");
		finishPureReadMonitoring("Initial read failed");
		return false;
	}
	prog.Finish(true);

	if (isBurst && ripDisc.errorCount > 0) {
		std::string msg = std::to_string(ripDisc.errorCount) +
			" sector read error(s) occurred in burst mode; affected output may contain unreadable or zero-filled audio.\n";
		Console::Warning(msg.c_str());
		if (!verifyRip) {
			Console::Info("Consider Safe mode or enabling physical compare for a second-pass check.\n");
		}
	}

	if (!isBurst && secureResult.unsecureSectors > 0) {
		std::string msg = std::to_string(secureResult.unsecureSectors) +
			" sector(s) could not be fully verified.\n";
		Console::Warning(msg.c_str());
	}

	// Error-handling policy: "Abort on error" refuses to save tracks that still
	// contain unrecoverable sectors. Burst aborts mid-read (handled in the burst
	// engine, so readOk is already false above); the safe path completes its
	// multi-pass recovery first, so the decision is made here on whatever stayed
	// unrecoverable. Skip modes (2/3) fall through and save with bad sectors
	// zero-filled. errorCount counts only hard-unrecoverable sectors.
	if (errorMode == 1 && ripDisc.errorCount > 0) {
		std::string msg = "\nAborting: " + std::to_string(ripDisc.errorCount) +
			" sector(s) could not be read and error handling is set to \"Abort on error\".\n";
		Console::Error(msg.c_str());
		Console::Info("No tracks were written. Re-run with a skip mode to save a best-effort (zero-filled) copy.\n");
		ripDisc.rawSectors.clear();
		ripDisc.rawSectors.shrink_to_fit();
		finishPureReadMonitoring("Read completed; output aborted by error policy");
		return false;
	}

	// Apply offset correction before splitting tracks
	if (ripDisc.driveOffset != 0) {
		copier.ApplyOffsetCorrection(ripDisc);
	}

	// ── 11. Build per-track sector map ──────────────────────────────────
	// ripDisc.tracks may include gap-fill tracks; slices covers all of them.
	struct TrackSlice { size_t start; size_t count; };
	std::vector<TrackSlice> slices(ripDisc.tracks.size());
	size_t cumIdx = 0;
	for (size_t i = 0; i < ripDisc.tracks.size(); i++) {
		DWORD readStart = (ripDisc.pregapMode == PregapMode::Skip)
			? ripDisc.tracks[i].startLBA : ripDisc.tracks[i].pregapLBA;
		DWORD cnt = ripDisc.tracks[i].endLBA - readStart + 1;
		slices[i] = { cumIdx, cnt };
		cumIdx += cnt;
	}

	// ── 12. Save each selected track ────────────────────────────────────
	Console::Info("\nSaving tracks...\n");
	int savedCount = 0;
	bool anyFlacFallback = false;
	std::vector<std::wstring> preservationArtifacts;

	for (size_t si = 0; si < selectedTracks.size(); si++) {
		int ri = ripIndices[si];
		const auto& t = ripDisc.tracks[ri];
		const TrackSlice& sl = slices[ri];

		// Build filename: "02. Artist - Title" or "Track 02"
		std::wostringstream prefix;
		prefix << std::setfill(L'0') << std::setw(2) << t.trackNumber << L". ";

		std::wstring baseName;
		bool hasCDText = (t.trackNumber > 0 &&
			static_cast<size_t>(t.trackNumber) <= disc.cdText.trackTitles.size() &&
			!disc.cdText.trackTitles[t.trackNumber - 1].empty());

		if (hasCDText) {
			std::string title = disc.cdText.trackTitles[t.trackNumber - 1];
			std::string artist;
			if (t.trackNumber > 0 &&
				static_cast<size_t>(t.trackNumber) <= disc.cdText.trackArtists.size() &&
				!disc.cdText.trackArtists[t.trackNumber - 1].empty()) {
				artist = disc.cdText.trackArtists[t.trackNumber - 1];
			}
			std::string narrow = artist.empty() ? title : (artist + " - " + title);

			std::wstring wide;
			if (Utf8ToWide(narrow, wide)) {
				std::wstring sanitized = SanitizeFilename(wide);
				if (!sanitized.empty()) {
					baseName = prefix.str() + sanitized;
				}
			}
		}

		if (baseName.empty()) {
			baseName = L"Track " + prefix.str().substr(0, 2);
		}

		std::wstring basePath = outputDir + baseName;
		std::wstring actualPath;
		bool flacFallback = false;

		bool ok = WriteTrackFile(format, basePath, ripDisc.rawSectors,
			sl.start, sl.count, actualPath, flacFallback);

		if (flacFallback) anyFlacFallback = true;

		if (ok) {
			preservationArtifacts.push_back(actualPath);
			Console::Success("  Saved: ");
			std::wcout << baseName;
			if (flacFallback)
				std::cout << " (WAV - FLAC encoding unavailable)";
			std::cout << "\n";
			savedCount++;
		}
		else {
			Console::Error("  Failed: ");
			std::wcout << baseName << L"\n";
		}
	}

	if (anyFlacFallback && !flacUnavailable) {
		Console::Warning("\nFLAC encoding unavailable or failed - affected tracks were saved as WAV.\n");
		Console::Info("Install FLAC command-line tools (https://xiph.org/flac/) and ensure\n");
		Console::Info("flac.exe is on your system PATH. If it is already installed, verify it launches correctly.\n");
	}

	std::string summary = "\n" + std::to_string(savedCount) + "/" +
		std::to_string(selectedTracks.size()) + " track(s) saved to: ";
	
	if (savedCount == 0) {
		Console::Error(summary.c_str());
	}
	else if (savedCount < static_cast<int>(selectedTracks.size())) {
		Console::Warning(summary.c_str());
	}
	else {
		Console::Success(summary.c_str());
	}
	std::wcout << outputDir << L"\n";

	if (savedCount != static_cast<int>(selectedTracks.size())) {
		ripDisc.rawSectors.clear();
		ripDisc.rawSectors.shrink_to_fit();
		std::cout << "\n";
		finishPureReadMonitoring("Read completed; one or more track files failed");
		return false;
	}

	// ── 13. Verify ripped tracks against physical disk ──────────────────
	bool physicalVerificationPassed = true;
	if (verifyRip) {
		// Per-track verification state. `verifiedAtAttempt` is 0 for verified-on-first-pass,
		// N>0 for verified-after-N-retries, and -1 for unresolved.
		std::vector<int> verifiedAtAttempt(selectedTracks.size(), -1);
		// Tracks with read errors during the rip itself are excluded from retry —
		// they cannot match because the rip data is already known-bad.
		std::vector<bool> hasRipReadErrors(selectedTracks.size(), false);
		for (size_t si = 0; si < selectedTracks.size(); si++) {
			int ri = ripIndices[si];
			if (TrackHadReadErrors(ripDisc, ripDisc.tracks[ri])) {
				hasRipReadErrors[si] = true;
			}
		}

		const int maxAttempts = autoRetry ? 4 : 1;  // 1 verify + up to 3 retries
		int attemptsRun = 0;

		for (int attempt = 0; attempt < maxAttempts; attempt++) {
			// Honour user cancellation cleanly instead of spinning through the
			// remaining attempts after a ReadDisc* call returned because of ESC.
			if (InterruptHandler::Instance().IsInterrupted() ||
				InterruptHandler::Instance().CheckEscapeKey()) {
				Console::Warning("\n*** Verification cancelled by user ***\n");
				break;
			}

			// Stop early if every track is either verified or known-bad from the rip.
			bool allDone = true;
			for (size_t si = 0; si < selectedTracks.size(); si++) {
				if (verifiedAtAttempt[si] < 0 && !hasRipReadErrors[si]) {
					allDone = false;
					break;
				}
			}
			if (allDone) break;

			if (attempt == 0) {
				Console::Info("\nVerifying against physical disk (second pass)...\n");
			}
			else {
				std::string msg = "\nRetry " + std::to_string(attempt) + " of " +
					std::to_string(maxAttempts - 1) + " - re-reading the disc...\n";
				Console::Info(msg.c_str());
			}

			ProgressIndicator verProg;
			verProg.SetLabel(attempt == 0 ? "  Verifying" : "  Retrying ");
			verProg.Start();

			DiscInfo verifyDisc = ripDisc;
			verifyDisc.rawSectors.clear();
			// Force cache defeat to ensure the drive actually re-reads the physical disc surface
			verifyDisc.enableCacheDefeat = true;

			bool verifyReadOk = false;
			SecureRipResult vResult;

			if (isBurst) {
				verifyReadOk = copier.ReadDiscBurst(verifyDisc, MakeProgressCallback(&verProg), speed);
			}
			else {
				verifyReadOk = copier.ReadDiscSecure(verifyDisc, secureConfig, vResult,
					MakeProgressCallback(&verProg));
			}

			if (!verifyReadOk) {
				verProg.Finish(false);
				Console::Error("Verification read failed.\n");
				verifyDisc.rawSectors.clear();
				if (!autoRetry) break;
				continue;  // try again on next retry attempt
			}
			verProg.Finish(true);
			attemptsRun++;  // only count attempts that produced data

			if (isBurst && verifyDisc.errorCount > 0) {
				std::string msg = std::to_string(verifyDisc.errorCount) +
					" sector read error(s) occurred during pass " +
					std::to_string(attempt + 1) + ".\n";
				Console::Warning(msg.c_str());
			}

			if (verifyDisc.driveOffset != 0) {
				copier.ApplyOffsetCorrection(verifyDisc);
			}

			// Compare only tracks not yet verified.
			std::cout << "\n";
			for (size_t si = 0; si < selectedTracks.size(); si++) {
				if (verifiedAtAttempt[si] >= 0) continue;  // already done
				int ri = ripIndices[si];
				const auto& t = verifyDisc.tracks[ri];
				const TrackSlice& sl = slices[ri];

				std::cout << "  Track " << std::setw(2) << t.trackNumber << ": ";

				if (hasRipReadErrors[si] ||
					TrackHadReadErrors(verifyDisc, verifyDisc.tracks[ri])) {
					Console::Warning("[READ ERRORS]\n");
					continue;
				}

				bool trackMatch = true;
				for (size_t i = 0; i < sl.count; i++) {
					size_t idx = sl.start + i;
					if (idx >= ripDisc.rawSectors.size() || idx >= verifyDisc.rawSectors.size()) {
						trackMatch = false;
						break;
					}
					if (memcmp(ripDisc.rawSectors[idx].data(),
						verifyDisc.rawSectors[idx].data(), AUDIO_SECTOR_SIZE) != 0) {
						trackMatch = false;
						break;
					}
				}

				if (trackMatch) {
					verifiedAtAttempt[si] = attempt;
					if (attempt == 0) Console::Success("[MATCH]\n");
					else {
						std::string msg = "[MATCH after retry " + std::to_string(attempt) + "]\n";
						Console::Success(msg.c_str());
					}
				}
				else {
					if (autoRetry && attempt < maxAttempts - 1)
						Console::Warning("[MISMATCH - will retry]\n");
					else
						Console::Error("[MISMATCH]\n");
				}
			}

			verifyDisc.rawSectors.clear();
		}

		// ── Final summary ─────────────────────────────────────────────
		int verifiedCount = 0;
		int unresolvedCount = 0;
		for (size_t si = 0; si < selectedTracks.size(); si++) {
			if (verifiedAtAttempt[si] >= 0) verifiedCount++;
			else unresolvedCount++;
		}

		if (unresolvedCount == 0) {
			Console::Success("\nAll ripped tracks verified successfully against the disk.\n");
			if (autoRetry && attemptsRun > 1) {
				std::string msg = "Verification used " + std::to_string(attemptsRun) +
					" pass(es) total.\n";
				Console::Info(msg.c_str());
			}
		}
		else {
			physicalVerificationPassed = false;
			std::string msg = "\n" + std::to_string(verifiedCount) + "/" +
				std::to_string(selectedTracks.size()) + " tracks verified, " +
				std::to_string(unresolvedCount) + " did NOT match";
			if (autoRetry) msg += " after " + std::to_string(attemptsRun) + " pass(es)";
			msg += ".\n";
			Console::Warning(msg.c_str());
			Console::Info("Possible causes: damaged disc, drive cache issues, or marginal sectors.\n");
			if (!autoRetry)
				Console::Info("Try Verify Mode 3 (auto-retry) or Secure Paranoid rip mode.\n");
			else
				Console::Info("Consider Secure Paranoid rip mode for the next attempt.\n");
		}
	}

	finishPureReadMonitoring(!verifyRip
		? "Read completed"
		: (physicalVerificationPassed
			? "Read and physical verification completed"
			: "Read completed; physical verification failed"));

	PreservationOffsetResult preservationOffset =
		AnalyzePreservationWriteOffset(ripDisc);
	std::wstring pureReadPath = outputDir + L"PioneerPureRead.log";
	if (pureReadLogSaved)
		preservationArtifacts.push_back(pureReadPath);
	PreservationManifestContext manifest;
	manifest.workflow = "Selected-track rip";
	manifest.artifacts = preservationArtifacts;
	manifest.drive = haveDriveCaps ? &driveCaps : nullptr;
	manifest.writeOffset = &preservationOffset;
	std::wstring manifestPath = outputDir + L"OptiScan_tracks.manifest.json";
	// Keep the original full-disc TOC/IDs in a selected-track manifest; the
	// artifact list itself records which tracks were actually written.
	if (!WritePreservationManifest(disc, manifest, manifestPath)) {
		Console::Error("Could not create the track preservation manifest.\n");
		ripDisc.rawSectors.clear();
		ripDisc.rawSectors.shrink_to_fit();
		return false;
	}
	else {
		Console::Success("Preservation manifest saved to: ");
		std::wcout << manifestPath << L"\n";
	}

	// ── 14. Cleanup ─────────────────────────────────────────────────────
	ripDisc.rawSectors.clear();
	ripDisc.rawSectors.shrink_to_fit();

	if (!physicalVerificationPassed) {
		Console::Warning("\nRip files were saved, but physical verification did not complete successfully.\n");
		return false;
	}

	Console::Success("\nRip complete. Files written to: ");
	std::wcout << outputDir << L"\n";
	return true;
}
