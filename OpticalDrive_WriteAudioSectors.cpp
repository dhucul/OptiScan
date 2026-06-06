#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleColors.h"
#include "Progress.h"
#include "InterruptHandler.h"
#include "WriteDiscInternal.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <vector>
#include <windows.h>

// ============================================================================
// WriteAudioSectors - Write pregap silence + audio/data sectors (+ optional subchannel)
//
// When a .sub file is provided, subchannel data is read from it.  Otherwise,
// if the write mode includes subchannel, Q-channel data is synthesized from
// the CUE sheet track list, injecting MCN and ISRC at the Red Book intervals
// (every 100 sectors at positions 99 and 49 respectively).
//
// Mixed-mode discs are supported: data tracks (MODE1/2352, MODE2/2352) are
// written from the same BIN file as raw 2352-byte sectors.  The Q-channel CTL
// field is set to 0x04 for data tracks and 0x00 for audio tracks.
// ============================================================================
bool OpticalDrive::WriteAudioSectors(const std::wstring& binFile,
	const std::wstring& subFile,
	const std::vector<TrackWriteInfo>& tracks,
	DWORD totalSectors,
	bool hasSubchannel,
	bool needsDeinterleave,
	int subchannelMode,
	const std::string& discMCN) {

	std::ifstream binInput(binFile, std::ios::binary);
	if (!binInput.is_open()) {
		Console::Error("Cannot open binary file\n");
		return false;
	}

	// Open .sub file if provided; otherwise synthesize Q-channel from CUE data
	bool hasSubFile = false;
	std::ifstream subInput;
	if (hasSubchannel && !subFile.empty()) {
		subInput.open(subFile, std::ios::binary);
		if (subInput.is_open()) {
			hasSubFile = true;
		}
		else {
			Console::Warning("Cannot open .sub file -- synthesizing Q-channel from CUE metadata\n");
		}
	}

	// Drive expects raw interleaved P-W (modes 2 and 4) vs packed (modes 1 and 3)
	bool driveWantsRaw = (subchannelMode == 2 || subchannelMode == 4);

	const DWORD sectorSize = hasSubchannel ? RAW_SECTOR_SIZE : AUDIO_SECTOR_SIZE;

	constexpr DWORD PREGAP_SECTORS = 150;
	DWORD writeTotalSectors = PREGAP_SECTORS + totalSectors;

	// Detect mixed-mode disc
	bool isMixedMode = std::any_of(tracks.begin(), tracks.end(),
		[](const TrackWriteInfo& t) { return !t.isAudio; });

	Console::Info("Binary file size: ");
	long long fileSize = static_cast<long long>(totalSectors) * AUDIO_SECTOR_SIZE;
	std::cout << (fileSize / (1024 * 1024)) << " MB (" << totalSectors << " sectors)\n";
	Console::Info("Total write: ");
	std::cout << writeTotalSectors << " sectors (150 pregap + " << totalSectors
		<< (isMixedMode ? " data+audio" : " audio") << ")\n";

	if (hasSubchannel) {
		Console::Info("Write mode: 2448 bytes/sector (2352 ");
		std::cout << (isMixedMode ? "data/audio" : "audio") << " + 96 subchannel";
		if (hasSubFile && needsDeinterleave) std::cout << ", deinterleaving";
		if (!hasSubFile) std::cout << ", synthesized Q-channel";
		if (!discMCN.empty()) std::cout << ", MCN: " << discMCN;
		std::cout << ")\n";
	}
	else {
		// subchannelMode 0 is the SAO (write type 02h) path; the old mode-5 "Raw
		// without subchannel" was removed, so this branch is always SAO now.
		Console::Info("Write mode: SAO (2352 bytes/sector, drive-generated subchannel)\n");
	}

	ProgressIndicator progress(35);
	progress.SetLabel("Writing");
	progress.Start();
	// Paint the bar at 0% immediately so there's a visible sign of life before
	// the final readiness wait or first batch completes -- otherwise a slow
	// drive looks like a total freeze with no output at all.
	progress.Update(0, static_cast<int>(writeTotalSectors));
	std::cout << "\n";

	Console::Info("Waiting for drive write readiness...\n");
	if (!WriteDiscInternal::WaitForDriveReady(m_drive, 10)) {
		Console::Warning("Drive not ready after CUE sheet (attempting write anyway)\n");
	}

	// No-progress watchdog: if not a single sector commits for this long, the
	// drive has accepted a write mode it can't actually perform (seen on the
	// Pioneer BDR-S13U with P-W subchannel: MODE SELECT + cue sheet are accepted,
	// but every WRITE silently reports "becoming ready" and never completes).
	// Bound the hang instead of looping forever with no output.
	constexpr int64_t kNoProgressAbortMs = 90000;  // 90s with zero sectors written
	auto lastProgressTime = std::chrono::steady_clock::now();

	const DWORD SECTORS_PER_WRITE = hasSubchannel ? 20 : 27;
	std::vector<BYTE> writeBuffer(sectorSize * SECTORS_PER_WRITE);
	BYTE rawSub[SUBCHANNEL_SIZE];

	DWORD sectorsWritten = 0;
	int32_t currentLBA = -150;
	int consecutiveErrors = 0;
	size_t currentTrackIdx = 0;

	// Track-boundary clamp: strict SAO drives (e.g. the Pioneer BDR-S13U) reject
	// a WRITE(10) that spans a track boundary with COMMAND SEQUENCE ERROR
	// (KEY=05 ASC=2C ASCQ=00). At each track transition the drive must update the
	// Q-channel track number and insert the run-out/run-in, which it can only do
	// on a WRITE-command boundary. Build the list of LBAs where the track number
	// changes -- the track's pregap (INDEX 00) when it has one, otherwise its
	// INDEX 01 start -- in absolute-LBA coordinates (LBA 0 == first bin sector),
	// then clamp each batch so a single WRITE never crosses one.
	std::vector<int32_t> trackBoundaries;
	trackBoundaries.reserve(tracks.size());
	for (size_t i = 0; i < tracks.size(); i++) {
		int32_t boundary = static_cast<int32_t>(tracks[i].startLBA);
		if (i > 0 && tracks[i].hasPregap && tracks[i].pregapLBA < tracks[i].startLBA) {
			boundary = static_cast<int32_t>(tracks[i].pregapLBA);
		}
		trackBoundaries.push_back(boundary);
	}
	std::sort(trackBoundaries.begin(), trackBoundaries.end());

	while (sectorsWritten < writeTotalSectors) {
		if (InterruptHandler::Instance().IsInterrupted()) {
			Console::Error("\nWrite operation cancelled by user\n");
			progress.Finish(false);
			binInput.close();
			if (hasSubFile) subInput.close();
			return false;
		}

		DWORD remaining = writeTotalSectors - sectorsWritten;
		DWORD batchSize = (remaining < SECTORS_PER_WRITE) ? remaining : SECTORS_PER_WRITE;

		// Clamp the batch so it ends exactly on the next track boundary; a single
		// WRITE(10) must never span a track-number change (see trackBoundaries).
		// boundaries are sorted ascending, so the first one greater than the
		// current position is the nearest upcoming transition.
		for (int32_t boundary : trackBoundaries) {
			if (boundary > currentLBA) {
				DWORD toBoundary = static_cast<DWORD>(boundary - currentLBA);
				if (toBoundary < batchSize) batchSize = toBoundary;
				break;
			}
		}

		for (DWORD s = 0; s < batchSize; s++) {
			BYTE* dest = writeBuffer.data() + s * sectorSize;
			DWORD globalSector = sectorsWritten + s;

			if (globalSector < PREGAP_SECTORS) {
				// ── Pregap sector (LBA -150 to -1) ──────────────────────
				memset(dest, 0x00, sectorSize);

				if (hasSubchannel) {
					BYTE* subDest = dest + AUDIO_SECTOR_SIZE;

					int relFrame = static_cast<int>(PREGAP_SECTORS) - 1 - static_cast<int>(globalSector);
					int absFrame = static_cast<int>(globalSector);

					// Pregap CTL matches the first track type
					BYTE pregapCtl = tracks[0].isAudio ? 0x00 : 0x04;

					BYTE q12[12];
					BuildPositionQ(q12, pregapCtl, 1, 0,
						static_cast<BYTE>(relFrame / (75 * 60)),
						static_cast<BYTE>((relFrame / 75) % 60),
						static_cast<BYTE>(relFrame % 75),
						static_cast<BYTE>(absFrame / (75 * 60)),
						static_cast<BYTE>((absFrame / 75) % 60),
						static_cast<BYTE>(absFrame % 75));

					BuildPackedSubchannel(subDest, q12, true);  // P=1 for pregap

					if (driveWantsRaw) {
						BYTE raw96[SUBCHANNEL_SIZE];
						InterleaveSubchannel(subDest, raw96);
						memcpy(subDest, raw96, SUBCHANNEL_SIZE);
					}
				}
			}
			else {
				// ── Data/audio sector (LBA 0+) ──────────────────────────
				binInput.read(reinterpret_cast<char*>(dest), AUDIO_SECTOR_SIZE);
				size_t audioRead = binInput.gcount();
				if (audioRead < AUDIO_SECTOR_SIZE) {
					std::fill(dest + audioRead, dest + AUDIO_SECTOR_SIZE, 0x00);
				}

				if (hasSubchannel) {
					BYTE* subDest = dest + AUDIO_SECTOR_SIZE;

					if (hasSubFile) {
						// ── Read subchannel from .sub file ──────────────
						if (needsDeinterleave) {
							subInput.read(reinterpret_cast<char*>(rawSub), SUBCHANNEL_SIZE);
							size_t subRead = subInput.gcount();
							if (subRead < SUBCHANNEL_SIZE) {
								std::fill(rawSub + subRead, rawSub + SUBCHANNEL_SIZE, 0x00);
							}
							WriteDiscInternal::DeinterleaveSubchannel(rawSub, subDest);
						}
						else {
							subInput.read(reinterpret_cast<char*>(subDest), SUBCHANNEL_SIZE);
							size_t subRead = subInput.gcount();
							if (subRead < SUBCHANNEL_SIZE) {
								std::fill(subDest + subRead, subDest + SUBCHANNEL_SIZE, 0x00);
							}
						}
					}
					else {
						// ── Synthesize Q-channel from CUE metadata ──────
						DWORD binSector = globalSector - PREGAP_SECTORS;

						bool isInPregap = false;
						size_t trkIdx = WriteDiscInternal::FindTrackForSector(tracks, binSector, isInPregap);

						BYTE trackNo = static_cast<BYTE>(tracks[trkIdx].trackNumber);
						BYTE indexNo = isInPregap ? 0 : 1;

						// CTL: 0x00 for audio tracks, 0x04 for data tracks
						BYTE ctl = tracks[trkIdx].isAudio ? 0x00 : 0x04;

						// Relative MSF (counts down in pregap, up in index 1)
						int relFrames;
						if (isInPregap) {
							relFrames = static_cast<int>(tracks[trkIdx].startLBA) - 1
								- static_cast<int>(binSector);
						}
						else {
							relFrames = static_cast<int>(binSector)
								- static_cast<int>(tracks[trkIdx].startLBA);
						}

						// Absolute MSF (LBA 0 = 00:02:00)
						int absFrames = static_cast<int>(binSector) + 150;

						BYTE absM = static_cast<BYTE>(absFrames / (75 * 60));
						BYTE absS = static_cast<BYTE>((absFrames / 75) % 60);
						BYTE absF = static_cast<BYTE>(absFrames % 75);
						BYTE relM = static_cast<BYTE>(relFrames / (75 * 60));
						BYTE relS = static_cast<BYTE>((relFrames / 75) % 60);
						BYTE relF = static_cast<BYTE>(relFrames % 75);

						BYTE q12[12];

						// Avoid replacing position Q at track/index transitions
						// so the player always sees the boundary change
						bool atTrackBoundary = (binSector == tracks[trkIdx].startLBA)
							|| (tracks[trkIdx].hasPregap && binSector == tracks[trkIdx].pregapLBA);

						// MCN and ISRC are only valid for audio tracks
						bool canInjectMCN = tracks[trkIdx].isAudio && !discMCN.empty()
							&& discMCN.size() >= 13;
						bool canInjectISRC = tracks[trkIdx].isAudio
							&& !tracks[trkIdx].isrcCode.empty();

						// MCN (Mode-2) at sector % 100 == 99
						// ISRC (Mode-3) at sector % 100 == 49
						if (!atTrackBoundary && canInjectMCN
							&& (binSector % 100 == 99)) {
							EncodeMCN(q12, discMCN.c_str(), absF);
						}
						else if (!atTrackBoundary && canInjectISRC
							&& (binSector % 100 == 49)) {
							EncodeISRC(q12, tracks[trkIdx].isrcCode.c_str(), absF);
						}
						else {
							BuildPositionQ(q12, ctl, trackNo, indexNo,
								relM, relS, relF, absM, absS, absF);
						}

						BuildPackedSubchannel(subDest, q12, isInPregap);

						if (driveWantsRaw) {
							BYTE raw96[SUBCHANNEL_SIZE];
							InterleaveSubchannel(subDest, raw96);
							memcpy(subDest, raw96, SUBCHANNEL_SIZE);
						}
					}
				}
			}
		}

		DWORD transferBytes = batchSize * sectorSize;

		BYTE writeCmd[10] = { 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		DWORD lbaUnsigned = static_cast<DWORD>(currentLBA);
		writeCmd[2] = static_cast<BYTE>((lbaUnsigned >> 24) & 0xFF);
		writeCmd[3] = static_cast<BYTE>((lbaUnsigned >> 16) & 0xFF);
		writeCmd[4] = static_cast<BYTE>((lbaUnsigned >> 8) & 0xFF);
		writeCmd[5] = static_cast<BYTE>(lbaUnsigned & 0xFF);
		writeCmd[7] = static_cast<BYTE>((batchSize >> 8) & 0xFF);
		writeCmd[8] = static_cast<BYTE>(batchSize & 0xFF);

		BYTE senseKey = 0, asc = 0, ascq = 0;
		bool writeOk = m_drive.SendSCSIWithSense(writeCmd, sizeof(writeCmd),
			writeBuffer.data(), transferBytes, &senseKey, &asc, &ascq, false);

		if (!writeOk) {
			// Watchdog: bound a silent stall. Forward progress (a committed batch)
			// refreshes lastProgressTime, so this only fires when nothing at all is
			// being written -- i.e. the drive can't really do this write mode.
			auto noProgressMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - lastProgressTime).count();
			if (noProgressMs > kNoProgressAbortMs) {
				Console::Error("\nNo write progress for ");
				std::cout << (noProgressMs / 1000) << "s at LBA " << currentLBA
					<< " -- aborting\n";
				if (hasSubchannel) {
					Console::Info("The drive accepted this write mode but is not "
						"completing writes in it.\n");
				}
				progress.Finish(false);
				binInput.close();
				if (hasSubFile) subInput.close();
				return false;
			}

			if (senseKey == 0x02 && asc == 0x04) {
				if (!WriteDiscInternal::WaitForDriveReady(m_drive, 30)) {
					consecutiveErrors++;
					if (consecutiveErrors >= 5) {
						Console::Error("\nDrive not recovering - aborting\n");
						progress.Finish(false);
						binInput.close();
						if (hasSubFile) subInput.close();
						return false;
					}
					continue;
				}
				DWORD binSector = (sectorsWritten >= PREGAP_SECTORS) ? sectorsWritten - PREGAP_SECTORS : 0;
				binInput.seekg(static_cast<long long>(binSector) * AUDIO_SECTOR_SIZE);
				if (hasSubFile)
					subInput.seekg(static_cast<long long>(binSector) * SUBCHANNEL_SIZE);
				consecutiveErrors = 0;
				continue;
			}

			// Surface the raw sense bytes alongside the friendly text. The
			// description alone hides whether a "Not Ready" is the recoverable
			// spin-up case (ASC 04, handled above) or a post-failure session
			// abort (some other ASC), and which Medium Error subtype occurred.
			char senseHex[80];
			std::snprintf(senseHex, sizeof(senseHex),
				" [KEY=%02X ASC=%02X ASCQ=%02X]", senseKey, asc, ascq);
			Console::Error("\nWrite failed at LBA ");
			std::cout << currentLBA << " ("
				<< m_drive.GetSenseDescription(senseKey, asc, ascq) << ")"
				<< senseHex << "\n";

			// A Medium Error (sense key 03h) is non-recoverable in SAO/DAO: the
			// disc is laid down as one continuous stream, so the drive cannot
			// re-position to rewrite the failed sector. Retrying the same LBA only
			// produces a cascade of follow-on "Not Ready" errors (the drive has
			// already aborted the recording). Stop now with actionable guidance
			// rather than spinning through the retry budget.
			if (senseKey == 0x03) {
				Console::Error("Non-recoverable medium error - aborting write\n");
				Console::Info("The drive could not write this sector. Likely causes:\n");
				Console::Info("  - Defective or low-quality blank disc (try a different disc)\n");
				Console::Info("  - Write speed too high for this media (select a lower speed)\n");
				Console::Info("  - Laser power not calibrated for this dye (enable power calibration / OPC)\n");
				progress.Finish(false);
				binInput.close();
				if (hasSubFile) subInput.close();
				return false;
			}

			consecutiveErrors++;
			if (consecutiveErrors >= 5) {
				Console::Error("Too many consecutive write errors - aborting\n");
				progress.Finish(false);
				binInput.close();
				if (hasSubFile) subInput.close();
				return false;
			}

			DWORD binSector = (sectorsWritten >= PREGAP_SECTORS) ? sectorsWritten - PREGAP_SECTORS : 0;
			binInput.seekg(static_cast<long long>(binSector) * AUDIO_SECTOR_SIZE);
			if (hasSubFile)
				subInput.seekg(static_cast<long long>(binSector) * SUBCHANNEL_SIZE);
			Sleep(1000);
			continue;
		}

		consecutiveErrors = 0;
		sectorsWritten += batchSize;
		currentLBA += batchSize;
		lastProgressTime = std::chrono::steady_clock::now();  // forward progress
		progress.Update(sectorsWritten, writeTotalSectors);

		DWORD binPosition = (sectorsWritten > PREGAP_SECTORS) ? sectorsWritten - PREGAP_SECTORS : 0;
		while (currentTrackIdx + 1 < tracks.size() &&
			binPosition >= tracks[currentTrackIdx + 1].startLBA) {
			currentTrackIdx++;
			const auto& t = tracks[currentTrackIdx];
			Console::Info("\n  Track ");
			std::cout << t.trackNumber
				<< (t.isAudio ? "" : (t.dataMode == 2 ? " [MODE2]" : " [MODE1]"))
				<< " reached\n";
		}
	}

	progress.Finish(true);
	binInput.close();
	if (hasSubFile) subInput.close();

	Console::Success("Successfully wrote ");
	std::cout << sectorsWritten << " sectors";
	if (isMixedMode) std::cout << " (mixed-mode)";
	if (hasSubchannel) {
		std::cout << " (with subchannel";
		if (!hasSubFile) std::cout << ", synthesized";
		std::cout << ")";
	}
	std::cout << "\n";
	return true;
}
