// ============================================================================
// ProtectionCheck.cpp - Audio CD copy-protection detection
//
// Performs a series of heuristic checks against the disc's TOC, subchannel
// data, lead-in/lead-out areas, and raw read behaviour to detect common
// audio CD copy-protection schemes.
//
// Detected mechanisms include:
//   - Illegal / non-standard TOC entries
//   - Intentional C2 / read errors (CDS, MediaClyS)
//   - Data track in last session (MediaMax / XCP)
//   - Multi-session abuse (session count > 2)
//   - Pre-emphasis flag anomalies
//   - Subchannel corruption / manipulation
//   - Lead-in overread blocking
//   - Non-standard track gaps
// ============================================================================
#include "ProtectionCheck.h"
#include "ConsoleColors.h"
#include "Progress.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <filesystem>

// ── Forward declarations of individual checks ───────────────────────────────
static void CheckIllegalTOC(const DiscInfo& disc, ProtectionCheckResult& result);
static void CheckMultiSession(const DiscInfo& disc, ProtectionCheckResult& result);
static void CheckDataTrackPresence(const DiscInfo& disc, ProtectionCheckResult& result);
static void CheckPreemphasisAnomaly(const DiscInfo& disc, ProtectionCheckResult& result);
static void CheckTrackGapAnomalies(const DiscInfo& disc, ProtectionCheckResult& result);
static void CheckIntentionalErrors(OpticalDrive& copier, DiscInfo& disc,
	ProtectionCheckResult& result, int scanSpeed);
static void CheckSubchannelManipulation(OpticalDrive& copier, DiscInfo& disc,
	ProtectionCheckResult& result, int scanSpeed);
static void CheckLeadInOverread(OpticalDrive& copier, ProtectionCheckResult& result);
static void PrintProtectionReport(const ProtectionCheckResult& result);
static bool SaveProtectionReport(const ProtectionCheckResult& result,
	const std::wstring& filename);

// ═════════════════════════════════════════════════════════════════════════════
//  Public entry point
// ═════════════════════════════════════════════════════════════════════════════

bool RunProtectionCheck(OpticalDrive& copier, DiscInfo& disc,
	const std::wstring& workDir, int scanSpeed) {

	Console::Heading("\n=== Copy-Protection Check ===\n\n");
	Console::Info("Analysing disc for copy-protection mechanisms...\n\n");

	ProtectionCheckResult result;

	// ── Structural / TOC-based checks (fast, no disc I/O) ───────────────
	Console::Info("[1/8] Checking TOC structure...\n");
	CheckIllegalTOC(disc, result);

	Console::Info("[2/8] Checking session layout...\n");
	CheckMultiSession(disc, result);

	Console::Info("[3/8] Checking for data tracks...\n");
	CheckDataTrackPresence(disc, result);

	Console::Info("[4/8] Checking pre-emphasis flags...\n");
	CheckPreemphasisAnomaly(disc, result);

	Console::Info("[5/8] Checking track gap layout...\n");
	CheckTrackGapAnomalies(disc, result);

	// ── I/O-based checks (require disc reads) ───────────────────────────
	Console::Info("[6/8] Scanning for intentional errors...\n");
	CheckIntentionalErrors(copier, disc, result, scanSpeed);

	bool cancelled = g_interrupt.IsInterrupted();

	if (!cancelled) {
		Console::Info("[7/8] Checking subchannel integrity...\n");
		CheckSubchannelManipulation(copier, disc, result, scanSpeed);
		cancelled = g_interrupt.IsInterrupted();
	}

	if (!cancelled) {
		Console::Info("[8/8] Testing lead-in overread...\n");
		CheckLeadInOverread(copier, result);
	}

	if (cancelled) {
		Console::Warning("\n*** Scan interrupted - partial results shown ***\n");
	}

	// ── Aggregate verdict (runs on whatever indicators were collected) ──
	result.detectedCount = 0;
	int strongCount = 0;
	int weakCount = 0;
	for (const auto& ind : result.indicators) {
		if (ind.detected) {
			result.detectedCount++;
			if (ind.severity >= 2)
				strongCount++;
			else
				weakCount++;
		}
	}

	if (result.detectedCount == 0) {
		result.protectionLikely = false;
		result.verdict = "No copy-protection indicators detected.";
		result.protectionType = "None";
	}
	else if (strongCount == 0) {
		// Only weak/informational indicators — not enough to claim protection.
		result.protectionLikely = false;
		result.verdict = std::to_string(weakCount) +
			" minor anomaly(ies) found - unlikely to be copy-protection.";
		result.protectionType = "None (minor anomalies)";
	}
	else if (strongCount == 1 && weakCount == 0) {
		result.protectionLikely = false;
		result.verdict = "One strong indicator found but no corroborating evidence - "
			"possible copy-protection, but not conclusive.";
		result.protectionType = "Inconclusive";
	}
	// ── Fix #6: require >= 2 strong, or >= 1 strong + >= 2 weak ─────────
	// Previously any strongCount >= 1 with weakCount > 0 triggered
	// "protection likely", which was too aggressive (e.g. one strong
	// indicator from a long disc + one weak from CD-Extra layout).
	else if (strongCount >= 2 || (strongCount >= 1 && weakCount >= 2)) {
		// Sufficient corroborating evidence for a positive verdict.
		result.protectionLikely = true;

		// Try to identify specific scheme from combination of indicators.
		bool hasDataTrack = false, hasErrors = false, hasMultiSession = false;
		bool hasSubchManip = false, hasTocIllegal = false;
		for (const auto& ind : result.indicators) {
			if (!ind.detected) continue;
			if (ind.name == "Data Track Present") hasDataTrack = true;
			if (ind.name == "Intentional Errors") hasErrors = true;
			if (ind.name == "Multi-Session Abuse") hasMultiSession = true;
			if (ind.name == "Subchannel Manipulation") hasSubchManip = true;
			if (ind.name == "Illegal TOC") hasTocIllegal = true;
		}

		if (hasDataTrack && hasMultiSession)
			result.protectionType = "MediaMax / XCP-style (data session + multi-session)";
		else if (hasErrors && hasTocIllegal)
			result.protectionType = "Cactus Data Shield / Key2Audio-style (errors + illegal TOC)";
		else if (hasErrors)
			result.protectionType = "Intentional-error based (CDS / MediaClyS)";
		else if (hasSubchManip)
			result.protectionType = "Subchannel-based protection";
		else
			result.protectionType = "Unknown / custom scheme";

		result.verdict = "Multiple protection indicators detected - disc is likely copy-protected.";
	}
	else {
		// strongCount == 1 with only 1 weak indicator — not conclusive.
		result.protectionLikely = false;
		result.verdict = std::to_string(strongCount) + " strong and " +
			std::to_string(weakCount) +
			" weak indicator(s) found - insufficient evidence for copy-protection.";
		result.protectionType = "Inconclusive";
	}

	// ── Output ──────────────────────────────────────────────────────────
	PrintProtectionReport(result);

	std::wstring logPath = workDir + L"\\protection_check.txt";
	if (SaveProtectionReport(result, logPath)) {
		Console::Success("Report saved to: ");
		std::wcout << logPath << L"\n";
	}
	else {
		Console::Warning("Failed to save protection report to: ");
		std::wcout << logPath << L"\n";
	}

	return !cancelled;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Individual checks
// ═════════════════════════════════════════════════════════════════════════════

// ── 1. Illegal / out-of-spec TOC entries ────────────────────────────────────
static void CheckIllegalTOC(const DiscInfo& disc, ProtectionCheckResult& result) {
	ProtectionIndicator ind;
	ind.name = "Illegal TOC";
	ind.severity = 2;

	bool anomaly = false;
	std::string details;

	// ── Check raw (pre-clamping) TOC values first ──────────────────────
	// If the drive returned LBAs that required clamping or Full TOC
	// recovery, the original TOC was corrupt — that alone is a strong
	// indicator of copy-protection (CDS, Key2Audio, etc.) or a broken
	// drive response.  Checking only the sanitised disc.tracks would
	// miss this entirely.
	if (disc.tocRepaired) {
		anomaly = true;
		details += "Original TOC contained out-of-range LBAs that required "
			"clamping/recovery. ";
		for (const auto& raw : disc.rawTocEntries) {
			if (raw.originalStartLBA > 382499) {
				details += "Track " + std::to_string(raw.trackNumber) +
					" raw startLBA=" + std::to_string(raw.originalStartLBA) + ". ";
			}
			if (raw.originalEndLBA > 382499) {
				details += "Track " + std::to_string(raw.trackNumber) +
					" raw endLBA=" + std::to_string(raw.originalEndLBA) + ". ";
			}
		}
		if (disc.rawLeadOutLBA > 404999) {
			details += "Raw lead-out LBA=" + std::to_string(disc.rawLeadOutLBA) + ". ";
		}
		details += "This is consistent with copy-protection schemes that inject "
			"bogus LBAs into the Format 0 TOC response.";
	}
	else if (disc.tocSignCorrected) {
		// Sign-corrected LBAs are a drive firmware issue, not protection.
		// Report as informational but do NOT flag as an anomaly.
		ind.severity = 0;
		ind.detected = false;
		ind.description = "TOC contained small negative LBAs (firmware "
			"signed/unsigned bug) - automatically corrected. Not a "
			"copy-protection indicator.";
		result.indicators.push_back(ind);
		return;
	}

	// ── Also check the current (possibly recovered) values ─────────────
	for (const auto& t : disc.tracks) {
		// Track numbers outside 1-99 (Red Book limit)
		if (t.trackNumber < 1 || t.trackNumber > 99) {
			anomaly = true;
			details += "Track number " + std::to_string(t.trackNumber) +
				" is outside the Red Book range 1-99. ";
		}
		// ── Fix #4: raise start-LBA limit from 80 min to ~85 min ────
		// Skip this check for tracks already reported via raw entries —
		// the clamped value (450000) would just duplicate the raw finding.
		if (t.startLBA > 382499) {
			bool alreadyReportedRaw = false;
			if (disc.tocRepaired) {
				for (const auto& raw : disc.rawTocEntries) {
					if (raw.trackNumber == t.trackNumber &&
						(raw.originalStartLBA > 382499 || raw.originalEndLBA > 382499)) {
						alreadyReportedRaw = true;
						break;
					}
				}
			}
			if (!alreadyReportedRaw) {
				anomaly = true;
				details += "Track " + std::to_string(t.trackNumber) +
					" starts beyond 85-minute limit (LBA " +
					std::to_string(t.startLBA) + "). ";
			}
		}
		// End before start
		if (t.endLBA < t.startLBA) {
			anomaly = true;
			details += "Track " + std::to_string(t.trackNumber) +
				" has endLBA < startLBA. ";
		}
	}

	// Lead-out beyond maximum CD capacity (~90 min absolute max)
	if (disc.leadOutLBA > 404999) {
		anomaly = true;
		details += "Lead-out at LBA " + std::to_string(disc.leadOutLBA) +
			" exceeds 90-minute maximum. ";
	}

	// Overlapping tracks
	for (size_t i = 1; i < disc.tracks.size(); i++) {
		if (disc.tracks[i].startLBA <= disc.tracks[i - 1].endLBA) {
			anomaly = true;
			details += "Track " + std::to_string(disc.tracks[i].trackNumber) +
				" overlaps with track " +
				std::to_string(disc.tracks[i - 1].trackNumber) + ". ";
		}
	}

	ind.detected = anomaly;
	ind.description = anomaly
		? "Non-standard TOC entries found: " + details
		: "TOC structure is within Red Book specifications.";

	result.indicators.push_back(ind);
}

// ── 2. Multi-session abuse ──────────────────────────────────────────────────
static void CheckMultiSession(const DiscInfo& disc, ProtectionCheckResult& result) {
	ProtectionIndicator ind;
	ind.name = "Multi-Session Abuse";
	ind.severity = 1;

	// Normal audio CDs are single-session.  Enhanced CDs may have 2 sessions
	// (audio + data).  More than 2 is highly suspicious.
	if (disc.sessionCount > 2) {
		ind.detected = true;
		ind.description = "Disc has " + std::to_string(disc.sessionCount) +
			" sessions - unusual for audio CDs. Multi-session abuse is used by "
			"some protection schemes to confuse ripping software.";
	}
	else if (disc.sessionCount == 2) {
		ind.detected = false;
		ind.description = "Disc has 2 sessions (typical Enhanced CD layout).";
		ind.severity = 0;
	}
	else {
		ind.detected = false;
		ind.description = "Single-session disc (standard layout).";
	}
	result.indicators.push_back(ind);
}

// ── 3. Data track presence (XCP / MediaMax indicator) ───────────────────────
static void CheckDataTrackPresence(const DiscInfo& disc, ProtectionCheckResult& result) {
	ProtectionIndicator ind;
	ind.name = "Data Track Present";
	ind.severity = 0;  // Informational by default — Enhanced CDs are common

	int dataCount = 0;
	int audioCount = 0;
	bool lastTrackIsData = false;
	bool firstTrackIsData = false;

	for (size_t i = 0; i < disc.tracks.size(); i++) {
		if (disc.tracks[i].isAudio) {
			audioCount++;
		}
		else {
			dataCount++;
			if (i == 0) firstTrackIsData = true;
			if (i == disc.tracks.size() - 1) lastTrackIsData = true;
		}
	}

	if (dataCount > 0 && audioCount > 0) {
		ind.detected = true;
		std::string pos;
		if (firstTrackIsData && lastTrackIsData) pos = "first and last";
		else if (firstTrackIsData) pos = "first";
		else if (lastTrackIsData) pos = "last";
		else pos = "middle";

		ind.description = std::to_string(dataCount) + " data track(s) found among " +
			std::to_string(audioCount) + " audio tracks (data is the " + pos +
			" track). Data tracks on audio CDs may contain auto-run DRM software "
			"(e.g. XCP, MediaMax).";

		// Only escalate to strong if the layout matches known protection
		// patterns: data in last track AND multi-session.  A single data
		// track at the end is the normal Enhanced CD (CD-Extra) layout.
		if (lastTrackIsData && disc.sessionCount >= 2 && firstTrackIsData)
			ind.severity = 2;   // Strong — data bookending audio + multi-session
		// ── Fix #5: do not flag standard CD-Extra layout ────────────
		// A data track as the last track in a 2-session disc is the
		// textbook CD-Extra (Enhanced CD) format, used by thousands of
		// commercial releases.  Previously this was severity 1, which
		// combined with any strong indicator pushed the verdict to
		// "protection likely".
		else if (lastTrackIsData && disc.sessionCount >= 2) {
			ind.detected = false;
			ind.severity = 0;
			ind.description += " Standard CD-Extra (Enhanced CD) layout - not suspicious.";
		}
		else
			ind.severity = 0;   // Likely a normal Enhanced CD
	}
	else {
		ind.detected = false;
		ind.description = dataCount == 0
			? "No data tracks found (pure audio disc)."
			: "Disc contains only data tracks (not an audio CD).";
	}
	result.indicators.push_back(ind);
}

// ── 4. Pre-emphasis flag anomalies ──────────────────────────────────────────
static void CheckPreemphasisAnomaly(const DiscInfo& disc, ProtectionCheckResult& result) {
	ProtectionIndicator ind;
	ind.name = "Pre-Emphasis Anomaly";
	ind.severity = 0;  // Informational — inconsistent pre-emphasis is rare but legitimate

	int preCount = 0;
	int audioTotal = 0;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		audioTotal++;
		if (t.hasPreemphasis) preCount++;
	}

	// Pre-emphasis on only some tracks is unusual but was a legitimate
	// mastering choice on some albums.  Only flag it as a weak warning.
	if (audioTotal > 1 && preCount > 0 && preCount < audioTotal) {
		ind.detected = true;
		ind.severity = 0;   // Was 1 — downgraded to informational
		ind.description = "Pre-emphasis is set on " + std::to_string(preCount) +
			" of " + std::to_string(audioTotal) +
			" audio tracks. This is unusual but can be a legitimate mastering choice.";
	}
	else {
		ind.detected = false;
		ind.description = preCount == 0
			? "No pre-emphasis flags set (normal)."
			: "All audio tracks have pre-emphasis set (legitimate mastering choice).";
	}
	result.indicators.push_back(ind);
}

// ── 5. Non-standard track gaps ──────────────────────────────────────────────
static void CheckTrackGapAnomalies(const DiscInfo& disc, ProtectionCheckResult& result) {
	ProtectionIndicator ind;
	ind.name = "Track Gap Anomaly";
	ind.severity = 1;

	// If the TOC was repaired, track boundaries are derived from recovered
	// data — gap calculations may not reflect the real disc layout.
	if (disc.tocRepaired) {
		ind.detected = false;
		ind.description = "Track gap analysis skipped - TOC was repaired from "
			"Full TOC data; gap values may be unreliable.";
		ind.severity = 0;
		result.indicators.push_back(ind);
		return;
	}

	bool anomaly = false;
	std::string details;
	int hugeGapCount = 0;
	// ── Fix #1: raise gap threshold from 10 s to 30 s ───────────────
	// Live albums, classical recordings, and discs with hidden tracks
	// commonly have gaps exceeding 10 seconds.  True protection schemes
	// that abuse gaps typically insert multi-minute silent zones.
	constexpr DWORD HUGE_GAP_THRESHOLD = 2250;  // 30 seconds at 75 sectors/sec

	for (size_t i = 1; i < disc.tracks.size(); i++) {
		if (!disc.tracks[i].isAudio || !disc.tracks[i - 1].isAudio) continue;
		DWORD start = disc.tracks[i].startLBA;
		DWORD prevEnd = disc.tracks[i - 1].endLBA;
		if (prevEnd >= UINT32_MAX || start <= prevEnd + 1) continue;  // Adjacent, overlapping, or overflow
		DWORD gap = start - prevEnd - 1;
		if (gap > HUGE_GAP_THRESHOLD) {
			hugeGapCount++;
			details += "Gap before track " +
				std::to_string(disc.tracks[i].trackNumber) +
				" is " + std::to_string(gap / 75) + " seconds. ";
		}
	}

	if (hugeGapCount > 0) {
		anomaly = true;
		details = std::to_string(hugeGapCount) +
			" unusually large gap(s) detected. " + details +
			"Large gaps can hide data or confuse ripping software.";
	}

	ind.detected = anomaly;
	ind.description = anomaly ? details : "Track gaps are within normal range.";
	result.indicators.push_back(ind);
}

// ── 6. Intentional read errors (CDS, MediaClyS, Key2Audio) ─────────────────
static void CheckIntentionalErrors(OpticalDrive& copier, DiscInfo& disc,
	ProtectionCheckResult& result, int scanSpeed) {
	ProtectionIndicator ind;
	ind.name = "Intentional Errors";
	ind.severity = 2;

	if (scanSpeed > 0) {
		copier.GetDriveRef().SetSpeed(scanSpeed);

		WORD actualRead = 0, actualWrite = 0;
		if (!copier.GetDriveRef().GetActualSpeed(actualRead, actualWrite)) {
			Console::Warning("Drive did not report actual speed after SetSpeed; continuing.\n");
		}
	}

	// Sample a spread of sectors across the disc.  Protection schemes
	// typically plant errors in specific regions (often at the beginning
	// of each track or in a dedicated protection zone).
	struct SamplePoint { DWORD lba; bool failed; int c2; };
	std::vector<SamplePoint> samples;

	// Build sample list: first 5 + last 5 sectors of every audio track,
	// plus 3 evenly-spaced sectors in the middle.
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		if (t.endLBA < t.startLBA) continue;  // Corrupt track — skip sampling
		DWORD len = t.endLBA - t.startLBA + 1;
		for (DWORD off = 0; off < 5 && off < len; off++)
			samples.push_back({ t.startLBA + off, false, 0 });
		for (DWORD off = 0; off < 5 && off < len; off++)
			samples.push_back({ t.endLBA - off, false, 0 });
		for (int m = 1; m <= 3; m++) {
			DWORD mid = t.startLBA + (len * m) / 4;
			samples.push_back({ mid, false, 0 });
		}
	}

	// Also sample sectors in the pre-gap region of track 1.
	// Only include pre-gap if the disc has a suspiciously large pre-gap,
	// since normal pre-gap sectors commonly have read errors.
	const TrackInfo* track1 = nullptr;
	for (const auto& t : disc.tracks) {
		if (t.trackNumber == 1) {
			track1 = &t;
			break;
		}
	}
	if (track1 == nullptr) {
		for (const auto& t : disc.tracks) {
			if (!t.isAudio) continue;
			if (track1 == nullptr || t.startLBA < track1->startLBA)
				track1 = &t;
		}
	}
	if (track1 != nullptr && track1->startLBA >= 300) {
		for (DWORD lba = track1->startLBA - 150;
			lba < track1->startLBA; lba += 10)
			samples.push_back({ lba, false, 0 });
	}

	// De-duplicate and sort.
	std::sort(samples.begin(), samples.end(),
		[](const SamplePoint& a, const SamplePoint& b) { return a.lba < b.lba; });
	samples.erase(std::unique(samples.begin(), samples.end(),
		[](const SamplePoint& a, const SamplePoint& b) { return a.lba == b.lba; }),
		samples.end());

	if (samples.empty()) {
		ind.detected = false;
		ind.severity = 0;
		ind.description = "Intentional error scan skipped: no valid audio sectors available to sample.";
		result.indicators.push_back(ind);
		return;
	}

	ProgressIndicator prog;
	prog.SetLabel("Error scan");
	prog.Start();

	int failCount = 0;
	int c2Total = 0;
	int total = static_cast<int>(samples.size());
	std::vector<BYTE> audio(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> sub(SUBCHANNEL_SIZE);

	for (int i = 0; i < total; i++) {
		if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
			g_interrupt.SetInterrupted(true);
			break;
		}

		int c2Errors = 0;
		bool ok = copier.GetDriveRef().ReadSectorWithC2(
			samples[i].lba, audio.data(), sub.data(), c2Errors);
		if (!ok) {
			ok = copier.GetDriveRef().ReadSector(
				samples[i].lba, audio.data(), sub.data());
			if (!ok) {
				samples[i].failed = true;
				failCount++;
			}
		}
		samples[i].c2 = c2Errors;
		c2Total += c2Errors;

		prog.Update(i + 1, total);
	}
	prog.Finish(!g_interrupt.IsInterrupted());

	double failRate = total > 0
		? static_cast<double>(failCount) / total : 0.0;

	// Check if hard read failures (not just C2) are concentrated at track
	// starts.  Minor C2 errors at track boundaries are normal on most discs
	// and should not be treated as intentional protection errors.
	int trackStartFailures = 0;
	int trackStartSamples = 0;
	for (const auto& s : samples) {
		bool isTrackStart = false;
		for (const auto& t : disc.tracks) {
			if (s.lba >= t.startLBA && s.lba < t.startLBA + 5) {
				isTrackStart = true;
				break;
			}
		}
		if (isTrackStart) {
			trackStartSamples++;
			if (s.failed) trackStartFailures++;
		}
	}

	bool concentrated = (trackStartSamples > 0 &&
		static_cast<double>(trackStartFailures) / trackStartSamples > 0.5);

	// ── Fix #3: require minimum absolute failures in concentrated path ──
	// Previously `concentrated && failCount > 0` could fire on a single
	// scratched sector at a track boundary (failCount == 1 out of 1
	// track-start sample = 100% concentration).  True CDS/Key2Audio
	// schemes produce errors across many track starts, so require at
	// least 5 hard failures before concluding intentional errors.
	if (failRate > 0.10 || (concentrated && failCount >= 5)) {
		ind.detected = true;
		std::string msg = std::to_string(failCount) + " of " +
			std::to_string(total) + " sampled sectors failed to read (" +
			std::to_string(static_cast<int>(failRate * 100)) + "%).";
		if (concentrated) {
			msg += " Errors are concentrated at track boundaries - consistent "
				"with Cactus Data Shield or Key2Audio.";
		}
		if (c2Total > 0) {
			msg += " Total C2 errors across samples: " +
				std::to_string(c2Total) + ".";
		}
		ind.description = msg;
	}
	else {
		ind.detected = false;
		ind.description = "No intentional read errors detected across " +
			std::to_string(total) + " sampled sectors.";
		if (c2Total > 0) {
			ind.description += " (minor C2 errors: " + std::to_string(c2Total) +
				" - normal for disc media)";
		}
	}
	result.indicators.push_back(ind);
}

// ── 7. Subchannel manipulation ──────────────────────────────────────────────
static void CheckSubchannelManipulation(OpticalDrive& copier, DiscInfo& disc,
	ProtectionCheckResult& result, int scanSpeed) {
	ProtectionIndicator ind;
	ind.name = "Subchannel Manipulation";
	ind.severity = 1;

	int sampleCount = 50;
	DWORD totalSectors = disc.leadOutLBA > 0 ? disc.leadOutLBA : 0;
	if (totalSectors == 0 && !disc.tracks.empty())
		totalSectors = disc.tracks.back().endLBA;

	if (totalSectors < static_cast<DWORD>(sampleCount)) {
		ind.detected = false;
		ind.description = "Disc too short to sample subchannel data meaningfully.";
		result.indicators.push_back(ind);
		return;
	}
	if (disc.tracks.empty()) {
		ind.detected = false;
		ind.description = "No tracks available to sample subchannel data.";
		result.indicators.push_back(ind);
		return;
	}

	DWORD step = totalSectors / sampleCount;
	if (step == 0) step = 1;

	int crcFails = 0;
	int readFails = 0;
	int msfJumps = 0;
	int sampled = 0;
	DWORD prevAbsMSF = 0;

	std::vector<BYTE> audio(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> sub(SUBCHANNEL_SIZE);

	for (DWORD lba = disc.tracks.front().startLBA;
		lba < totalSectors && sampled < sampleCount; lba += step) {
		if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
			g_interrupt.SetInterrupted(true);
			break;
		}

		if (!copier.GetDriveRef().ReadSector(lba, audio.data(), sub.data())) {
			readFails++;   // Track read failures separately — not subchannel issues
			sampled++;
			continue;
		}

		// ── De-interleave Q channel from raw subchannel data ────────
		// ReadSector returns raw interleaved subchannel (cdb[10]=0x01):
		// each of the 96 bytes carries one bit per channel (bit 6 = Q).
		BYTE qChannel[12] = {};
		for (int i = 0; i < 96; i++) {
			int byteIdx = i / 8;
			int bitIdx = 7 - (i % 8);
			if (sub[i] & 0x40)
				qChannel[byteIdx] |= (1 << bitIdx);
		}

		uint16_t storedCrc = (static_cast<uint16_t>(qChannel[10]) << 8) | qChannel[11];
		uint16_t calcCrc = SubchannelCRC16(qChannel, 10);
		if (storedCrc != static_cast<uint16_t>(~calcCrc))
			crcFails++;

		// Check absolute MSF progression (bytes 7-9 in Q).
		DWORD absMin = BcdToBin(qChannel[7]);
		DWORD absSec = BcdToBin(qChannel[8]);
		DWORD absFrm = BcdToBin(qChannel[9]);

		// Skip MSF comparison if BCD values are clearly invalid
		if (absSec >= 60 || absFrm >= 75) {
			prevAbsMSF = 0;
			sampled++;
			continue;
		}

		DWORD absMSF = (absMin * 60 + absSec) * 75 + absFrm;

		if (prevAbsMSF > 0 && absMSF > 0) {
			DWORD expected = step;
			DWORD diff = (absMSF > prevAbsMSF)
				? absMSF - prevAbsMSF : prevAbsMSF - absMSF;
			if (diff > expected * 2 + 150)
				msfJumps++;
		}
		prevAbsMSF = absMSF;
		sampled++;
	}

	// Only count sectors that were actually read for the CRC failure rate.
	int readable = sampled - readFails;
	double crcFailRate = readable > 0
		? static_cast<double>(crcFails) / readable : 0.0;

	// Raise the threshold: many drives have ~30-40% subchannel CRC errors
	// under normal conditions due to poor subchannel ECC.  True manipulation
	// typically shows >50% corruption plus MSF jumps.
	if (crcFailRate > 0.50 && msfJumps > 5) {
		ind.detected = true;
		ind.severity = 2;   // Strong — both CRC and MSF anomalies
		ind.description = std::to_string(crcFails) + " of " +
			std::to_string(readable) +
			" readable sectors have invalid Q-channel CRC (" +
			std::to_string(static_cast<int>(crcFailRate * 100)) + "%). " +
			std::to_string(msfJumps) +
			" unexpected MSF jumps detected. "
			"This is consistent with subchannel-based copy protection.";
	}
	// ── Fix #2: require both CRC failures AND MSF jumps ─────────────
	// Previously `crcFailRate > 0.50 || msfJumps > 5` could trigger on
	// drives with poor subchannel reads (high CRC failures alone) or on
	// discs with BCD rounding artefacts (MSF jumps alone).  Now both
	// signals must be present for even a weak detection.
	else if (crcFailRate > 0.50 && msfJumps > 3) {
		ind.detected = true;
		ind.severity = 1;   // Weak — moderate evidence
		ind.description = std::to_string(crcFails) + " of " +
			std::to_string(readable) +
			" readable sectors have invalid Q-channel CRC (" +
			std::to_string(static_cast<int>(crcFailRate * 100)) + "%). " +
			std::to_string(msfJumps) +
			" unexpected MSF jumps. Possibly poor subchannel reading by the drive.";
	}
	else {
		ind.detected = false;
		ind.description = "Subchannel data appears consistent (" +
			std::to_string(crcFails) + " CRC failures in " +
			std::to_string(readable) + " readable samples).";
	}
	result.indicators.push_back(ind);
}

// ── 8. Lead-in overread blocking ────────────────────────────────────────────
static void CheckLeadInOverread(OpticalDrive& copier, ProtectionCheckResult& result) {
	ProtectionIndicator ind;
	ind.name = "Lead-In Overread Block";
	ind.severity = 1;

	bool canOverread = copier.GetDriveRef().TestOverread(/*leadIn=*/true);

	if (!canOverread) {
		DriveCapabilities caps;
		bool hasCaps = copier.DetectDriveCapabilities(caps);

		if (hasCaps && caps.supportsOverreadLeadIn) {
			ind.detected = true;
			ind.description =
				"Drive supports lead-in overread, but the disc blocks it. "
				"This may indicate protection that prevents offset-correction "
				"reads into the lead-in area.";
		}
		else {
			ind.detected = false;
			ind.description =
				"Lead-in overread not available (drive does not support it).";
			ind.severity = 0;
		}
	}
	else {
		ind.detected = false;
		ind.description = "Lead-in overread succeeded - no blocking detected.";
	}
	result.indicators.push_back(ind);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Reporting
// ═════════════════════════════════════════════════════════════════════════════

static void PrintProtectionReport(const ProtectionCheckResult& result) {
	Console::Heading("\n=== Protection Check Results ===\n\n");

	for (const auto& ind : result.indicators) {
		// Icon: check / warning / cross
		if (!ind.detected) {
			Console::Success("  [PASS] ");
		}
		else if (ind.severity <= 1) {
			Console::Warning("  [WARN] ");
		}
		else {
			Console::Error("  [FAIL] ");
		}

		Console::SetColor(Console::Color::Cyan);
		std::cout << ind.name;
		Console::Reset();
		std::cout << "\n         " << ind.description << "\n\n";
	}

	Console::BoxHeading("Verdict");
	if (result.protectionLikely) {
		Console::Warning("  Protection likely: ");
		std::cout << result.protectionType << "\n";
		Console::Warning("  ");
		std::cout << result.verdict << "\n";
	}
	else {
		Console::Success("  ");
		std::cout << result.verdict << "\n";
	}
	std::cout << "  Indicators triggered: " << result.detectedCount
		<< " / " << result.indicators.size() << "\n";
	Console::BoxFooter();
}

static bool SaveProtectionReport(const ProtectionCheckResult& result,
	const std::wstring& filename) {
	std::ofstream f(std::filesystem::path(filename), std::ios::out | std::ios::trunc);
	if (!f) return false;

	f << "=== Audio CD Copy-Protection Check Report ===\n\n";
	for (const auto& ind : result.indicators) {
		f << (ind.detected ? "[DETECTED] " : "[OK]       ") << ind.name << "\n";
		f << "           " << ind.description << "\n\n";
	}
	f << "--- Verdict ---\n";
	f << "Protection likely : " << (result.protectionLikely ? "YES" : "NO") << "\n";
	f << "Protection type   : " << result.protectionType << "\n";
	f << "Summary           : " << result.verdict << "\n";
	f << "Indicators hit    : " << result.detectedCount << " / "
		<< result.indicators.size() << "\n";
	f.flush();
	return f.good();
}
