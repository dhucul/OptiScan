// OptiScanWorkflowHost.cpp - drive/session state and GUI workflow helpers.

#include "framework.h"
#include "OptiScanWorkflowHost.h"

#include "AccurateRip.h"
#include "ConsoleFormat.h"
#include "Drive.h"
#include "DriveSelection.h"
#include "FileUtils.h"
#include "InterruptHandler.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

// Disc / drive state retained across button clicks.
OpticalDrive g_copier;
DiscInfo      g_disc;
std::wstring  g_workDir;
wchar_t       g_audioDrive = 0;
bool          g_hasTOC = false;
bool          g_driveOpen = false;
static HWND ModalOwnerForCurrentThread(HWND hOwner) {
    if (!hOwner || !IsWindow(hOwner)) return nullptr;
    return hOwner;
}

// Lazily open the first audio CD drive on first button press that needs one.
// Returns true if the drive is open (TOC may or may not be available). When
// `outFreshlyScanned` is provided, it is set to true if the call performed
// the initial TOC read (so callers that always re-scan for asterisked ops
// can skip the duplicate read on the first click). Pass readToc=false to skip
// the open-time TOC/pre-gap scan entirely — used by operations like Erase
// CD-RW that don't need disc contents (and where scanning pre-gaps on a disc
// that's about to be wiped is pointless and slow).
bool EnsureDriveOpen(HWND hOwner, bool* outFreshlyScanned,
                     bool needAudioDisc, bool readToc) {
    if (outFreshlyScanned) *outFreshlyScanned = false;
    if (g_driveOpen) return true;

    std::vector<wchar_t> audioDrives;
    // Quiet scan — per-workflow drive pickers will show the list when needed.
    // For the auto-pick branches below, we print a single concise line below.
    std::vector<wchar_t> cdDrives = ScanDrives(audioDrives, /*verbose=*/false);
    if (cdDrives.empty()) {
        MessageBoxW(ModalOwnerForCurrentThread(hOwner), L"No CD/DVD drives were found on this system.",
                    L"OptiScan", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return false;
    }

    wchar_t drive = 0;
    if (audioDrives.size() == 1) {
        drive = audioDrives[0];
    } else if (audioDrives.size() > 1) {
        drive = SelectAudioDrive(audioDrives);
    } else if (needAudioDisc) {
        drive = WaitForDisc(cdDrives, 30);
    } else {
        // Workflow doesn't need an audio source (e.g. Write Disc burns from
        // files on disk). Open any CD drive so the rest of the program can
        // run; the workflow may pick a different one via its own selector.
        drive = cdDrives.front();
    }
    if (!drive) {
        MessageBoxW(ModalOwnerForCurrentThread(hOwner), L"No audio disc was selected.",
                    L"OptiScan", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return false;
    }

    if (!g_copier.Open(drive)) {
        MessageBoxW(ModalOwnerForCurrentThread(hOwner), L"Failed to open the selected drive.",
                    L"OptiScan", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return false;
    }

    g_audioDrive = drive;
    g_driveOpen = true;
    g_workDir = GetWorkingDirectory();

    // Single-line confirmation so the user knows which drive is in use without
    // a full per-drive scan dump.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Using drive %c:\n", static_cast<char>(drive));
    Console::Info(buf);

    // Read TOC; failure is non-fatal (some operations don't need it). Skipped
    // entirely when readToc is false (e.g. Erase CD-RW), so the drive opens
    // without spending time on a TOC + pre-gap scan of a disc about to be wiped.
    if (readToc) {
        g_disc = DiscInfo{};
        g_hasTOC = g_copier.ReadTOC(g_disc);

        // Query AccurateRip at disc open so the disc's recognition status is shown
        // up front, without requiring a manual "Rescan disc". Mirrors the original
        // console workflow. Network-gated and timeout-bounded inside Lookup(); the
        // returned pressing CRCs aren't retained here — this is purely the
        // FOUND/NOT FOUND + disc-ID readout.
        if (g_hasTOC) {
            std::vector<std::vector<uint32_t>> pressingCRCs;
            AccurateRip::Lookup(g_disc, pressingCRCs);
            // Capture the disc's metadata at open so it's available to the FIRST
            // workflow without a separate rescan. This is load-bearing: the first
            // asterisked op after a fresh open skips Prescan()/RefreshDisc() (the
            // `!freshlyScanned` guard in the WM_COMMAND handler), so whatever is
            // read here is the only metadata that first op (e.g. Copy disc) sees.
            // Reading CD-Text + ISRC here too -- not just MCN -- ensures the first
            // copy's .cue carries TITLE/PERFORMER and per-track ISRC instead of
            // coming out bare. (Previously only MCN was read here, so a Copy run as
            // the very first action produced a cue with no CD-Text.)
            g_copier.ReadCDText(g_disc);
            g_copier.ReadISRC(g_disc);
            g_copier.ReadMCN(g_disc);
        }
    } else {
        g_disc = DiscInfo{};
        g_hasTOC = false;
    }

    if (outFreshlyScanned) *outFreshlyScanned = true;
    return true;
}

// Write Disc (option 3) burns from .bin/.cue/.sub files on disk — it does not
// need an audio CD in the drive, so it should skip EnsureDriveOpen's 30-second
// "wait for audio disc" path when no audio CD is present.
bool ButtonNeedsAudioDisc(int btnIndex) {
    // Button index 2 == menu choice 3 == Write Disc.
    // Buttons that don't touch the drive at all (handled by ButtonNeedsDrive)
    // also don't need an audio disc.
    if (btnIndex == 2) return false;
    if (btnIndex == 26 || btnIndex == 27) return false;  // Check updates / Help
    if (btnIndex == 30) return false;                    // Erase CD-RW (any RW disc, may be full)
    return true;
}

// Check-for-updates (menu 26) and Help (menu 27) don't touch the drive at all
// — they just hit the network / print text. Skip EnsureDriveOpen entirely so
// the user isn't forced to insert a disc to read the help screen.
bool ButtonNeedsDrive(int btnIndex) {
    // Button index 26 == Check for updates (op id 26).
    // Button index 27 == Help (op id 27).
    return btnIndex != 26 && btnIndex != 27;
}

// Check-for-updates (menu 26) and Help (menu 27) are quick, drive-free
// operations — there's no need to grey out the rest of the menu while they
// run. The worker's IsRunning() guard still prevents overlapping workflows.
bool ButtonDisablesMenu(int btnIndex) {
    return btnIndex != 26 && btnIndex != 27;  // Check updates / Help
}

// GUI button indices (0-based) whose label ends in "*" — these workflows
// rely on per-track pregap data, so the TOC must be freshly read before
// they run. Kept in sync with the `*` markers in CommandLabels[].
bool ButtonNeedsPrescan(int btnIndex) {
    static const int prescanButtons[] = {
        0, 1, 2, 3,   // Copy disc, Rip tracks, Write disc, Write tracks
        4,            // Recovery rip (drive-independent)
        5, 6, 7, 8,   // Quality scan, C2 scan, BLER scan, Disc rot
        9, 10,        // Surface map, Multi-pass verify
        12,           // Audio content analysis
        15, 16,       // Subchannel integrity, Verify subchannel burn
        24,           // Disc balance check
    };
    for (int b : prescanButtons) {
        if (b == btnIndex) return true;
    }
    return false;
}

// Re-runs the TOC + pregap probe and refreshes the cached disc state.
// Used before asterisked workflows so they see up-to-date pregap layout
// even if the user has swapped discs since the last scan.
void Prescan() {
    DiscInfo fresh;
    if (g_copier.ReadTOC(fresh)) {
        g_disc = fresh;
        g_copier.ReadCDText(g_disc);
        g_copier.ReadISRC(g_disc);
        g_copier.ReadMCN(g_disc);
        g_hasTOC = true;
    }
}

// Full disc refresh: close and reopen the drive (which resets every cached
// per-drive probe in ScsiDrive::Open) before re-reading the TOC + metadata.
// Prescan() above re-reads the TOC on the *existing* handle, but after the user
// swaps the disc that handle — and the drive's own cache — can keep reporting
// the previous disc's TOC. Closing and reopening forces a clean re-detect, so
// Copy disc / Rip tracks always operate on the disc currently in the drive.
void RefreshDisc() {
    // No known drive letter to reopen — fall back to a same-handle TOC re-read.
    if (!g_audioDrive) { Prescan(); return; }

    Console::Info("Refreshing disc...\n");
    g_copier.Close();
    Sleep(1500);  // let the drive settle / spin up a freshly seated disc

    if (!g_copier.Open(g_audioDrive)) {
        Console::Error("Failed to reopen the drive for a disc refresh.\n");
        g_driveOpen = false;
        g_hasTOC = false;
        return;
    }

    // The disc may not be ready immediately after reopen (drive spinning up),
    // so retry the TOC read a few times, bailing early on user cancellation.
    DiscInfo fresh;
    bool ok = false;
    for (int attempt = 0; attempt < 10; attempt++) {
        if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) return;
        if (g_copier.ReadTOC(fresh)) { ok = true; break; }
        Console::Info("Waiting for disc to become ready...\n");
        Sleep(2000);
    }

    g_hasTOC = ok;
    if (ok) {
        g_disc = fresh;
        g_copier.ReadCDText(g_disc);
        g_copier.ReadISRC(g_disc);
        g_copier.ReadMCN(g_disc);
    } else {
        Console::Warning("No TOC found after refresh.\n");
    }
}

// Source-drive re-selection for the read workflows (Copy disc / Rip tracks /
// Recovery rip). EnsureDriveOpen short-circuits once any drive is open, and
// RefreshDisc reopens that same drive letter — so without this, a disc sitting
// in a *different* drive than the one opened first can never be reached.
//
// Re-scan the system and update g_audioDrive to the drive that should be read:
//   - no audio disc anywhere -> keep the current drive (RefreshDisc will reopen
//     it and report "no TOC" if it's empty);
//   - exactly one audio disc -> point at it, switching drives if it's not the
//     current one (no ambiguity, so no prompt — honours "prompt only when >1");
//   - more than one audio disc -> let the user choose via SelectAudioDrive.
//
// This only DECIDES the drive letter. The caller runs RefreshDisc() right after,
// which performs the single close/reopen + TOC read on whatever g_audioDrive now
// holds — so we deliberately don't open the handle here (that would just be an
// extra open/close that RefreshDisc immediately redoes). ScanDrives can probe
// the in-use drive because g_copier opens it with FILE_SHARE_READ|WRITE.
void ReselectSourceDriveIfMultiple() {
    std::vector<wchar_t> audioDrives;
    // Quiet scan — SelectAudioDrive prints its own numbered list.
    ScanDrives(audioDrives, /*verbose=*/false);

    if (audioDrives.empty()) return;            // keep current drive

    if (audioDrives.size() == 1) {
        g_audioDrive = audioDrives[0];          // switch if different; same = no-op
        return;
    }

    wchar_t pick = SelectAudioDrive(audioDrives);
    if (pick) g_audioDrive = pick;              // 0 = cancelled -> keep current
}

// Translate a GUI button index (0-based) to the DispatchMenuChoice case.
// `choice` is a stable legacy op id (see MainMenu.h), NOT the displayed button
// number. The Recovery rip is shown as button 5 (Ripping section) but carries
// op id 30 so the existing 1..29 cases didn't have to be renumbered; the
// buttons after it keep their original op ids while their *display* numbers
// shift up by one.
// Returns -1 if no dispatch is required (Batch/Clear/Exit are handled separately).
int ButtonToMenuChoice(int btnIndex) {
    if (btnIndex >= 0 && btnIndex <= 3) return btnIndex + 1;  // Copy/Rip/Write/WriteTracks → 1..4
    if (btnIndex == 4) return 30;                             // Recovery rip → op id 30
    if (btnIndex >= 5 && btnIndex <= 29) return btnIndex;     // shifted display, original op id
    if (btnIndex == 30) return 31;                            // Erase CD-RW → op id 31
    if (btnIndex == 31) return 32;                            // FE/TE servo scan → op id 32
    return -1;                                                // Batch/Clear/Exit
}

// Parse a space- or comma-separated list of menu numbers (1..29) into a vector
// of valid choices, in the user's listed order with duplicates dropped. Returns
// empty if the input has nothing recognisable.
std::vector<int> ParseBatchChoices(const std::string& input) {
    std::vector<int> out;
    std::string token;
    auto flush = [&](void) {
        if (token.empty()) return;
        try {
            int v = std::stoi(token);
            if (v >= 1 && v <= 30 &&
                std::find(out.begin(), out.end(), v) == out.end()) {
                out.push_back(v);
            }
        } catch (...) {}
        token.clear();
    };
    for (char c : input) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            token.push_back(c);
        } else {
            flush();
        }
    }
    flush();
    return out;
}
