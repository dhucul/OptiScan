// ============================================================================
// MenuUI.cpp - Help-screen renderer (the only console-menu helper that
// survived the GUI port; everything else was wired in via DispatchMenuChoice).
// ============================================================================
#include "MenuUI.h"
#include "ConsoleColors.h"
#include <iostream>

// ── PrintHelpMenu ───────────────────────────────────────────────────────
// Outputs a detailed help screen listing every available operation with a
// multi-line description and a "Best for:" hint.  Entries are grouped by
// category with visible section headers matching the main menu layout.
void PrintHelpMenu() {
	Console::Heading("\n=== Help - Test Descriptions ===\n");

	// Each HelpItem bundles a title, detailed description, and best-use hint.
	struct HelpItem { const char* title; const char* desc; const char* best; };

	// Helper lambda to print a single help entry with coloured title.
	auto PrintEntry = [](const HelpItem& item) {
		Console::SetColor(Console::Color::Cyan);
		std::cout << item.title << "\n";
		Console::Reset();
		std::cout << "   " << item.desc << "\n";
		std::cout << "   Best for: " << item.best << "\n\n";
		};

	// Helper lambda to print a section header matching the main menu style.
	auto PrintSection = [](const char* label) {
		Console::SetColor(Console::Color::DarkGray);
		std::cout << "-- " << label << " --\n\n";
		Console::Reset();
		};

	// ═════════════════════════════════════════════════════════════════════
	//  Ripping
	// ═════════════════════════════════════════════════════════════════════
	PrintSection("Ripping");

	PrintEntry({ "1. Copy Disc",
		"Rips audio tracks to WAV/FLAC files with optional AccurateRip verification.\n"
		"   Supports drive offset correction, subchannel extraction, pre-gap extraction,\n"
		"   secure rip modes (burst/standard/paranoid), and detailed logging.",
		"Creating high-quality digital backups of your audio CDs." });

	PrintEntry({ "2. Rip Tracks (WAV/FLAC)",
		"Rips individual or multiple tracks to separate WAV or FLAC files.\n"
		"   Select specific tracks or rip the entire disc track-by-track.\n"
		"   Supports burst and secure (C2-guided) rip modes, drive speed selection,\n"
		"   and drive offset correction.\n"
		"\n"
		"   FLAC encoding requires flac.exe in your system PATH or working directory.\n"
		"   If FLAC encoding fails, the WAV file is kept as a fallback.",
		"Extracting specific tracks as standalone audio files for playback or archiving." });

	PrintEntry({ "3. Write Disc (.bin/.cue/.sub Files)",
		"Burns audio data back to a blank CD-R/CD-RW from .bin/.cue/.sub files.\n"
		"   Supports writing audio images previously ripped with this tool,\n"
		"   including subchannel data restoration when .sub files are present.",
		"Creating accurate copies of audio CDs from previously ripped images." });

	PrintEntry({ "4. Write Tracks to Disc Using Current Disc's Pregaps",
		"Burns a new disc from individual ripped track files (.wav or .flac), using\n"
		"   the pregap durations of the disc currently in the drive. Files in the\n"
		"   chosen folder are matched to audio tracks alphabetically. The new disc\n"
		"   reproduces the source disc's track-to-track gap timing exactly.\n"
		"\n"
		"   FLAC inputs are decoded via flac.exe on the system PATH.\n"
		"\n"
		"   Pregap audio (between INDEX 00 and INDEX 01 of each track) is captured\n"
		"   fresh from the source disc at write time, with read-offset correction\n"
		"   applied so it aligns with the ripped WAVs. This preserves non-silent\n"
		"   pregaps (live albums, DJ sets) so AccurateRip CRCs match - AR's per-\n"
		"   track CRC actually covers each track *plus* the next track's pregap.\n"
		"\n"
		"   Write-offset compensation: prompts for an offset to pre-shift the audio\n"
		"   so the burned disc verifies against AccurateRip. The default suggestion\n"
		"   is -(read offset), which is correct for most drives. Without this, the\n"
		"   burned disc's track CRCs are shifted by the burner's write offset and\n"
		"   will not match AccurateRip / dBpoweramp.\n"
		"\n"
		"   Workflow:\n"
		"     1. Insert the source CD (its TOC supplies the pregap layout)\n"
		"     2. Pick a folder of one .wav/.flac per audio track\n"
		"     3. Choose the write-offset compensation\n"
		"     4. Eject and insert a blank CD-R/CD-RW when prompted\n"
		"     5. Choose write speed; the burn uses the standard write pipeline.",
		"Re-burning a CD from your ripped tracks while preserving its track gaps." });

	PrintEntry({ "5. Recovery Rip (Drive-Independent)",
		"Rips the disc with a recovery engine that rebuilds hard sectors from the\n"
		"   agreement of many re-reads instead of trusting the drive's C2. Each\n"
		"   problem sector is read repeatedly; the passes are aligned to defeat\n"
		"   re-read jitter, then majority-voted byte-by-byte. The drive's C2 is\n"
		"   used only as a tie-break, and only if it passes a reliability probe\n"
		"   first - on a lying drive the engine falls back to pure consensus.\n"
		"\n"
		"   Output is a .bin/.cue(/.sub) image plus a _recovery.txt report listing\n"
		"   which sectors were recovered, partial, or never readable. Slow by\n"
		"   design; use low speed on damaged discs. It cannot recover a sample\n"
		"   that every read got wrong the same way (audio reads expose no CIRC\n"
		"   parity) - those bytes are reported, not faked.",
		"Last-resort extraction of scratched or degrading discs that fail normal rips." });

	// ═════════════════════════════════════════════════════════════════════
	//  Disc Quality
	// ═════════════════════════════════════════════════════════════════════
	PrintSection("Disc Quality");

	PrintEntry({ "6. Quality Scan (Hardware Error Graphs)",
		"Performs a hardware-driven CIRC quality measurement: the drive enters a\n"
		"   dedicated scan mode and reports backend-specific counters per time\n"
		"   slice without transferring audio data. Plextor/LiteOn report C1, C2,\n"
		"   and CU; Pioneer reports C1/BLER plus diagnostic E22. This is the\n"
		"   same measurement QPXTool's Q-Check performs, and provides true C1\n"
		"   error rates the standard BLER scan cannot measure.\n"
		"\n"
		"   Three hardware backends are auto-detected, in order:\n"
		"     - Plextor Q-Check (0xE9/0xEB): classic Plextor drives -\n"
		"         PX-708A, PX-712A/SA, PX-716A/SA/AL, PX-755A/SA, PX-760A/SA\n"
		"     - Pioneer vendor scan (0x3B/0x3C): Pioneer BDR-* burners\n"
		"         (e.g. BDR-S13U). E22 is diagnostic-only; verified C2/E32 and\n"
		"         CU require CD Check support or independent rip verification.\n"
		"     - LiteOn/MediaTek (0xDF): LiteOn and rebadged MediaTek drives\n"
		"\n"
		"   Newer Plextor/Lite-On drives (PX-891SAF, etc.) support D8 reads but\n"
		"   not Q-Check; use option 8 (BLER Scan) on those instead.",
		"Hardware quality measurement with backend-accurate counter semantics." });

	PrintEntry({ "7. C2 Error Scan",
		"Performs a disc quality scan using the drive's C2 error reporting capability.\n"
		"   Auto-detects best C2 mode: error pointers (standard MMC), error block,\n"
		"   or Plextor vendor D8 commands when available.\n"
		"\n"
		"   Uses multi-pass scanning with cache defeat for accuracy.\n"
		"   Error sectors are re-read to verify results.\n"
		"\n"
		"   C2 errors indicate uncorrectable read errors. Use this for disc health\n"
		"   checks before ripping or detailed analysis of damaged discs.",
		"Quick disc health assessment or detailed error analysis before ripping." });

	PrintEntry({ "8. BLER Scan (Detailed)",
		"Measures Block Error Rate - the frequency of raw errors before correction.\n"
		"   Provides per-track statistics, error clustering analysis, zone distribution,\n"
		"   and a text-based error graph across the disc surface.\n"
		"\n"
		"   C1 block error reporting is auto-detected at startup.  Drives that populate\n"
		"   bytes 294-295 of the C2 response (Plextor D8, many LiteOn/ASUS/Pioneer\n"
		"   drives in ErrorPointers mode) will show full C1+C2 statistics.\n"
		"   Other drives report C2 errors only.\n"
		"\n"
		"   Red Book standard: average BLER should be < 220 errors/second.",
		"Professional-grade disc quality analysis with C1/C2 breakdown." });

	PrintEntry({ "9. Disc Rot Detection",
		"Analyzes error patterns to detect physical degradation (disc rot/bronzing).\n"
		"   Checks for characteristic edge deterioration and oxidation patterns.",
		"Evaluating older discs or checking storage conditions." });

	PrintEntry({ "10. Generate Surface Map",
		"Creates a visual representation of the entire disc surface quality.\n"
		"   Shows error density patterns, scratch locations, and problem areas.\n"
		"   Outputs a text file with a grid of symbols indicating sector health.",
		"Visual documentation of disc condition or identifying damaged regions." });

	PrintEntry({ "11. Multi-Pass Verification",
		"Reads the disc multiple times (2-10 passes) and compares results.\n"
		"   Inconsistent reads indicate marginal sectors or drive issues.\n"
		"   Sectors that differ between passes are flagged as unreliable.",
		"Maximum confidence in rip accuracy." });

	PrintEntry({ "12. Compare Disc CRCs (Original vs. Copy)",
		"Performs a full read of two discs (original then copy) and compares\n"
		"   CRC-32 checksums for every audio track. The tool reads the first\n"
		"   disc, ejects it, waits for the second disc, then reads and compares\n"
		"   track-by-track.\n"
		"\n"
		"   Read mode:\n"
		"     Burst  - Single fast read with no error correction (default).\n"
		"     Secure - C2-guided multi-pass read that re-reads sectors with C2\n"
		"              errors until consistent data is obtained. Use this when\n"
		"              discs may have scratches or marginal sectors.\n"
		"\n"
		"   A matching CRC on every track confirms a bit-perfect copy.\n"
		"   Mismatches pinpoint exactly which tracks differ.",
		"Verifying that a burned CD-R is an exact copy of the original disc." });

	// ═════════════════════════════════════════════════════════════════════
	//  Disc Information
	// ═════════════════════════════════════════════════════════════════════
	PrintSection("Disc Info");

	PrintEntry({ "13. Audio Content Analysis",
		"Analyzes audio characteristics: silence detection, clipping, DC offset,\n"
		"   and per-track RMS/peak levels.\n"
		"   (Pre-emphasis is verified separately by the Subchannel Integrity Check,\n"
		"   option 16, from the Q control field.)",
		"Understanding the audio mastering of the disc." });

	PrintEntry({ "14. Disc Fingerprint (CDDB/MusicBrainz/AccurateRip IDs)",
		"Generates unique disc identifiers for online database lookups:\n"
		"   - CDDB/FreeDB ID for metadata lookup\n"
		"   - MusicBrainz Disc ID for accurate metadata matching\n"
		"   - AccurateRip IDs for rip verification\n"
		"   - Audio content hash for duplicate detection\n"
		"   Results are saved to disc_fingerprint.txt.",
		"Looking up album metadata or verifying disc identity." });

	PrintEntry({ "15. Lead Area Check",
		"Examines lead-in and lead-out areas for hidden data or damage.\n"
		"   These areas contain TOC data and are critical for disc recognition.\n"
		"   Can reveal hidden track zero audio (HTOA) or pre-gap content.",
		"Diagnosing discs that fail to load or have TOC issues." });

	PrintEntry({ "16. Subchannel Integrity Check",
		"Verifies the integrity of subchannel data (Q-channel timing, etc.).\n"
		"   Subchannel errors can cause incorrect track indexing or timing issues.\n"
		"   Reports total error count across all sectors, then cross-checks the\n"
		"   in-track Q control field: pre-emphasis (vs. the TOC), 4-channel, and\n"
		"   copy-permit flags per track.",
		"Diagnosing timing/indexing issues, verifying subchannel extraction, or\n"
		"confirming whether a track needs de-emphasis for accurate audio." });

	PrintEntry({ "17. Verify Subchannel Burn Status",
		"Samples sectors across the disc and reads raw subchannel data to determine\n"
		"   whether subchannel information was actually mastered/burned onto the disc.\n"
		"   Checks Q-channel CRC validity, P-channel pause/play state, R-W channel\n"
		"   content (CD-G graphics), and MSF timing consistency.\n"
		"\n"
		"   Pressed/mastered CDs always have valid subchannel data. Burned CD-Rs may\n"
		"   or may not, depending on the burning software and settings used.",
		"Deciding if subchannel extraction is useful before ripping, or identifying burned copies vs. originals." });

	PrintEntry({ "18. Copy-Protection Check",
		"Scans the disc for common audio CD copy-protection mechanisms.\n"
		"   Performs 8 heuristic checks:\n"
		"   - Illegal / non-standard TOC entries\n"
		"   - Multi-session abuse (> 2 sessions)\n"
		"   - Data tracks mixed with audio (XCP / MediaMax indicator)\n"
		"   - Pre-emphasis flag anomalies\n"
		"   - Non-standard track gap sizes\n"
		"   - Intentional read errors (CDS / Key2Audio / MediaClyS)\n"
		"   - Subchannel data manipulation (Q-channel CRC / MSF jumps)\n"
		"   - Lead-in overread blocking\n"
		"\n"
		"   Results are aggregated into an overall verdict with a best-guess\n"
		"   identification of the protection scheme. Report saved to\n"
		"   protection_check.txt.",
		"Determining whether a disc uses copy protection before ripping." });

	// ═════════════════════════════════════════════════════════════════════
	//  Drive Diagnostics
	// ═════════════════════════════════════════════════════════════════════
	PrintSection("Drive");

	PrintEntry({ "19. Drive Capabilities",
		"Detects and displays your CD/DVD drive's hardware capabilities.\n"
		"   Shows support for: C2 errors, accurate stream, CD-TEXT, subchannel.\n"
		"   Also displays: read/write speeds, buffer size, overread capability.\n"
		"   Provides a ripping quality score to assess drive suitability.",
		"Checking if your drive is suitable for accurate ripping." });

	PrintEntry({ "20. Drive Offset Detection",
		"Automatically detects your CD drive's read offset using AccurateRip database.\n"
		"   Offset correction ensures sample-accurate rips that match the original master.\n"
		"   Displays the detected offset in samples along with a confidence percentage.",
		"Configuring your drive for accurate ripping." });

	PrintEntry({ "21. C2 Validation Test",
		"Tests the reliability of your drive's C2 error reporting at different speeds.\n"
		"   Some drives report false C2 errors at high speeds. This test verifies accuracy\n"
		"   by comparing C2 results at slow and fast speeds for consistency.\n"
		"   Tests up to 3 sectors spread across inner, middle, and outer disc regions.",
		"Determining if your drive's C2 detection is trustworthy before scanning." });

	PrintEntry({ "22. Speed Comparison Test",
		"Tests read performance at multiple speeds to find optimal ripping speed.\n"
		"   Slower speeds often yield better results on damaged discs.\n"
		"   Compares C2 error counts at each speed to identify the best trade-off.",
		"Determining the best speed for problematic discs." });

	PrintEntry({ "23. Seek Time Analysis",
		"Measures drive seek performance across the disc surface.\n"
		"   Slow seeks may indicate mechanical issues or disc damage.\n"
		"   Tests seek latency at various positions from inner to outer edge.",
		"Diagnosing drive performance or disc readability issues." });

	PrintEntry({ "24. Chipset Identification",
		"Identifies the internal chipset / controller used by your CD/DVD drive.\n"
		"   Displays chipset vendor, model, and firmware-level details when available.\n"
		"   Useful for determining hardware-level capabilities not exposed via standard\n"
		"   MMC feature queries.",
		"Identifying drive hardware for compatibility or capability research." });

	PrintEntry({ "25. Disc Balance Check",
		"Detects disc wobble or eccentricity by reading sample sectors at increasing\n"
		"   speeds (4x through 40x) and measuring how C2 errors change.\n"
		"\n"
		"   A well-balanced disc maintains low error rates across all speeds.\n"
		"   An unbalanced disc vibrates at high RPM, causing a sharp spike in\n"
		"   C2 errors and read failures above a certain speed threshold.\n"
		"\n"
		"   Reports a 0-100 balance score and recommends a safe rip speed.",
		"Diagnosing vibration/wobble issues or choosing optimal rip speed for warped/unbalanced discs." });

	// ═════════════════════════════════════════════════════════════════════
	//  Utility
	// ═════════════════════════════════════════════════════════════════════
	PrintSection("Utility");

	PrintEntry({ "26. Rescan Disc",
		"Re-scans drives and reloads disc metadata (TOC, CD-TEXT, ISRC, AccurateRip).\n"
		"   Automatically detects if the drive letter changed and re-opens the handle.\n"
		"   Supports switching between multiple drives if more than one is present.",
		"Use after swapping discs without restarting the program." });

	PrintEntry({ "27. Check for Updates",
		"Checks for a newer version of the tool online.\n"
		"   Compares the running version against the latest published release.",
		"Keeping the tool up to date." });

	PrintEntry({ "28. Help (Test Descriptions)",
		"Displays this help screen with detailed descriptions of each operation.",
		"Understanding the purpose and details of each operation." });

	PrintEntry({ "29. Pioneer CD Check (Audio Quality)",
		"Pioneer-only hardware audio-quality measurement. The drive scans the\n"
		"   audio range internally using WRITE/READ BUFFER 0xE6 at offset\n"
		"   0x300000. Quick mode samples every 0.05 mm of disc radius; Full mode\n"
		"   measures the complete audio range. Both report:\n"
		"     - C1 uncorrectable frame count\n"
		"     - C2 uncorrectable byte count\n"
		"     - Tracking-error (TE) peak and integration max\n"
		"\n"
		"   Pioneer's A/B/C/D thresholds grade corrected, uncorrectable, and\n"
		"   tracking-error measurements. PureRead is temporarily disabled so it\n"
		"   cannot hide raw errors, and the previous setting is restored afterward.\n"
		"\n"
		"   Firmware support varies. Capability byte 44 is advisory because some\n"
		"   drives report it incorrectly; OptiScan probes the command and leaves\n"
		"   unsupported or incomplete measurements explicitly unmeasured.",
		"Authoritative quality assessment of an audio CD on Pioneer drives." });

	PrintEntry({ "30. Jitter / Beta Scan (LiteOn)",
		"Hardware-driven physical-layer scan using the LiteOn/MediaTek vendor\n"
		"   command 0xDF/0x1B.  Reports per-time-slice jitter (EFM pit-timing\n"
		"   variation) and beta (pit/land asymmetry, ~0 = ideal).\n"
		"\n"
		"   Jitter complements C1/C2/CU measurements: it can flag a marginal\n"
		"   disc or pickup before BLER does, and tracks pressing quality on\n"
		"   pristine discs that show zero C1.\n"
		"\n"
		"   Requires legacy LiteOn 0xDF/0x1B jitter support. Some MediaTek drives\n"
		"   expose only 0xF3 quality scanning, which cannot report jitter/beta.\n"
		"   Output is a CSV log of (lba, jitter, beta) plus a summary report.",
		"Pressing-quality diagnostics and early-warning disc/drive health checks." });

	PrintEntry({ "31. Erase CD-RW (Rewritable)",
		"Blanks a rewritable (CD-RW) disc so it can be written again. No TOC\n"
		"   pre-scan is performed - the disc's current contents are erased, so\n"
		"   they don't need to be read first, even if the disc is full.\n"
		"\n"
		"   Checks that the media is actually rewritable (CD-R is write-once and\n"
		"   cannot be blanked), then offers:\n"
		"     Quick erase - clears the TOC/PMA only. Fast (typically under a\n"
		"                   minute) and makes the disc writable again.\n"
		"     Full erase  - wipes the entire recorded surface. Much slower\n"
		"                   (several minutes), but overwrites all old data.\n"
		"\n"
		"   A final confirmation is required before anything is erased.",
		"Reusing a CD-RW, or fully wiping one before disposal." });

	PrintEntry({ "32. Batch Run (multiple ops, 1 prescan)",
		"Runs several menu items in succession with a single shared pre-scan\n"
		"   (TOC + CD-Text + ISRC) at the start, so the disc only spins up once\n"
		"   for the whole batch.\n"
		"\n"
		"   Prompts for a space- or comma-separated list of menu numbers in the\n"
		"   range 1-30 (example: \"6 7 8 9\"). Duplicates are ignored and the\n"
		"   numbers are run in the order given. Use Clear output (View menu, or\n"
		"   the Clear info box button) during the batch to cancel at the next\n"
		"   operation boundary.\n"
		"\n"
		"   Each step still prompts for its own per-op choices (output folder,\n"
		"   speed, etc.) - the batch saves the pre-scan time, not the per-op\n"
		"   interaction. Avoid mixing Write Disc / Write Tracks (options 3/4)\n"
		"   with other ops in one batch, since those workflows may change the\n"
		"   active drive partway through.",
		"Running several quality scans or info readouts back-to-back on one disc." });

	PrintEntry({ "33. Clear Info Box",
		"Clears the output/info pane in the GUI.\n"
		"   Does not affect the disc, drive, or any in-progress operation -\n"
		"   only the on-screen log buffer is wiped. While a workflow is running\n"
		"   this button acts as Cancel: it signals the workflow (or batch loop)\n"
		"   to stop at the next checkpoint.",
		"Tidying the output area between operations, or cancelling a running workflow." });

	PrintEntry({ "34. Exit",
		"Exits the program. If a workflow is running, it is asked to cancel first.",
		"Closing the tool when done." });

	// ═════════════════════════════════════════════════════════════════════
	//  Accessibility
	// ═════════════════════════════════════════════════════════════════════
	PrintSection("Accessibility");
	std::cout <<
		"   OptiScan works with screen readers (NVDA, JAWS, Narrator).\n"
		"\n"
		"   Accessible output:  View > Accessible output  (Ctrl+Shift+A).\n"
		"   Replaces the graphical log with a standard read-only text box the\n"
		"   screen reader can read, review line by line, and copy. It turns on\n"
		"   automatically when a screen reader is detected at startup. In this\n"
		"   mode, graphs are written as plain-text summaries instead of bar art\n"
		"   and the menu-click sound is silenced.\n"
		"\n"
		"   Keyboard:  Tab / Shift+Tab move between the command buttons and the\n"
		"   output box; Space or Enter activates the focused button.\n"
		"\n"
		"   Operations menu:  every option is also on the Operations menu (press\n"
		"   Alt, then use the arrow keys or the underlined access keys), grouped\n"
		"   the same way as the on-screen buttons - so the whole option set is\n"
		"   reachable without tabbing across the grid.\n"
		"\n"
		"   Spoken status:  with a screen reader running, the program announces\n"
		"   when an operation starts, progress at 25/50/75/100%, and completion.\n"
		"\n";

	// Footer note about required privileges for raw SCSI access.
	Console::SetColor(Console::Color::DarkGray);
	std::cout << "Note: Administrator privileges are recommended for SCSI pass-through commands.\n";
	Console::Reset();
}
