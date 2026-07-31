# OptiScan
"You take the blue pill—the story ends, you wake up in your bed and believe whatever you want to believe.
You take the red pill—you stay in Wonderland, and I show you how deep the codebase goes."

A Windows **GUI application** for high-quality audio CD ripping, writing, and advanced disc diagnostics, written in C++.

OptiScan reads and writes audio CDs at the raw sector level using SCSI/MMC commands and provides multiple quality scanning modes to assess disc health before, during, or after extraction.

**[Download the latest release](https://github.com/dhucul/OptiScan/releases/latest)** — `OptiScan-<version>-Setup.exe`, a 64-bit installer for Windows 10 or later. It installs the Microsoft Visual C++ 2015–2022 runtime if the system doesn't already have it, so it requires administrator rights.

> [!IMPORTANT]
> **Drive compatibility is not universal.** OptiScan relies on low-level SCSI/MMC and vendor-specific optical-drive commands, so support depends on the exact drive model, firmware, chipset, USB bridge, and media type. A drive may work for normal ripping but still fail features such as pregap detection, subchannel reading/writing, CD-Text writing, C2/C1 reporting, or hardware quality scans.
>
> Known supported/targeted drive families vary by feature:
>
> | Drive family | Support level |
> |---|---|
> | **Classic Plextor** (PX-708, PX-712, PX-716, PX-755, PX-760, Premium/Premium2) | Best supported for audio extraction and Plextor Q-Check/vendor features |
> | **LiteOn / MediaTek-based drives** (including some ASUS and Plextor OEM models) | Supported for many ripping, subchannel, jitter/beta, and LiteOn/MediaTek quality-scan features, but some models may have subchannel/pregap quirks |
> | **Pioneer drives** (BDR/DVR families) | Partially supported; normal extraction and some Pioneer-specific quality checks may work, but not all ripping, writing, subchannel, pregap, or reporting features are supported on every model |
> | **LG/HL-DT-ST and other MediaTek-based drives** | Partially supported; some models work well for basic operations, while others may need slower reads or may reject raw read, subchannel, pregap, CD-Text, or write-mode commands |
> | **Generic MMC optical drives** | Basic ripping, writing, and scans may work if the required MMC commands are accepted, but advanced features are not guaranteed |
>
> Unsupported or untested drives should be treated as experimental. Always run **Drive capabilities & characterization** first and verify results before relying on a rip, burn, or quality report.

The original command-line workflow has been ported to a native Win32 GUI: every menu item is a button, output streams into an in-window console pane, and prompts (drive selection, output directory, numeric inputs, yes/no confirmations) appear as modal dialogs. Folder and file paths are picked through the standard Windows folder picker rather than typed by hand.

---

## Features

### Ripping
- **Burst, standard, and secure ripping** with configurable multi-pass verification and cache defeat
- **Drive-independent recovery rip** — rebuilds hard sectors from cross-read consensus (per-byte majority voting + jitter alignment) instead of trusting the drive's C2; outputs a `.bin`/`.cue`(/`.sub`) image plus a recovery report
- **Drive read offset correction** with auto-detection (AccurateRip database, pregap analysis, or manual)
- **AccurateRip V1** checksum calculation and online verification (V2 data is received from the server but not yet used for verification)
- **Pre-gap extraction** (include in image, skip, or extract separately)
- **Hidden track detection** — detects hidden audio before Track 1 (HTOA) and after the last track
- **Subchannel reading** with integrity verification
- **Whole-disc raw subchannel capture** for both audio and mixed-mode data sectors
- **CD-Text and ISRC extraction**
- **CUE sheet generation**
- **Archival preservation manifests** — CRC32, MD5, SHA-1, and SHA-256 for every rip artifact
- **Raw data-track validation** — automatic Mode 1, original formless Mode 2, and Mode 2 XA detection with the checks each format actually carries
- **Preservation write-offset analysis** — conservative header/index-boundary evidence recorded without changing playback or AccurateRip data
- **Disc fingerprinting** — CDDB, MusicBrainz, and AccurateRip disc IDs
- **TOC-less disc scanning** — reconstructs track layout from raw Q subchannel when the TOC is damaged or missing

### Disc Writing
- **Write audio CDs** from `.bin` / `.cue` / `.sub` file sets
- **Write tracks with source-disc pregaps** — burn a new disc from individual ripped `.wav` / `.flac` files while preserving the track-to-track gap durations of the disc currently in the drive
- **Automatic write mode negotiation** — tries Raw DAO with subchannel, falls back to SAO
- **Subchannel writing** — writes packed or raw P-W subchannel data from `.sub` files when the drive supports it
- **CD-Text writing** — builds and sends CD-Text packs (Title, Performer) from CUE sheet metadata
- **CD-RW detection and blanking** — detects rewritable media, supports quick and full blank with progress tracking
- **Optical Power Calibration (OPC)** — optional laser power calibration before writing
- **Disc capacity check** — verifies the image fits before writing begins
- **Write verification** — cache flush, session close, lead-out finalization, and post-write sector readback
- **Configurable write speed** with drive speed selection

### Disc Quality
- **Hardware quality scan** — Plextor Q-Check, Pioneer, or LiteOn/MediaTek scanning with backend-accurate graphs (Pioneer reports C1/BLER plus diagnostic E22, not verified C2/CU)
- **C2 error scan** — quick pass/fail quality check
- **BLER scan** — detailed per-second error rate with Red Book compliance check
- **Disc rot detection** — two-phase spatial degradation pattern analysis
- **Surface map** — per-sector C2 error CSV for external visualization
- **Multi-pass verification** — reads sectors N times to detect read inconsistency

### Disc Information
- **Audio content analysis** — detects silent, clipped, low-level, and DC-offset sectors
- **Disc fingerprint** — CDDB, MusicBrainz, and AccurateRip disc IDs
- **Lead area check** — scans the first/last 150 sectors for edge damage
- **Subchannel integrity check** — validates Q-channel CRC data across the disc
- **Subchannel burn status** — determines whether subchannel data was mastered onto the disc
- **Copy-protection detection** — heuristic scan for common audio CD protection schemes

### Drive Diagnostics
- **Drive capabilities & characterization** — active BE/D8, C2/subchannel layout, cache, and lead-area probes with per-firmware cached profiles
- **Drive offset detection** — auto-detects read offset via AccurateRip database or pregap analysis
- **C2 validation test** — verifies that the drive's C2 error reporting is reliable
- **Speed comparison test** — reads sectors at two speeds to detect surface instability
- **Seek time analysis** — measures seek latency to detect mechanical issues
- **Chipset identification** — identifies the drive's internal chipset/controller, interface type, and USB bridge
- **Disc balance check** — detects vibration and wobble by sweeping read speed from 4× to 40×

### Utility
- **Rescan disc** — re-reads disc TOC and metadata; supports switching between multiple drives
- **Check for updates** — queries GitHub Releases for newer versions
- **Help** — built-in test descriptions
- **Pioneer CD Check** — hardware audio-quality measurement on supported Pioneer drives (`WRITE/READ BUFFER 0xE6`) with Quick 0.05 mm radial sampling or a full-range pass, reporting C1/C2 uncorrectable counts and tracking-error peaks with Pioneer's A/B/C/D grading
- **Jitter / beta scan** — LiteOn/MediaTek vendor scan (`0xDF/0x1B`) reporting per-time-slice EFM jitter and pit/land asymmetry
- **Batch run** — runs multiple menu items in succession with a single shared pre-scan (TOC + CD-Text + ISRC) at the start, instead of re-reading the disc between each operation
- **Clear info box** — clears the GUI output pane when idle; doubles as Cancel for the active workflow (or current batch step) while a workflow is running

---

## Interactive Menu

OptiScan's main window has a **navigation rail** down the left with six pages — Overview, Rip & Copy, Disc Quality, Analysis, Drive Tools, Utilities. Each page shows only its own commands as cards; **Overview** shows everything, led by three hero cards for the most common jobs (Copy disc, Rip tracks, Quality scan). The output console spans the bottom of every page.

Clicking a card runs that operation on a background worker thread; the console streams progress while a status line and progress meter track long-running reads/writes. Only one workflow runs at a time — the **Clear output** card doubles as a Cancel while a workflow is in flight.

The table below lists every operation with the number shown on its card. That number is stable across pages and matches the **Operations** menu, so it is the reliable way to refer to a command.

| # | Page | Operation |
|---|---|---|
| 1 | Rip & Copy | Copy disc — *hero card on Overview* |
| 2 | Rip & Copy | Rip tracks (WAV/FLAC) — *hero card on Overview* |
| 3 | Rip & Copy | Write disc (.bin/.cue/.sub files) |
| 4 | Rip & Copy | Write tracks to disc using current disc's pregaps |
| 5 | Rip & Copy | Recovery rip (drive-independent) |
| 6 | Disc Quality | Quality scan (hardware error graphs) — *hero card on Overview* |
| 7 | Disc Quality | C2 error scan |
| 8 | Disc Quality | BLER scan (detailed) |
| 9 | Disc Quality | Disc rot detection |
| 10 | Disc Quality | Generate surface map |
| 11 | Disc Quality | Multi-pass verification |
| 12 | Disc Quality | Compare disc CRCs (original vs. copy) |
| 13 | Analysis | Audio content analysis |
| 14 | Analysis | Disc fingerprint (CDDB/MusicBrainz/AccurateRip IDs) |
| 15 | Analysis | Lead area check |
| 16 | Analysis | Subchannel integrity check |
| 17 | Analysis | Verify subchannel burn status |
| 18 | Analysis | Copy-protection check |
| 19 | Drive Tools | Drive capabilities & characterization |
| 20 | Drive Tools | Drive offset detection |
| 21 | Drive Tools | C2 validation test |
| 22 | Drive Tools | Speed comparison test |
| 23 | Drive Tools | Seek time analysis |
| 24 | Drive Tools | Chipset identification |
| 25 | Drive Tools | Disc balance check |
| 26 | Utilities | Rescan disc |
| 27 | Utilities | Check for updates |
| 28 | Utilities | Help (test descriptions) |
| 29 | Drive Tools | Pioneer CD Check (audio quality) |
| 30 | Drive Tools | Jitter / beta scan (LiteOn) |
| 31 | Drive Tools | Erase CD-RW (rewritable) |
| 32 | Drive Tools | FE/TE servo scan (experimental, LiteOn) |
| 33 | Utilities | Batch run (multiple ops, 1 prescan) |
| 34 | Utilities | Clear info box |
| 35 | Utilities | Exit |

Operations marked with **\*** in the Operations menu use pre-gap analysis (scan range includes pregap sectors). Some cards shorten the full name to fit — the Operations menu always carries the complete wording.

**Clear info box** (card caption: *Clear output*) clears the console when idle and acts as a Cancel for the active workflow — or for the current step of a batch — while one is running.

---

## Accessibility

OptiScan is usable with screen readers (NVDA, JAWS, Narrator) and fully operable from the keyboard.

- **Accessible output mode** — *View ▸ Accessible output* (**Ctrl+Shift+A**) replaces the custom-drawn log panel with a standard read-only multiline text box that screen readers read natively, and that supports caret review and copy. It **turns on automatically** when a screen reader is detected at startup. In this mode the C1/C2/CU graphs, heatmaps, and score bars are written as plain-text numeric summaries instead of Unicode bar art, and the synthesized menu-click sound is silenced. It honours Windows High Contrast themes; otherwise it matches the app's dark console theme.
- **Keyboard navigation** — **Tab / Shift+Tab** move focus between the command buttons and the output box (the focused button shows a bright border); **Space** or **Enter** activates the focused button.
- **Operations menu** — every command is also available from the **Operations** menu (press **Alt**, then use the arrow keys or the underlined access keys), grouped the same way as the on-screen buttons, so the whole option set is reachable without tabbing across the grid. **File**, **View**, and **Help** menus cover app commands; *View ▸ Clear output* mirrors the Clear button.
- **Spoken status** — with a screen reader running, the program announces when an operation starts, progress milestones (25/50/75/100%), and completion, via UI Automation notification events.
- **Dialogs** — drive selection, folder pickers, and text/confirmation prompts are standard keyboard-accessible dialogs (Tab, Enter, Esc).

---

## Appearance & Themes

OptiScan ships with a runtime theme switcher. Pick a theme from **View ▸ Theme** and the entire UI re-colours live — window chrome, command buttons, input dialogs, the log/console text roles, and the C1/C2/CU/BLER graph colours — and the choice is remembered for next launch (per user).

| Theme | Look |
|---|---|
| **Graphite** *(default)* | The original cool graphite/slate with a warm-tan accent |
| **Apple Light** | Soft-white rail and cards over a saturated purple instrumentation canvas, with an Apple-blue accent |
| **Catppuccin Frappé** | Soft pastel dark — lavender/blue base with peach and mauve accents |
| **Nord** | Arctic slate with frost-blue accents and aurora status colours |
| **Arc-Dark** | Flat blue-grey dark with the signature Arc blue accent |

Every theme renders the **same layout and the same style** — navigation rail, hero cards, command cards, output card — and differs only in colour. Behind the content sits a **procedural instrumentation canvas** drawn from the active palette (gradient base, radial scan glow and rays, a measurement dot matrix, concentric scan arcs, a horizon line), so each theme looks native rather than recoloured. Graph severity colours (green → yellow → red), the Red Book reference line, and log status tags (`[OK]`/`[WARN]`/`[ERROR]`) all follow the active palette too.

Colour is the *only* thing a theme controls: every role lives in one `Palette` struct (`Theme.h`), and the renderer reads it rather than hardcoding literals. The output console stays dark in all five themes — including Apple Light, which is deliberately a hybrid of light chrome around a dark log.

Windows **High Contrast** themes are honoured: when High Contrast is active the app defers to the system colours instead of applying its own.

---

## Rip Modes

OptiScan offers five rip modes with increasing verification. The mode determines how many re-read passes are performed and how errors are handled.

| Mode | Passes | Matches required | Cache defeat | Description |
|---|---|---|---|---|
| **Burst** | 1 | — | No | Maximum speed, no verification |
| **Standard** | 1 | 1 | No | Single pass with retry on error, C2-guided |
| **Secure Fast** | 2 | 2 | No | Light verification with minimal re-reads |
| **Secure Standard** | 3–6 | 2 | Yes | Balanced re-read strategy |
| **Secure Paranoid** | 4–8 | 3 | Yes | Maximum accuracy, speed-capped |

All secure modes use C2 error pointers when available — a clean C2 read is trusted as verified, skipping unnecessary re-reads. Cache defeat forces a seek to a distant location between reads to ensure each pass is a true disc re-read rather than cached data.

For discs that fail even Secure Paranoid — or when the drive's C2 reporting can't be trusted — use the separate **[Recovery Rip](#recovery-rip-drive-independent)** engine (menu option 5), which rebuilds hard sectors from cross-read consensus instead of relying on the drive's C2.

### Pioneer Real-Time PureRead diagnostics

On Pioneer drives that support Real-Time PureRead, full-disc and selected-track rips isolate the firmware's cumulative PureRead counters to the current read session. After all extraction and requested physical-verification passes finish, OptiScan reports the number of PureRead error-sector events, transferred sectors, their ratio, the last transfer position, and Pioneer's **Perfect / Better / Good / Not Good / Bad / Fatal** indicator.

The summary is diagnostic only: a PureRead error event is not verified C2/CU data and does not by itself prove the output is incorrect. Rip success, error handling, and AccurateRip decisions remain unchanged. Full-disc copies write `<basename>_pureread.log`; selected-track rips write `PioneerPureRead.log` in the output folder. OptiScan reads the counters only before and after the session, never concurrently with audio-sector transfers.

### When to Use Each Mode

| Mode | Best for | Speed | Trade-off |
|---|---|---|---|
| **Burst** | Pristine discs, quick personal rips where AccurateRip will verify the result anyway | Fastest | No verification — a misread is kept silently |
| **Standard** | Everyday rips of clean discs; C2-guided retry catches the occasional bad sector | Fast | Single pass; relies on the drive's C2 being honest |
| **Secure Fast** | Lightly worn discs where you want a verification pass without a big time cost | Moderate | Only 2 passes — limited rescue on stubborn sectors |
| **Secure Standard** | The general-purpose archival default for most discs | Slower | Cache defeat + re-reads make it noticeably slower than Standard |
| **Secure Paranoid** | Scratched or important discs where bit-perfect accuracy outweighs time | Slowest | Speed-capped; can be very slow on damaged media |
| **[Recovery Rip](#recovery-rip-drive-independent)** | Last resort: discs that fail Secure Paranoid, or drives whose C2 the validation test flags as unreliable | Very slow (use a low speed) | Consensus-based, not C2-based; cannot recover samples every pass misreads identically |

**Rule of thumb:** start with **Secure Standard** for archival rips and **Burst**/**Standard** for casual ones. Escalate to **Secure Paranoid** when a disc reports errors, and only fall back to **Recovery Rip** when secure modes can't get a clean read.

---

## Recovery Rip (Drive-Independent)

**Question answered:** *"This disc fails a normal rip and the drive's C2 can't be trusted — can I still get the audio out?"*

The recovery rip is a separate read engine from the secure rip. Where secure ripping trusts the drive's C2 error pointers to decide which sectors to re-read, the recovery rip treats C2 as, at most, a hint. Hard sectors are rebuilt from the **statistical agreement of many re-reads** instead.

### The strategy

| Technique | What it does | Trusts the drive's C2? |
|---|---|---|
| **Per-byte majority voting** | Reads a problem sector many times; each of the 2352 byte positions settles on the value that reaches a quorum of agreeing reads. Corruption that is *unstable* across reads is voted out. | No |
| **Jitter / offset alignment** | Each re-read is read as an overlapping multi-sector window and slid against a reference frame before voting, so drives that mis-seek on re-read (returning sample-shifted audio) still contribute aligned votes instead of garbage. | No |
| **Hybrid C2 tie-break** | The drive's C2 bitmap is used only as a per-byte vote filter, and only after a reliability probe (the C2 validation test) confirms the drive's C2 is trustworthy. On a lying drive it is ignored and the engine runs pure consensus. | Only after validation |

### How it runs

1. **C2 gate** — probe whether the drive's C2 is reliable. Only then is C2 used as a tie-break.
2. **Baseline sweep** — read every sector once. C2-clean reads are trusted; failed or C2-flagged audio sectors are queued for rescue.
3. **Consensus rescue** — for each queued sector, read the overlapping window up to *N* times, align each pass to defeat jitter, then majority-vote every byte. A sector is **Recovered** when every byte reaches quorum, **Partial** when some bytes never do (the best-vote value is kept and flagged), or **Unrecovered** if it never read at all.

**Output:** a `.bin`/`.cue`(/`.sub`) image (same format as Copy disc), a `_recovery.txt` report, and a multi-hash `.manifest.json`. During extraction, `.recovery.state` and `.recovery.partial.bin` sidecars make the job resumable. Re-running with the same output basename continues only after a stable content fingerprint confirms the matching disc, even on another drive; OptiScan translates the new drive's read offset into the checkpoint drive's sample coordinate before voting. Incompatible or corrupt checkpoints are retained with an `.invalid` suffix for diagnosis. Active sidecars are removed only when there are no partial, unrecovered, or requested-subchannel failures and the final image, verification, reports, and manifest all succeed.

The recovery report lists every problem sector's status, confirmed-byte count, maximum jitter, and **C2-disputed byte count** — bytes consensus accepted but the drive's C2 flagged bad on a majority of reads (when the C2 tie-break was active). Those are kept as consensus's best value but called out as a heads-up, since the drive considered the sample uncorrectable.

**The hard limit:** audio reads never expose the disc's CIRC parity, so a sample that *every* pass misread the same way is indistinguishable from a correct one and cannot be recovered. The recovery rip reports those bytes rather than silently faking them — it recovers the *unstable*, and is honest about the *stably wrong*.

**When to use:** as a last resort on scratched or degrading discs that fail Burst/Secure rips, especially on drives whose C2 reporting the C2 validation test flags as unreliable. It is slow by design — use a low speed.

---

## Quality Scan Modes

OptiScan offers multiple quality scans. Each answers a different question about a disc.

### Quality Scan — Hardware-Driven

**Question answered:** *"What quality counters does this drive's hardware report across the disc?"*

This scan uses **vendor-specific hardware commands** to put the drive into a dedicated error-measurement mode — **no audio data is transferred**. Plextor and LiteOn backends report C1/C2/CU statistics. Pioneer reports C1/BLER plus its raw E22 diagnostic counter; E22 is not treated as verified C2/E32 or CU, and cannot by itself establish copy integrity.

On a Pioneer backend, Q-Check also invokes the separate CD Check protocol over the full audio range when firmware supports it. BLER, C2, and Disc Rot use that same shared path through their Pioneer fallback. The engine disables PureRead temporarily, enters the utility's prepare/inspect state, retries transient commands three times, always stops measurement and restores inspection mode, and accepts a clean zero only after the requested range completed with valid data. Unsupported, partial, cancelled, stalled, and invalid scans remain explicitly **unmeasured**.

Three command sets are supported:

| Drive type | Command set | Detection |
|---|---|---|
| **Classic Plextor** (PX-708A, PX-712A/SA, PX-716A/SA/AL, PX-755A/SA, PX-760A/SA) | Q-Check vendor commands `0xE9` (start) / `0xEB` (poll) | Auto-probed at startup |
| **Pioneer** (DVR/BDR families, including BDR-S13U where firmware permits) | Vendor `WRITE/READ BUFFER` `0x3B/0x3C`, reporting BLER plus diagnostic E22; CU/E32 requires separate CD Check support | Auto-probed after Plextor Q-Check |
| **LiteOn / MediaTek-based** (LiteOn, ASUS, some Plextor OEM) | LiteOn vendor commands `0xF3` (new) or `0xDF` (old) | Auto-probed if Q-Check is unavailable |

If none of these command sets is supported by the drive, the scan reports the incompatibility and suggests using the BLER scan (option 8) instead.

**Output:** Per-second backend-specific time-series data, aggregate statistics (total, average, peak), quality rating, and a CSV log (`qcheck_scan.csv`) for graphing. Pioneer CSV rows contain only measured C1 and diagnostic E22 columns; unmeasured C2/E32 and CU fields are omitted rather than serialized as clean zeroes.

**When to use:** For drive-reported quality trends without relying on host-side C2 pointer interpretation. Plextor/LiteOn provide the fullest C1/C2/CU assessment; Pioneer C1/E22 remains diagnostic and should be paired with secure extraction/AccurateRip for a copy decision.

---

### C2 Error Scan (Quick)

**Question answered:** *"Does this disc have uncorrectable errors?"*

A CD drive performs two internal error-correction stages. **C1** corrects minor errors transparently. **C2** is the second and final stage — a C2 error means the drive's hardware could not fully correct the data and the returned audio samples may be wrong.

This scan reads every audio sector once and asks the drive to report C2 error pointers. It supports four sensitivity modes:

| Mode | Behavior |
|---|---|
| **Standard** | Single pass, byte-level C2 counting |
| **PlexTools-style** | Single pass with cache defeat, then conditional re-read of error sectors |
| **Multi-pass** | Two-pass scan without cache defeat (faster) |
| **Paranoid** | Cache defeat + conditional three-pass re-read of error sectors |

Optionally performs **dual-speed validation** — re-reads error sectors at a different speed to distinguish real media errors from speed-dependent read artifacts.

**Output:** Total C2 count, per-sector error counts, error LBA list, and a CSV log (`c2_scan.csv`).

**When to use:** Quick check before ripping to decide whether standard or secure mode is needed.

---

### BLER Scan (Detailed)

**Question answered:** *"What is the error rate over time, and does it meet Red Book standards?"*

**BLER** (Block Error Rate) is an IEC 60908 (Red Book) concept. The standard defines maximum acceptable error rates measured per second of audio playback. OptiScan reads every audio sector, counts C2 errors, and aggregates them into one-second time buckets (75 sectors = 1 second at 1× CD speed). The result is a complete time-series error profile of the disc.

#### C1 Block Error Reporting

A CD drive performs two internal error-correction stages. **C1** corrects minor errors transparently — every disc has some C1 activity, even in perfect condition. **C2** is the second and final stage — a C2 error means the drive's hardware could not fully correct the data.

Some drives expose per-sector C1 block error counts in bytes 294–295 of the C2 error pointer response. OptiScan auto-detects this at startup by probing sample sectors across the disc. When available, the BLER report includes full C1 statistics alongside C2, giving a much more complete picture of disc health — a disc can have zero C2 errors but elevated C1 rates indicating early wear.

| Drive type | C1 reporting | Detection method |
|---|---|---|
| **Plextor (D8-capable)** | Always available | Vendor command `0xD8` returns C1/C2 block error stats |
| **LiteOn, ASUS, Pioneer** | Often available | Standard MMC ErrorPointers mode, bytes 294–295 probed |
| **Other MMC drives** | Auto-detected | Probes 150 sectors across 3 zones; reports C1 if any byte 294 is non-zero |
| **ErrorBlock-only drives** | Not available | Different data layout; C1 bytes are not present |

#### Metrics

| Metric | Description |
|---|---|
| **Avg C1/sec** | Mean C1 corrections per second (when available) |
| **Max C1/sec** | Peak one-second C1 count |
| **Avg C2/sec** | Mean C2 errors per second across the entire disc |
| **Max C2/sec** | Peak one-second error count (with timestamp) |
| **C1 utilization** | Average C1 rate as a percentage of the Red Book 220/sec limit |
| **C2 margin** | How close C1 is to exhausting C2 correction capacity (WIDE / ADEQUATE / NARROW / CRITICAL / EXHAUSTED) |
| **Red Book threshold** | Avg BLER < 220/sec = PASS (IEC 60908 compliance) |
| **Quality threshold** | Avg C2/sec < 1.0 = GOOD for archival ripping |
| **Per-track breakdown** | C1 count, C2 count, affected sectors, avg/sec, and status per track |
| **ASCII error graph** | Visual distribution of C2 errors across the disc timeline |

#### C1 Assessment Scale

| C1 Avg/sec | Assessment |
|---|---|
| < 5 | **EXCELLENT** — minimal correction needed |
| 5–50 | **GOOD** — normal wear |
| 50–220 | **FAIR** — elevated but within Red Book limits |
| > 220 | **POOR** — exceeds Red Book BLER limit |

#### Overall Quality Rating

| Rating | Criteria |
|---|---|
| **EXCELLENT** | Zero C2 errors |
| **GOOD** | Avg < 1.0/sec, longest error run < 3 sectors |
| **ACCEPTABLE** | Avg < 10.0/sec, longest error run < 10 sectors |
| **POOR** | Above acceptable thresholds |
| **BAD** | Read failures occurred (sectors could not be read at all) |

**Output:** Full report printed to console, plus a CSV log (`bler_scan.csv`) with per-second LBA and error count for graphing in external tools.

**When to use:** Detailed quality assessment — see exactly where errors are, whether the disc meets Red Book limits, and whether it is safe to archive. The C1 data (when available) reveals early degradation that C2-only scans miss entirely.

---

### Disc Rot Detection

**Question answered:** *"Is this disc physically degrading, and how urgently should I back it up?"*

Disc rot is the chemical or physical deterioration of a CD's reflective aluminum layer. It produces **characteristic spatial error patterns** that are distinct from scratches, fingerprints, or manufacturing defects. A disc can have zero C2 errors on a single read pass and still be in early-stage rot, detectable only through read instability across multiple passes.

OptiScan performs a two-phase scan:

**Phase 1 — C2 error distribution**
Reads the entire disc and classifies every sector into three radial zones:

| Zone | Disc position |
|---|---|
| **Inner** | 0–33% (near the center hub) |
| **Middle** | 33–66% |
| **Outer** | 66–100% (near the outer edge) |

Error rates are computed per zone to reveal spatial concentration patterns.

**Phase 2 — Adaptive read consistency**
Re-reads sampled sectors multiple times (3 passes per sample) to detect **read instability** — the same sector returning different audio data on different reads. The sampling density adapts per zone: zones with higher error rates from Phase 1 receive denser sampling (down to every 20th sector) while clean zones are sampled sparsely (every 200th sector).

The scan then evaluates four degradation indicators:

| Indicator | Detection rule | What it means |
|---|---|---|
| **Edge concentration** | Outer error rate > 2× inner rate and > 1% | Rot typically starts at the disc edge where the protective lacquer is thinnest |
| **Progressive pattern** | Error rate increases monotonically inner → middle → outer, outer > 0.5% | Classic inward-spreading rot progression |
| **Pinhole pattern** | > 10 small clusters (≤ 3 sectors) comprising > 50% of all clusters | Microscopic holes in the reflective layer caused by oxidation |
| **Read instability** | > 5% of re-read samples return different data | The reflective layer is intermittently unreadable — data is being lost |

A weighted scoring system produces the final risk level:

| Indicator | Weight |
|---|---|
| Edge concentration | +25 |
| Progressive pattern | +25 |
| Read instability | +20 |
| Pinhole pattern | +15 |
| Inconsistency rate > 10% | +15 |

| Score | Risk level |
|---|---|
| 0–9 | **NONE** — Disc appears healthy |
| 10–29 | **LOW** — Minor issues, consider backing up soon |
| 30–49 | **MODERATE** — Early degradation, back up immediately |
| 50–74 | **HIGH** — Significant degradation, back up NOW |
| 75–100 | **CRITICAL** — Severe damage, extract whatever data is possible |

**Output:** Zone error rates, cluster analysis, indicator flags, risk assessment, and a recommendation. Saved as a text report (`discrot_report.txt`).

**When to use:** When you suspect physical deterioration (visible bronzing, edge discoloration, age > 15 years) and need to know whether data loss is imminent.

---

## C2/BLER vs. Disc Rot — Summary of Differences

| | C2 Scan | BLER Scan | Disc Rot Detection |
|---|---|---|---|
| **Question** | Are there uncorrectable errors? | What is the error rate over time? | Is the disc physically degrading? |
| **C1 reporting** | No | Yes (auto-detected per drive) | No |
| **Read passes** | 1–3 (configurable) | 1 | Full disc + adaptive multi-pass sampling |
| **Spatial analysis** | No | Zone distribution (inner/middle/outer) | Yes — three-zone radial classification |
| **Read consistency** | Not tested | Not tested | Multi-pass re-read detects instability |
| **Pattern analysis** | None | Per-second bucketing, per-track totals, error clustering | Edge concentration, progressive gradient, pinhole clusters |
| **Typical cause detected** | Scratches, fingerprints, poor burns | Same as C2 but with temporal context + early wear via C1 | Chemical oxidation, delamination, bronzing |
| **Output format** | Console report + CSV | Console report + CSV + ASCII graph | Console report + text log |
| **Actionable result** | "Use secure rip" or "clean the disc" | "Meets/fails Red Book" or "use Paranoid mode" | "Back up NOW — data loss imminent" |
| **Speed** | Fast (minutes) | Moderate (full disc read) | Slow (full disc read + re-read sampling) |

**Key insight:** A disc can pass a C2 scan with zero errors yet still be in early-stage rot — Phase 2's read consistency check catches degradation that a single read pass cannot. The BLER scan's C1 data (when available) fills the gap between "zero C2" and "actual disc health" — elevated C1 rates reveal wear that hasn't yet progressed to uncorrectable errors. Conversely, a disc with high BLER from a surface scratch will show **no rot indicators** because the damage is mechanical, not chemical.

---

## Disc Balance Check

**Question answered:** *"Is this disc mechanically balanced, and what is the maximum safe rip speed?"*

Unbalanced or warped CDs vibrate at high rotation speeds, causing read instability, jitter, and in severe cases, read failures. OptiScan sweeps the drive through six read speeds (4×, 8×, 16×, 24×, 32×, 40×) and measures four independent metrics at each speed:

| Metric | What it measures |
|---|---|
| **Error rate** | C2 errors per sector (or hardware C1/C2 via LiteOn `0xDF` when available) |
| **Read time jitter** | Coefficient of variation of per-sector read times |
| **Stability ratio** | Per-sector read time consistency (higher = more wobble) |
| **Speed scaling** | Whether actual throughput scales linearly with requested speed |

Each metric produces a 0–100 sub-score. These are blended into a single **Balance Score**:

| Score | Assessment | Recommendation |
|---|---|---|
| 75–100 | **GOOD** — disc is well balanced | Any rip speed is safe |
| 50–74 | **FAIR** — some wobble detected | Reduce rip speed |
| 0–49 | **POOR** — significant balance problem | Use 4×–8× maximum |

The scan also determines the **maximum safe rip speed** — the highest speed at which no wobble degradation was detected. On Pioneer drives, Disc Balance additionally runs the utility-compatible Quick CD Check at 0.05 mm radial intervals and reports genuine uncorrectable bytes separately. That sampled data-loss result never changes the mechanical balance score, and a failed or unsupported CD Check is shown as **unmeasured**, never as zero errors.

**Output:** Per-speed error rates, jitter statistics, sub-scores, balance score, and safe speed recommendation.

**When to use:** Before ripping at high speed, or when you suspect a disc is warped, cracked near the hub, or has an off-center label.

---

## Chipset Identification

**Question answered:** *"What chipset is in my drive, and does it have any known audio extraction quirks?"*

OptiScan identifies the drive's internal chipset/controller by combining:

1. **SCSI INQUIRY** vendor, model, and firmware strings
2. **Known model-to-chipset lookup table** covering Plextor, LiteOn, Pioneer, ASUS, Samsung/TSSTcorp, Sony, NEC/Optiarc, Panasonic, LG/HLDS, Philips, and Ricoh
3. **Firmware signature analysis** for unrecognized models
4. **Interface type probing** — detects SATA, IDE/ATAPI, USB, or SCSI attachment via `IOCTL_STORAGE_QUERY_PROPERTY`
5. **USB bridge identification** — recognizes JMicron, ASMedia, Realtek, and VIA bridge chips

The report includes the chipset family, detection confidence, interface type, USB bridge (if applicable), and any known audio extraction quirks for the identified chipset.

**When to use:** When setting up a new drive — understanding the chipset helps choose optimal extraction settings and explains drive-specific behavior (e.g., TSSTcorp drives may report inaccurate C2 data).

---

## Subchannel Data Extraction

### What is subchannel data?

Every CD sector contains two separate data streams read simultaneously by the laser:

| Channel | Size per sector | Content | Error correction |
|---|---|---|---|
| **Main channel** | 2,352 bytes | Audio samples (or data) | CIRC — two layers of Reed-Solomon (C1 + C2) with interleaving |
| **Subchannel** | 96 bytes | Navigational metadata split across 8 sub-channels (P, Q, R, S, T, U, V, W) | None — only a 16-bit CRC on the Q channel |

The **P channel** carries a simple pause/play flag. The **Q channel** carries track number, index, and MSF timestamps — the data a CD player uses for its display. The **R–W channels** are optional and carry CD-G graphics (karaoke), CD-TEXT, or are simply empty.

### When to enable subchannel extraction

When ripping, OptiScan asks whether to include subchannel data. Enabling it creates an additional `.sub` file alongside the `.bin` image. Use the following guidelines:

| Scenario | Include subchannel? | Reason |
|---|---|---|
| **Archival / preservation rip** | **Yes** | Preserves the complete disc image including all metadata channels. A `.bin`+`.sub`+`.cue` set is a bit-perfect representation of the original disc. |
| **CD-G / karaoke disc** | **Yes** | The R–W channels contain the graphics data. Without subchannel extraction, the karaoke visuals are lost. |
| **CD-TEXT disc** | **Yes** | Artist and title metadata encoded in the R–W subchannel is preserved. |
| **Disc with hidden tracks or non-standard indexing** | **Yes** | Raw Q-channel data captures index points and timing that may not be fully represented in the TOC. |
| **Pressed / factory disc (standard audio)** | **Optional** | Pressed CDs always have valid P+Q subchannel data, but the R–W channels are usually empty on standard audio CDs. Extraction preserves timing metadata but adds no audio content. |
| **Burned CD-R (standard audio)** | **No** | Most CD-R burns do not write meaningful R–W subchannel data. The P+Q timing is generated automatically by the drive and is typically less reliable than the TOC. |
| **Quick rip for personal listening** | **No** | Subchannel data is not needed for playback. Skipping it produces a smaller output and slightly faster rips. |

### How to decide

If you are unsure whether a disc has subchannel content worth preserving, run **option 17 — Verify Subchannel Burn Status** before ripping. This samples sectors across the disc and reports:

- Whether Q-channel CRC data is valid (indicating reliable subchannel data was mastered/burned)
- Whether R–W channels contain CD-G graphics or CD-TEXT metadata
- The media type (CD-ROM / CD-R / CD-RW)
- A clear recommendation on whether to enable subchannel extraction

**Rule of thumb:** If you are archiving and storage is not a concern, always include subchannel data — it costs ~4% extra file size and ensures nothing is lost. If you are ripping for playback only, skip it.

# Subchannel Data on Burned Audio CDs: Why R–W Is Often Empty and P+Q Gets Auto-Generated

This section explains why **R–W subchannel data is usually missing** on burned audio CDs, why **P+Q timing is commonly generated automatically**, and why the **TOC is typically more reliable** for navigation.

---

## 1) R–W Subchannel Data (Meaningful Content)

### What it is
The **R–W channels** (subchannels **3 through 8**) *can* carry meaningful data such as:

- **CD+G graphics** (commonly used for karaoke)
- **CD-Text** (in some implementations)

### Why it's often missing
Most consumer CD burning software defaults to **not writing** R–W subchannel content unless explicitly configured. Examples include:

- Nero
- ImgBurn
- iTunes
- Finder / Windows Explorer

These tools generally prioritize writing the **audio** and the **core control/timing subchannels** (especially P+Q). As a result, they often write **generic, blank, or repeating** patterns in R–W instead of real CD+G or mastered subchannel data.

### Outcome
On typical burned audio CDs:

- Players rarely use R–W during playback.
- Players rely on the **Table of Contents (TOC)** for track start/end times.
- For an audio CD that has CD-TEXT, the CD-TEXT data is commonly stored in the lead-in area (before track 1). So the location is often lead-in, but the mechanism is still subchannel R-W.

---

## 2) P+Q Timing Generation (Automatic)

### What it is
The **Q channel** contains critical low-level timing and identification info, such as:

- track numbers
- timestamps (relative/absolute time)
- track/index boundaries

### The issue
During common burn modes like:

- **TAO (Track-At-Once)**
- quick **DAO (Disc-At-Once)**

…the drive often **generates Q-subchannel codes on the fly** based on the incoming stream, rather than using a pre-authored mastering file containing precise subchannel timing.

### Result
Because timing is created in real time:

- it can be subject to small inconsistencies ("jitter")
- it's usually fine for audio playback
- but it can sometimes trip up:
  - very picky older CD players
  - certain data recovery or verification workflows that expect highly consistent subchannel timing

---

## 3) TOC vs. Q-Subchannel Reliability

### TOC (Table of Contents)
- Stored in the **Lead-In** at the beginning of the disc
- Written at the **end of the burn process**
- Fixed and generally read very reliably by drives/players
- Used to determine where tracks start and end

### P+Q timing
- Updated continuously across the burn
- A brief write/reading issue can create a **Q-channel glitch**
- This can happen even when the audio data is still recoverable/corrected via ECC

### Conclusion
- **TOC** is generally the most reliable for **disc navigation**
- **Q-subchannel** is heavily used for **playback tracking** and timing, but can be more vulnerable to momentary glitches

---

## Summary

For standard audio CD burns, these behaviors explain why:

- some players show a "CD+G" label or track name while others don't
- burned discs may take longer to load than pressed discs
- special features like CD+G graphics often don't work unless explicitly authored and written

If you need maximum compatibility for special features (especially **CD+G**), use software/hardware that supports:

- **RAW DAO (Disc-At-Once)**, and
- explicit inclusion of **R–W subchannel data**


### Output files

| Subchannel setting | Files created |
|---|---|
| **Include** | `.bin` (audio) + `.sub` (96 bytes/sector raw subchannel) + `.cue` (sheet) |
| **Audio only** | `.bin` (audio) + `.cue` (sheet) |

---

### Subchannel Integrity Verification

**Question answered:** *"Is the Q subchannel data on this disc readable, and could it affect track boundary detection?"*

The Q subchannel is a narrow metadata channel embedded alongside the audio data on every CD sector. It carries the current track number, index point, and timestamps — the information a CD player uses for its real-time display ("Track 3, 2:47"). OptiScan reads and validates this data for every audio sector on the disc.

#### Why subchannel errors are expected

Unlike audio data, subchannel data has **no error correction**:

| Property | Audio Data | Q Subchannel |
|---|---|---|
| **Error correction** | CIRC — two layers of Reed-Solomon (C1 + C2) with interleaving | None — only a 16-bit CRC for detection |
| **Interleaving** | Yes — data is spread across ~100 frames to survive scratches | No — each 96-bit frame stands alone |
| **Redundancy** | ~25% of raw channel data is parity bytes | Zero — 10 bytes payload + 2 bytes CRC |
| **On read failure** | Hardware reconstructs the original samples perfectly | Data is simply lost — no recovery possible |

A single bit flip anywhere in a 96-bit subchannel frame causes the CRC to fail. The same bit flip in the audio channel would be silently corrected by the C1/C2 error correction hardware before the data ever reaches software.

**A subchannel error rate of 1–3% is normal and expected on most CDs, even brand-new pressed discs.** This is not a defect — it reflects the physical limitations of a channel that was designed as best-effort navigational metadata, not as a reliable data transport.

#### What the scan checks

For every audio sector, OptiScan:

1. Reads the raw 96-byte interleaved subchannel data via `READ CD` (subchannel mode 01h)
2. De-interleaves the Q channel bits and validates the CRC-16-CCITT checksum
3. If the raw CRC fails, retries once (transient errors are common)
4. If raw reading fails twice, falls back to the drive's formatted Q subchannel (mode 02h)
5. If a valid Q frame is recovered, validates that the reported track number matches the expected track from the TOC

Errors are classified into three categories:

| Error type | Meaning |
|---|---|
| **CRC/Read errors** | The Q subchannel CRC failed on both raw attempts and the formatted fallback also failed — the data for this sector is unrecoverable |
| **Track mismatches** | The Q data was read and CRC-verified successfully, but the reported track number does not match the expected track from the TOC. Common at track boundaries and pregaps |
| **Index errors** | The decoded index value is outside the valid BCD range (0–99) |

The scan also tracks **burst errors** — consecutive sectors with failures — to identify localized damage versus uniformly distributed noise.

#### Interpreting results

| Error rate | Assessment |
|---|---|
| **0%** | Exceptionally clean — uncommon even on new discs |
| **< 1%** | Excellent — no impact on ripping |
| **1–3%** | Normal — typical baseline for most CDs and drives |
| **3–5%** | Elevated — may indicate disc wear, but audio extraction is unaffected |
| **5–10%** | High — disc surface may be degraded; cross-reference with C2 scan |
| **> 10%** | Severe — likely physical damage; prioritize backup with secure rip mode |

#### Why this does not affect audio quality

The subchannel and the audio data are physically separate channels on the disc. A subchannel CRC failure means the 96-bit navigational frame for that sector was unreadable — it says nothing about the 2,352-byte audio payload, which has its own independent and far more robust error correction.

OptiScan (and all modern rippers) determines track boundaries from the **Table of Contents** in the disc lead-in, not from per-sector subchannel data. The subchannel is useful for:

- Detecting index points within tracks (e.g., hidden tracks in pregaps)
- Extracting ISRC codes and MCN (Media Catalog Number)
- Verifying that the TOC and the on-disc metadata are consistent

A high subchannel error rate is a signal to inspect the disc further (run a C2 or disc rot scan), but it does not indicate that the extracted audio will contain errors.

**Output:** Sector count, per-category error totals, burst analysis, and measured read speed.

**When to use:** Before ripping discs where index point accuracy matters (live albums, gapless recordings), or as a general disc health indicator to decide whether further diagnostics are warranted.

---

## Disc Writing

OptiScan can write audio CDs from `.bin` / `.cue` / `.sub` file sets produced by a previous rip. The write workflow handles media detection, capacity verification, mode negotiation, CD-Text embedding, and session finalization automatically.

### Write Mode Negotiation

Not all drives support the same write modes. OptiScan probes the drive by testing both MODE SELECT and SEND CUE SHEET together, since some drives accept the mode page but reject the CUE sheet. The negotiation tries modes in priority order:

| Priority | Mode | Block size | Description |
|---|---|---|---|
| 1 | Raw + packed P-W | 2448 bytes | Exact 1:1 copy including subchannel data (best fidelity) |
| 2 | Raw + raw P-W | 2448 bytes | Raw subchannel format (deinterleaving handled by host) |
| 3 | DAO + packed P-W | 2448 bytes | Disc-At-Once with packed subchannel |
| 4 | DAO + raw P-W | 2448 bytes | Disc-At-Once with raw subchannel |
| 5 | Raw (no subchannel) | 2352 bytes | Raw write mode without subchannel data |
| 6 | Plain SAO | 2352 bytes | Audio only — drive generates subchannel automatically (last resort) |

If the drive silently downgrades the requested write parameters (accepts MODE SELECT but stores different values), OptiScan detects this via readback verification and rejects the mode.

### Subchannel Writing

When a `.sub` file is provided alongside the `.bin` and `.cue`, OptiScan validates it against the expected sector count and writes subchannel data interleaved with the audio. Raw P-W subchannel data is automatically deinterleaved to packed format when required by the negotiated write mode.

If the drive does not support any subchannel write mode, OptiScan falls back to plain SAO and writes audio only, with a warning that subchannel data will be omitted.

### CD-Text Embedding

If the CUE file contains `TITLE` and/or `PERFORMER` commands (at disc and/or track level), OptiScan automatically:

1. Builds CD-Text packs (pack types 0x80 Title, 0x81 Performer, 0x8F Size Information)
2. Computes CRC-16 (CRC-CCITT, inverted per Red Book) for each 18-byte pack
3. Sends the packs to the drive via WRITE BUFFER (buffer ID 0x08)

The drive embeds the CD-Text in the lead-in R-W subchannel during SAO/DAO writing. If the drive rejects the CD-Text data, the audio is still written normally.

### CD-RW Detection and Blanking

Before writing, OptiScan queries the disc via READ DISC INFORMATION (0x51) to detect:

- **Media type** — CD-R (write-once) or CD-RW (rewritable)
- **Disc status** — empty, appendable, or complete (full)

If the primary command fails (e.g., corrupted TOC), a fallback via GET CONFIGURATION (0x46) determines the media profile.

For CD-RW media that needs erasing, two blanking modes are available:

| Mode | SCSI blank type | Behavior |
|---|---|---|
| **Quick blank** | 0x01 (minimal) | Erases PMA, lead-in, and pregap only (~1–2 minutes) |
| **Full blank** | 0x00 (entire disc) | Erases the entire disc surface (~10+ minutes) |

If the standard blank fails (e.g., corrupted TOC prevents the drive from processing a normal blank), OptiScan automatically attempts a recovery blank via erase session (type 0x06), then retries the original blank. Progress is tracked via REQUEST SENSE polling with a real-time progress bar.

### Write Workflow

The full write sequence is:

1. **Media check** — verify disc is empty and writable; blank CD-RW if needed
2. **File validation** — verify `.bin`, `.cue`, and optional `.sub` files exist and are consistent
3. **Capacity check** — verify the image (including pregap and lead-out overhead) fits on the disc
4. **CUE sheet parsing** — extract track layout, pregap data, ISRC codes, MCN, and CD-Text metadata
5. **Power calibration** — optional OPC (SEND OPC INFORMATION, 0x54)
6. **Write mode negotiation** — probe drive capabilities and select best supported mode
7. **SCSI CUE sheet** — build and send the disc layout (SEND CUE SHEET, 0x5D) with Track 1 pregap at MSF 00:00:00
8. **CD-Text** — build and send packs if CUE metadata is present
9. **Audio sector writing** — write 150 sectors of pregap silence followed by BIN file data via WRITE(10), with subchannel data appended when available
10. **Finalization** — SYNCHRONIZE CACHE (0x35), CLOSE SESSION (0x5B), and lead-out polling until the drive is ready

### Post-Write Verification

After finalization, OptiScan can verify the written disc by reading back the first and last sector of each track to confirm readability.

### Write Tracks Using Source-Disc Pregaps

**Question answered:** *"Can I burn a new disc from my individual ripped tracks while keeping the original CD's track-to-track gap timing?"*

This workflow takes a folder of ripped audio files (one `.wav` or `.flac` per audio track) plus the source disc's TOC and produces a new burned CD whose pregap durations match the source disc frame-for-frame.

The flow:

1. The source disc must be in the drive when the workflow starts — its TOC supplies the pregap layout (`startLBA - pregapLBA` per track).
2. The user picks a folder; files are matched to audio tracks alphabetically. FLAC inputs are decoded to a temporary WAV via `flac.exe`.
3. Each WAV is validated as 16-bit / 44100 Hz / stereo PCM. Sector counts are checked against the source TOC; mismatches produce a confirmation prompt.
4. A temporary `.bin` is built containing `[track 1][gap][track 2][gap][track 3]…` where each gap equals the source disc's pregap for the following track. A matching `.cue` is generated with `INDEX 00` / `INDEX 01` entries reflecting the BIN-relative LBAs, plus any CD-Text and ISRC carried in the source TOC.
5. The user is prompted to eject the source disc and insert a blank CD-R/CD-RW. The drive is reopened and the standard `WriteDisc` pipeline is invoked (blanking, OPC, write-mode negotiation, CUE sheet, CD-Text, IMAPI fallback).
6. Temp files are cleaned up after the burn.

**Write-offset compensation:**

For the burned disc to verify against AccurateRip / dBpoweramp, the audio must be pre-shifted by the burner's write offset before sending it to the drive. Otherwise the drive's write offset shifts every sample on the new disc, and the resulting per-track CRCs won't match the AccurateRip database.

The workflow detects the drive's read offset (from the AccurateRip drive database) and offers four choices:

1. **Apply −(read offset)** — recommended default; correct for the common case where write offset = −read offset.
2. **Apply +(read offset)** — for drives where write and read offsets share a sign.
3. **Manual entry** — if you've calibrated the combined offset for your specific burner.
4. **No compensation (0)** — only correct for drives with a true zero combined offset.

After the burn, run **option 12 — Compare disc CRCs (Original vs. Copy)** to confirm the detected sample offset is zero. If it's non-zero, that's the residual you should add to the next burn's compensation value.

The shift is applied to the in-memory BIN sector list (peak ≈3× audio size during the shift). Shifted samples that fall off the start/end of the disc are zero-padded; for typical drive offsets (≤500 samples) this falls well inside AccurateRip's skip window (5 sectors at each disc boundary).

**Pregap audio capture:**

Because the rip workflow uses `PregapMode::Skip` and discards gap audio, the WAV files don't contain it. To preserve AccurateRip CRC accuracy on discs with non-silent pregaps (live albums, continuous mixes, DJ sets), the Write Tracks workflow re-reads each track's pregap region directly from the source disc that's still in the drive — with read-offset correction applied so the captured audio aligns sample-for-sample with the WAVs.

This matters because AccurateRip's CRC for track *i* is computed over `[startLBA[i] .. startLBA[i+1] - 1]`, which **includes** the pregap audio that lives between INDEX 00 and INDEX 01 of track *i+1*. If that region contains music (not silence) and you replace it with silence on the new disc, track *i*'s CRC won't match — even though track *i*'s own audio is bit-perfect.

For typical pop CDs all pregaps are silence and the captured audio is also silence; the result is identical. The fix only changes anything when the source disc actually had audio in pregap regions.

**Limitations:**

- Track 1's hidden-track region (HTOA) is not carried over — the user's track 1 file is taken at face value and the new disc gets only the standard 150-frame mandatory pregap before track 1.
- Write-offset compensation requires you to know your burner's write offset (or guess via −read offset). The Write Disc workflow (option 3) does not yet apply this compensation; it burns the BIN as-supplied.
- Pregap reads can fail on damaged discs; in that case the workflow falls back to silence for that track's pregap and prints a warning. The downstream track may then fail AR.

---

## Copy-Protection Detection

**Question answered:** *"Is this disc copy-protected, and what scheme is being used?"*

OptiScan performs an 8-step heuristic scan that combines structural analysis (no disc I/O) with targeted reads to detect common audio CD copy-protection mechanisms.

### Checks Performed

| Step | Check | I/O required | Severity | What it detects |
|---|---|---|---|---|
| 1 | **Illegal TOC** | No | Strong | Track numbers outside 1–99, impossibly high start LBAs (> 85 min), overlapping tracks |
| 2 | **Multi-session abuse** | No | Strong | Session count > 2 (used by MediaMax/XCP to confuse rippers) |
| 3 | **Data track presence** | No | Strong | Non-audio track in last session (rootkit installer, autorun) |
| 4 | **Pre-emphasis anomaly** | No | Weak | Pre-emphasis flag set inconsistently across tracks |
| 5 | **Track gap anomalies** | No | Weak | Non-standard gap sizes between tracks |
| 6 | **Intentional errors** | Yes | Strong | Clusters of C2 / read errors deliberately mastered onto the disc (CDS, MediaClyS) |
| 7 | **Subchannel manipulation** | Yes | Strong | Corrupted or manipulated subchannel data patterns |
| 8 | **Lead-in overread block** | Yes | Strong | Drive refuses to read lead-in area (some protections block this) |

### Verdict Logic

The scan classifies each indicator as **strong** (severity ≥ 2) or **weak** (severity < 2) and applies the following rules:

| Condition | Verdict |
|---|---|
| No indicators detected | **No protection** |
| Only weak indicators | **Unlikely** — minor anomalies, not protection |
| 1 strong, no weak | **Inconclusive** — possible but not confirmed |
| 1 strong + 1 weak | **Inconclusive** — insufficient corroborating evidence |
| ≥ 2 strong, or ≥ 1 strong + ≥ 2 weak | **Protection likely** — specific scheme identified if possible |

### Identified Schemes

When sufficient indicators are present, OptiScan attempts to identify the specific protection scheme:

| Indicator combination | Identified scheme |
|---|---|
| Data track + multi-session | MediaMax / XCP-style |
| Intentional errors + illegal TOC | Cactus Data Shield / Key2Audio-style |
| Intentional errors alone | CDS / MediaClyS-style |
| Subchannel manipulation | Subchannel-based protection |

**Output:** Per-indicator results, aggregate verdict, identified scheme (if any), and a text report saved to `protection_check.txt`.

**When to use:** Before ripping unfamiliar discs — particularly commercial releases from 2001–2007 when audio CD copy-protection was widespread. The scan helps decide whether to use secure rip mode and warns about potential extraction issues.

---

## Pre-gap Scanning

OptiScan detects pre-gaps (INDEX 00 regions) on audio CDs using a two-phase algorithm with backward refinement. Pre-gaps are the audio segments (often silent) that exist between INDEX 00 and INDEX 01 of each track.

### Why Pre-gap Detection Matters

CD subchannel data can be unreliable, with drives frequently reporting stale or incorrect index values near track boundaries. A naive sector-by-sector scan would be both slow and prone to false positives. OptiScan's approach prioritizes accuracy over speed by scanning every sector with majority-voted subchannel reads, then refining the result backward to compensate for drive latency.

### The Algorithm

#### Phase 1: Fine Scan (Precise Boundary Detection)
- **Step Size**: 1 sector (sector-by-sector)
- **Range**: Up to 450 sectors before the track's INDEX 01 boundary (Track 1 scans from LBA 0)
- **Reliability**: Uses `ReadSectorQ()` with 3-round majority voting to filter out spurious subchannel reads
- **Validation**: Requires **≥3 consecutive INDEX 0 hits** to accept a boundary
  - Isolated spurious subchannel values are rejected
  - Prevents false positives from stale Q subchannel data
- **Early Exit**: Stops scanning once INDEX 1 is detected after a confirmed boundary

#### Phase 2: Backward Refinement (Compensating for Read Displacement)
- **Range**: Checks up to **8 sectors backward** from the detected boundary
- **Purpose**: Compensate for subchannel read displacement (drives often report index changes several sectors late)
- **Method**: Probes backward sector-by-sector using majority-voted reads until a non-INDEX 0 sector is found
- **Result**: Captures the true start of the pre-gap region

### Scan Parameters

- **Drive Speed**: Reduced to 4× during pre-gap scanning for improved subchannel reliability
- **Track 1 Special Case**: Scans from LBA 0 (disc start) since Track 1 pre-gaps may begin at the very start of the disc
- **Other Tracks**: Scans up to **450 sectors backward** (~6 seconds) to accommodate:
  - Standard pre-gaps (typically 150 frames / 2 seconds)
  - Non-standard pre-gaps on live albums or gapless discs

This algorithm ensures accurate pre-gap detection across a wide variety of CD pressings and drive models, even when subchannel data quality is poor.

### Output

The detected pre-gap boundary is stored in `disc.tracks[i].pregapLBA` and reported to the user.

---

## TOC-Less Disc Scanning

**Question answered:** *"Can I still extract audio from a disc with a damaged or missing Table of Contents?"*

When `READ TOC` fails or returns illegal entries, OptiScan can reconstruct the track layout by scanning the disc directly. The algorithm is inspired by CloneCD and IsoBuster:

| Phase | Method | Purpose |
|---|---|---|
| **Phase 0** | `READ DISC INFORMATION` + `READ TRACK INFORMATION` | Firmware-level query — completes in milliseconds, no disc I/O |
| **Phase 1** | Coarse Q-subchannel scan from LBA 0 (75-sector steps) | Discover track numbers and approximate boundaries; gap-hopping survives data tracks and inter-session gaps |
| **Phase 2** | Group coarse samples by track number | Build candidate track list |
| **Phase 3** | Binary-search refinement of audio↔audio boundaries | Pinpoint exact track transitions |
| **Phase 4** | Build validated `DiscInfo` | Populate track list with validated start/end LBAs |

If the standard TOC is read successfully but contains out-of-range entries that had to be clamped, OptiScan automatically re-scans the disc using this method for more accurate boundaries.

---

## Windows APIs Used

OptiScan is a native Win32 application built directly against the Windows SDK with no third-party frameworks. The APIs it relies on are grouped below by purpose, along with what each is used for. Linked import libraries are pulled in via `#pragma comment(lib, ...)` in the relevant source files.

### Device I/O and SCSI/MMC pass-through (the core of the program)

All low-level disc access goes through a raw device handle and `DeviceIoControl`.

| API | Used for |
|---|---|
| `CreateFileW` (on `\\.\X:`) | Opens the optical drive as a raw device handle with `GENERIC_READ`/`GENERIC_WRITE`, which is the prerequisite for SCSI pass-through |
| `DeviceIoControl` | The workhorse: issues `IOCTL_SCSI_PASS_THROUGH_DIRECT` to send arbitrary SCSI/MMC CDBs (READ CD, READ TOC, MODE SENSE/SELECT, SEND CUE SHEET, WRITE, vendor commands, etc.), plus the storage and CD-ROM IOCTLs below |
| `IOCTL_SCSI_PASS_THROUGH_DIRECT` (`SCSI_PASS_THROUGH_DIRECT`) | Sends raw SCSI command descriptor blocks to the drive and retrieves sense data — every read, write, scan, and vendor command flows through this |
| `IOCTL_STORAGE_QUERY_PROPERTY` (`STORAGE_PROPERTY_QUERY`, `STORAGE_ADAPTER_DESCRIPTOR`, `STORAGE_DEVICE_DESCRIPTOR`) | Reads the bus type (USB/SATA/IDE/SCSI) for interface detection and the vendor/product strings for drive naming and USB-bridge identification |
| `IOCTL_STORAGE_EJECT_MEDIA` | Ejects the tray |
| `IOCTL_STORAGE_CHECK_VERIFY` | Polls whether media is present/ready while waiting for a disc |
| `IOCTL_CDROM_READ_TOC` (`CDROM_TOC`) | Fast firmware-level TOC read used during drive enumeration to count audio tracks |
| `CloseHandle` | Closes the drive handle |

These rely on the SDK headers `winioctl.h`, `ntddscsi.h`, and `ntddcdrm.h`.

### Drive enumeration

| API | Used for |
|---|---|
| `GetLogicalDrives` | Gets the bitmask of present drive letters |
| `GetDriveTypeW` | Filters that list down to `DRIVE_CDROM` devices |
| `GetTickCount`, `Sleep` | Timeout/poll loop while waiting for a disc to be inserted |
| `GetLastError` (`ERROR_NOT_READY`, `ERROR_MEDIA_CHANGED`, etc.) | Distinguishes "no disc yet" from real failures during media-ready polling |

### Networking — AccurateRip lookup and update check (WinHTTP / WinINet)

Linked against `winhttp.lib` and `wininet.lib`.

| API | Used for |
|---|---|
| `WinHttpOpen`, `WinHttpConnect`, `WinHttpOpenRequest`, `WinHttpSendRequest`, `WinHttpReceiveResponse` | Performs the HTTP/HTTPS request to the AccurateRip database and to the GitHub releases API |
| `WinHttpQueryHeaders` | Reads the HTTP status code (200/404/403) |
| `WinHttpQueryDataAvailable`, `WinHttpReadData` | Streams the response body (the AccurateRip `.bin` blob and the GitHub JSON) |
| `WinHttpSetTimeouts` | Bounds the request so a dead connection doesn't hang the UI |
| `WinHttpCloseHandle` | Releases WinHTTP handles |
| `InternetGetConnectedState` (WinINet) | Fast connectivity pre-check before the update lookup to fail quickly when offline |
| `ShellExecuteW` (`shellapi.h`) | Opens the download page in the user's browser when an update is available |

### GUI — window, message loop, and controls (User32 / common controls)

Linked against `comctl32.lib`.

| API | Used for |
|---|---|
| `RegisterClassExW`, `CreateWindowW`/`CreateWindowExW`, `DefWindowProc`, `ShowWindow`, `UpdateWindow`, `DestroyWindow` | Main window, custom output control, modal prompt dialog, and all child controls (buttons, static labels, the accessible EDIT, progress bar) |
| `GetMessage`, `TranslateMessage`, `DispatchMessage`, `PostMessageW`/`SendMessageW`, `PeekMessageW`, `WaitMessage`, `PostQuitMessage` | The main message loop and the prompt dialog's private modal loop; cross-thread UI updates from the worker thread |
| `TranslateAccelerator`, `LoadAccelerators`, `IsDialogMessageW` | Keyboard shortcuts (e.g. Ctrl+Shift+A) and Tab/Shift+Tab focus navigation between controls |
| `LoadIcon`, `LoadCursor`, `LoadStringW`, `GetStockObject` | Window class resources (icon, cursor, title strings, background brush) |
| `InitCommonControlsEx`, `PROGRESS_CLASS` (`PBM_SETRANGE32`/`PBM_SETPOS`/`PBM_SETBARCOLOR`) | The live progress meter |
| `CreateWindowEx` with `EDIT`/`STATIC`/`BUTTON` classes; `EM_SETSEL`, `EM_REPLACESEL`, `EM_SCROLLCARET`, `EM_SETLIMITTEXT` | The screen-reader-readable output mirror (a read-only multiline EDIT) and prompt fields |
| `BeginDeferWindowPos`/`DeferWindowPos`/`EndDeferWindowPos`, `MoveWindow`, `SetWindowPos`, `InvalidateRect`, `GetClientRect`, `GetWindowRect`, `MapWindowPoints` | Batched, flicker-free layout of the button grid and panels |
| `EnableWindow`, `SetFocus`, `GetFocus`, `IsChild`, `GetDlgCtrlID`, `GetDlgItem`, `SetWindowTextW`/`GetWindowTextW`, `GetWindowLongPtr`/`SetWindowLongPtr` | Per-control enable/disable during workflows, focus handling, and per-control state storage |
| `MessageBoxW` | Yes/No/Cancel confirmations and simple notices |
| `CreatePopupMenu`/`AppendMenuW`/`InsertMenuW`/`TrackPopupMenu`, `GetMenu`, `EnableMenuItem`, `CheckMenuItem`, `DrawMenuBar` | The categorised "Operations" menu and the output control's right-click Copy/Select-All menu |
| `OpenClipboard`/`EmptyClipboard`/`SetClipboardData`/`CloseClipboard` (`CF_UNICODETEXT`) | Copy from the output log |
| `SetTimer`/`KillTimer`, `SetCapture`/`ReleaseCapture`, `GetCursorPos`, `ScreenToClient`/`ClientToScreen` | Click-drag text selection and auto-scroll in the custom output control |
| `SetLayeredWindowAttributes` (`WS_EX_LAYERED`), `AdjustWindowRect`, `GetSystemMetrics`, `BringWindowToTop`, `SetForegroundWindow` | The translucent floating prompt dialog and its centering |

### High-DPI and multi-monitor scaling

| API | Used for |
|---|---|
| `SetProcessDpiAwarenessContext` (per-monitor-v2) | Declares DPI awareness so the UI renders crisply |
| `GetDpiForSystem`, `GetDpiForWindow`, `WM_DPICHANGED` handling | Computes and updates the UI scale factor when moved between monitors |
| `MonitorFromRect`, `GetMonitorInfoW` | Per-monitor work-area and scaling decisions |
| `SystemParametersInfoW` (`SPI_GETWORKAREA`, `SPI_GETSCREENREADER`, `SPI_GETHIGHCONTRAST`, `SPI_GETWHEELSCROLLLINES`) | Work-area sizing, screen-reader auto-detection, High Contrast detection, and mouse-wheel scroll amount |

### Graphics — GDI and GDI+

Linked against `gdiplus.lib`. The window background, panels, buttons, and the custom console output control are all custom-drawn.

| API | Used for |
|---|---|
| GDI+ (`GdiplusStartup`/`GdiplusShutdown`, `Graphics`, `Image::FromStream`, `GraphicsPath`, gradient/solid brushes, pens, fonts) | All the styled UI rendering: gradient panels, rounded rectangles, the embedded PNG artwork, the title text, and tech-accent decorations |
| GDI (`BeginPaint`/`EndPaint`, `CreateCompatibleDC`/`CreateCompatibleBitmap`, `BitBlt`, `SelectObject`, `DeleteDC`/`DeleteObject`) | Double-buffered painting to avoid flicker |
| `CreateFontW`/`CreateFontIndirectW`, `CreateSolidBrush`/`CreatePatternBrush`, `CreatePen`, `FillRect`, `RoundRect`, `DrawTextW`, `ExtTextOutW`, `SetTextColor`/`SetBkColor`/`SetBkMode` | Fonts, brushes, and text rendering in the custom output control and owner-drawn buttons |
| `GetTextExtentPoint32W`, `GetTextMetricsW`, `GetGlyphIndicesW`, `GetObjectW` | Monospaced cell metrics and per-glyph font-fallback (Cascadia Mono → Segoe UI → Segoe UI Symbol → Segoe UI Emoji) so box-drawing and symbol glyphs render |
| `WM_DRAWITEM` owner-draw (`DRAWITEMSTRUCT`) | Custom-painted command buttons and dialog buttons |

### COM — folder picker and disc-burning fallback

Linked against `ole32.lib`.

| API | Used for |
|---|---|
| `CoInitializeEx`/`CoUninitialize`, `CoCreateInstance` | COM lifetime and object creation for the dialogs and IMAPI |
| `IFileOpenDialog` (with `FOS_PICKFOLDERS`), `IShellItem`, `SHCreateItemFromParsingName`, `CoTaskMemFree` (`shobjidl.h`/`shlobj.h`) | The native Windows folder picker for choosing output/source directories |
| IMAPI2 (`IDiscMaster2`, `IDiscRecorder2`, `IRawCDImageCreator`, `IDiscFormat2RawCD`, `IDiscFormat2TrackAtOnce`) | Fallback disc-burning path (DAO, then TAO) when the drive rejects the raw SCSI CUE-sheet write |
| `IStream`/`CreateStreamOnHGlobal`, `SAFEARRAY` helpers, `VARIANT` helpers, `BSTR` (`SysAllocString`/`SysFreeString`), `IConnectionPoint`/`IConnectionPointContainer`, `CoCreateFreeThreadedMarshaler` | Feeding track audio to IMAPI as streams, enumerating recorders, and receiving burn-progress callbacks |

### Accessibility — UI Automation

Linked against `uiautomationcore.lib`.

| API | Used for |
|---|---|
| `IRawElementProviderSimple` / `UiaHostProviderFromHwnd` / `UiaReturnRawElementProvider` (`WM_GETOBJECT`) | Exposes a UIA root provider so screen readers see the window |
| `UiaRaiseNotificationEvent`, `UiaClientsAreListening` | Speaks operation start/progress (25/50/75/100%)/completion announcements through NVDA, JAWS, or Narrator |

### Audio — WinMM

Linked against `winmm.lib`.

| API | Used for |
|---|---|
| `waveOutOpen`/`waveOutClose`, `waveOutPrepareHeader`/`waveOutUnprepareHeader`, `waveOutWrite`, `waveOutReset` (`WAVEFORMATEX`, `WAVEHDR`) | Plays the synthesized menu-click sound and keeps the audio session warm with a looping-silence buffer to avoid startup latency |

### Threading and synchronization

| API | Used for |
|---|---|
| `<thread>` / `<mutex>` / `<atomic>` (std, backed by Win32 primitives) | Runs each workflow on a background worker thread so the UI stays responsive; cancellation flag and output-queue synchronization |
| `WaitForSingleObject`, `GetExitCodeProcess` | Waiting on the spawned `flac.exe` process |
| `TerminateProcess`/`GetCurrentProcess` | Last-resort force-exit on shutdown if a worker is wedged in a SCSI call |

### Process and file system

| API | Used for |
|---|---|
| `CreateProcessW` (with `CREATE_NO_WINDOW`), `CloseHandle` | Launches `flac.exe` to decode FLAC inputs to temporary WAV during the Write-Tracks workflow |
| `GetModuleFileNameW`, `GetCurrentDirectoryW` | Determines the working/output directory |
| `CreateDirectoryW`, `GetFileAttributesW`, `DeleteFileW` | Recursive output-directory creation and temp-file cleanup |
| `FindFirstFileW`/`FindNextFileW`/`FindClose` (`WIN32_FIND_DATAW`) | Enumerating ripped track files in a folder |
| `LoadLibraryW`, `GetModuleHandleW`, `GetProcAddress` | Loads `Msftedit.dll`; Wine detection via the presence of `ntdll!wine_get_version` |
| `GetEnvironmentVariableW` | Honors the `OPTISCAN_ALLOW_VM` override |

### Registry — environment detection

| API | Used for |
|---|---|
| `RegOpenKeyExW`, `RegQueryValueExW`, `RegCloseKey` | Reads SMBIOS strings under `HARDWARE\DESCRIPTION\System\BIOS` to detect virtual machines (so quality scans aren't run where SCSI pass-through is unreliable) |

### Text encoding and resources

| API | Used for |
|---|---|
| `MultiByteToWideChar` / `WideCharToMultiByte` (`CP_UTF8`) | Converts between the UTF-8 console/stream layer and the wide-character Win32 UI throughout |
| `FindResourceW`, `LoadResource`, `SizeofResource`, `LockResource`, `GlobalAlloc`/`GlobalLock`/`GlobalFree`/`GlobalUnlock` | Loads the embedded PNG artwork from the executable's resources into a GDI+ image |
| `OutputDebugStringA` | Diagnostic logging of drive speed negotiation (visible in a debugger) |

The target Windows platform is taken from the installed SDK (`SDKDDKVer.h`), and the project builds as a Unicode x64 application.

---

## Building

Requires **Visual Studio** with the **C++ Desktop Development** workload. Open `OptiScan.vcxproj` and build. There are no third-party dependencies — everything links against import libraries that ship with the Windows SDK, pulled in via `#pragma comment(lib, ...)` in the source. Those libraries are: `gdiplus.lib` and `comctl32.lib` (UI rendering and common controls), `winhttp.lib` and `wininet.lib` (AccurateRip lookups and the GitHub update check), `ole32.lib` (the folder picker and the IMAPI2 disc-burning fallback), `uiautomationcore.lib` (screen-reader announcements), and `winmm.lib` (the UI click sound).

### Optional runtime dependency: `flac.exe`

The core program has no runtime dependencies. The one exception is FLAC handling, which is delegated to the external `flac.exe` command-line encoder rather than an internal codec, so `flac.exe` must be on the system `PATH` for FLAC to work. Two workflows use it:

- **Rip tracks (option 2)** — when FLAC output is selected, each track is ripped to a temporary WAV and then encoded to FLAC via `flac.exe`. If `flac.exe` isn't found, OptiScan warns and saves the tracks as WAV instead.
- **Write tracks (option 4)** — when any source file is a FLAC, it is decoded to a temporary WAV via `flac.exe` before being burned. Here a missing `flac.exe` aborts the workflow.

WAV ripping and all other operations work without it. Install it from the [FLAC project](https://xiph.org/flac/) and ensure `flac.exe` is on `PATH` if you need FLAC support.

---

## Usage

Launch `OptiScan.exe` (from the build output or via Visual Studio). The main window appears with the button grid. Insert an audio CD into your drive — on the first button click that needs one, OptiScan auto-detects drives, opens the chosen drive, reads the TOC, queries AccurateRip, and runs the requested workflow. Operations that don't need an audio source (Write Disc) skip the audio-CD wait.

Workflows that prompt for paths use the standard **Windows folder picker**; numeric and text prompts use modal input dialogs. Click **Clear info box** at any time during a workflow to request cancellation at the next safe checkpoint.

---

## License

This project is provided as-is for personal and educational use.

## Acknowledgments

- **cdda2wav (cdrtools)** — a long-standing reference implementation for secure CD audio extraction. Its work on drive handling, offset behavior, and reliable digital audio extraction helped shape the field OptiScan builds upon.

- **cdparanoia / paranoia libraries** — influential for robust read-verification strategies and jitter correction. OptiScan's paranoid and multi-pass verification concepts draw from the reliability goals established by the paranoia toolset.

- **[AccurateRip](http://www.accuraterip.com/)** — OptiScan calculates AccurateRip V1 checksums and queries the AccurateRip online database to verify rip accuracy against submissions from other users worldwide. The AccurateRip database and protocol were created by Illustrate, the developers of dBpoweramp.

- **[Exact Audio Copy (EAC)](https://www.exactaudiocopy.de/)** by André Wiethoff — the pioneering CD ripper that defined secure ripping methodology. EAC established the practices of multi-pass sector reading, C2 error pointer detection, drive cache defeat, read offset correction, overreading into lead-in/lead-out, and paranoid-mode extraction with bit-level verification. OptiScan's secure rip implementation, error handling strategy, and overall approach to verifiable extraction are directly informed by the standards EAC set.

- **[dBpoweramp](https://www.dbpoweramp.com/)** by Illustrate — created the AccurateRip system and popularized the concept of verifying CD rips against a shared online database of checksums. dBpoweramp's approach to automatic drive offset detection, C2 error reporting, and its emphasis on making verified ripping accessible to a broad audience established industry-wide expectations for audio CD extraction software.

- **[QPXTool](https://qpxtool.sourceforge.io/)** — open-source CD/DVD quality scanning tool that documents the Plextor Q-Check (0xE9/0xEB), Pioneer WRITE/READ BUFFER (0x3B/0x3C), and LiteOn/MediaTek (0xDF) vendor command sets used for hardware-driven quality measurement. OptiScan's quality scan implementation draws from QPXTool's reverse-engineering of these vendor-specific commands.

EAC and dBpoweramp together defined the modern standard for verifiable, bit-perfect audio CD extraction. OptiScan builds on the methodology and concepts they established.
