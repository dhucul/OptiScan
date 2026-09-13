#define NOMINMAX
#include "OpticalDrive.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include "PregapDetection.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <map>
#include <cstring>
#include <chrono>

static DWORD ParseTocLBA(const BYTE* p) {
	DWORD raw = (static_cast<DWORD>(p[0]) << 24) |
		(static_cast<DWORD>(p[1]) << 16) |
		(static_cast<DWORD>(p[2]) << 8) |
		static_cast<DWORD>(p[3]);

	if (raw >= 0x80000000u) {
		int32_t signedVal = static_cast<int32_t>(raw);
		if (signedVal >= -450 && signedVal < 0) {
			return 0;
		}
	}
	return raw;
}

bool OpticalDrive::ReadTOC(DiscInfo& disc, bool skipPregapScan) {
    if (!ReadFullTOC(disc)) return false;
    for (auto& track : disc.tracks) {
        track.pregapLBA = track.startLBA;
        track.index01LBA = track.startLBA;
        track.pregapVerified = false;
    }
    if (skipPregapScan) return true;
    ScopedDriveSpeed restoreSpeed(m_drive);
    m_drive.SetSpeed(4);
    Console::Info("\nDetermining pregaps from verified Q positions...\n");
    const bool reliableToc = !disc.tocRepaired || disc.tocLBAsRecovered;
    for (size_t i = 0; i < disc.tracks.size(); ++i) {
        auto& track = disc.tracks[i];
        if (!track.isAudio || (disc.selectedSession > 0 && track.session != disc.selectedSession)) continue;
        if (i == 0 && track.trackNumber == 1) {
            // Program-area audio starts at zero even when INDEX 01 is later.
            // Never drop that audio because Q or silence detection failed.
            Pregaps::SetFirstTrackBoundary(track, reliableToc,
                [&](DWORD lba, int& qt, int& qi) {
                    return !g_interrupt.IsInterrupted() && m_drive.ReadSectorQ(lba, qt, qi);
                });
        } else if (i > 0 && reliableToc && !g_interrupt.IsInterrupted()) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            const auto boundary = Pregaps::FindBoundary(track.startLBA,
                disc.tracks[i - 1].startLBA, track.trackNumber, disc.tracks[i - 1].trackNumber,
                [&](DWORD lba, int& qt, int& qi) { return m_drive.ReadSectorQ(lba, qt, qi); },
                [&] { return g_interrupt.IsInterrupted() || std::chrono::steady_clock::now() >= deadline; });
            if (boundary.verified) {
                track.pregapLBA = boundary.start;
                track.pregapVerified = true;
                if (boundary.start > 0)
                    disc.tracks[i - 1].endLBA = std::min(disc.tracks[i - 1].endLBA, boundary.start - 1);
            }
        }
        std::cout << "  Track " << track.trackNumber << ": ";
        if (track.pregapVerified) {
            const DWORD frames = track.startLBA - track.pregapLBA;
            std::cout << frames << " program-area pregap frame(s)\n";
        } else {
            Console::Warning("pregap UNKNOWN (Q unavailable, inconsistent, or timed out); audio boundaries retained.\n");
        }
        if (g_interrupt.IsInterrupted()) break;
    }
    if (!g_interrupt.IsInterrupted() && reliableToc) DetectHiddenTrack(disc);
    return !g_interrupt.IsInterrupted();
}

bool OpticalDrive::ReadFullTOC(DiscInfo& disc) {
	BYTE sessionCdb[10] = { 0x43, 0x00, 1, 0, 0, 0, 0, 0, 12, 0 };
	std::vector<BYTE> sessionBuf(12);
	if (m_drive.SendSCSI(sessionCdb, 10, sessionBuf.data(), 12)) {
		if (sessionBuf[3] >= sessionBuf[2]) {
			disc.sessionCount = sessionBuf[3] - sessionBuf[2] + 1;
		}
	}

	BYTE tocCdb[10] = { 0x43, 0x00, 0, 0, 0, 0, 0, 0x03, 0x24, 0 };
	std::vector<BYTE> tocBuf(804);
	if (!m_drive.SendSCSI(tocCdb, 10, tocBuf.data(), 804)) return false;

	int tocLen = (tocBuf[0] << 8) | tocBuf[1];
	if (tocLen < 2) return false;

	int firstTrack = tocBuf[2];
	int lastTrack = tocBuf[3];
	int n = lastTrack - firstTrack + 1;
	if (n <= 0 || n > 99) return false;

	disc.tracks.clear();

	BYTE fullTocCdb[10] = { 0x43, 0x00, 2, 0, 0, 0, 0, 0x08, 0x00, 0 };
	std::vector<BYTE> fullToc(2048);
	bool hasFullToc = m_drive.SendSCSI(fullTocCdb, 10, fullToc.data(), 2048);

	// ── Dump raw TOC for offline testing ──
#ifdef _DEBUG
	{
		std::ofstream dump("toc_dump.bin", std::ios::binary);
		if (dump) {
			auto writeBlock = [&](const char* tag, const BYTE* data, uint32_t len) {
				dump.write(tag, 4);                                      // 4-byte tag
				dump.write(reinterpret_cast<const char*>(&len), 4);      // 4-byte length
				dump.write(reinterpret_cast<const char*>(data), len);    // payload
				};
			writeBlock("SESN", sessionBuf.data(), static_cast<uint32_t>(sessionBuf.size()));
			uint32_t toc0Size = std::min(static_cast<uint32_t>(tocLen + 2), 804u);
			writeBlock("TOC0", tocBuf.data(), toc0Size);
			if (hasFullToc) {
				int fullTocLen = (fullToc[0] << 8) | fullToc[1];
			 uint32_t fullTocSize = std::min(static_cast<uint32_t>(fullTocLen + 2), 2048u);
				writeBlock("TOC2", fullToc.data(), fullTocSize);
			}
			std::cout << "  (TOC dumped to toc_dump.bin)\n";
		}
	}
#endif

	std::vector<int> trackSession(100, 1);
	auto sessionOf = [&](int trackNumber) -> int {
		return trackNumber >= 1 && trackNumber <= 99 ? trackSession[trackNumber] : 1;
	};
	std::map<int, DWORD> sessionLeadOut;    // session number → lead-out LBA
	std::map<int, DWORD> fullTocTrackStart; // track number → LBA from Full TOC MSF
	if (hasFullToc) {
		int fullTocLen = (fullToc[0] << 8) | fullToc[1];
		int endOffset = (fullTocLen + 2 < 2048) ? fullTocLen + 2 : 2048;
		BYTE* p = fullToc.data() + 4;
		BYTE* end = fullToc.data() + endOffset;
		while (p + 11 <= end) {
			int session = p[0];
			int point = p[3];
			if (point >= 1 && point <= 99) {
				trackSession[point] = session;

				// Extract track start MSF (PMIN/PSEC/PFRAME) for TOC recovery.
				// Copy-protection schemes corrupt Format 0 LBAs but the raw
				// Q-subchannel data in the lead-in (Format 2) often retains
				// the real track positions.
				int pmin = p[8];
				int psec = p[9];
				int pframe = p[10];
				if (pmin < 100 && psec < 60 && pframe < 75) {
					DWORD lba = static_cast<DWORD>((pmin * 60 + psec) * 75 + pframe);
					if (lba >= 150) lba -= 150; else lba = 0;
					fullTocTrackStart[point] = lba;
				}
			}

			// Point 0xA2 = lead-out start address for this session (MSF in PMIN/PSEC/PFRAME)
			if (point == 0xA2) {
				int pmin = p[8];
				int psec = p[9];
				int pframe = p[10];
				if (pmin < 100 && psec < 60 && pframe < 75) {
					const DWORD rawMsf = static_cast<DWORD>((pmin * 60 + psec) * 75 + pframe);
					if (rawMsf >= 150) sessionLeadOut[session] = rawMsf - 150;
				}
			}

			p += 11;
		}
	}

	bool anySignCorrected = false;

	// Parse lead-out LBA once — reused for last track's endLBA and disc.leadOutLBA.
	BYTE* leadOut = tocBuf.data() + 4 + n * 8;
	DWORD rawLeadOut = (static_cast<DWORD>(leadOut[4]) << 24) |
		(static_cast<DWORD>(leadOut[5]) << 16) |
		(static_cast<DWORD>(leadOut[6]) << 8) |
		static_cast<DWORD>(leadOut[7]);
	disc.leadOutLBA = ParseTocLBA(leadOut + 4);
	if (disc.leadOutLBA != rawLeadOut) {
		std::cout << "  Note: Lead-out LBA " << rawLeadOut << " (signed: "
			<< static_cast<int32_t>(rawLeadOut)
			<< ") - firmware sign bug corrected to "
			<< disc.leadOutLBA << ".\n";
		anySignCorrected = true;
	}

	for (int i = 0; i < n; i++) {
		BYTE* td = tocBuf.data() + 4 + i * 8;
		TrackInfo t = {};
		t.trackNumber = td[2];
		t.isAudio = (td[1] & 0x04) == 0;
		t.mode = t.isAudio ? 0 : 1;
		t.hasPreemphasis = (td[1] & 0x01) != 0;
		t.session = sessionOf(t.trackNumber);

		DWORD rawStart = (static_cast<DWORD>(td[4]) << 24) |
			(static_cast<DWORD>(td[5]) << 16) |
			(static_cast<DWORD>(td[6]) << 8) |
			static_cast<DWORD>(td[7]);
		t.startLBA = ParseTocLBA(td + 4);
		if (t.startLBA != rawStart) {
			std::cout << "  Note: Track " << t.trackNumber
				<< " LBA " << rawStart << " (signed: "
				<< static_cast<int32_t>(rawStart)
				<< ") - firmware sign bug corrected to "
				<< t.startLBA << ".\n";
			anySignCorrected = true;
		}

		if (i < n - 1) {
			BYTE* nextTd = tocBuf.data() + 4 + (i + 1) * 8;
			DWORD nextLBA = ParseTocLBA(nextTd + 4);
			t.endLBA = nextLBA > 0 ? nextLBA - 1 : 0;

			// If next track is in a different session, cap endLBA at this
			// session's lead-out to avoid scanning the inter-session gap.
			int nextSession = sessionOf(tocBuf[4 + (i + 1) * 8 + 2]);
			if (nextSession != t.session) {
				auto it = sessionLeadOut.find(t.session);
				if (it != sessionLeadOut.end() && it->second - 1 < t.endLBA) {
					t.endLBA = it->second - 1;
				}
			}
		}
		else {
			t.endLBA = disc.leadOutLBA > 0 ? disc.leadOutLBA - 1 : 0;
		}
		disc.tracks.push_back(t);
	}

	// For enhanced/multisession CDs, store the session-1 lead-out separately.
	// AccurateRip checksum slicing stops here; its disc IDs still use the
	// overall TOC lead-out (which points past the data session).
	auto it = sessionLeadOut.find(1);
	if (it != sessionLeadOut.end() && disc.sessionCount > 1) {
		disc.audioLeadOutLBA = it->second;
	}
	else {
		disc.audioLeadOutLBA = disc.leadOutLBA;
	}

	// ── TOC sanity check ──
	// A CD holds at most ~360,000 sectors (80 min).  Even extreme overburn
	// never exceeds ~450,000.  Any LBA beyond that is corrupt data from the
	// drive — clamp it so the disc remains usable rather than rejecting it.
	constexpr DWORD MAX_VALID_CD_LBA = 450000;
	bool tocRepaired = false;

	// Snapshot raw LBAs before clamping so the protection checker can
	// evaluate the original TOC, not the sanitised version.
	disc.rawTocEntries.clear();
	for (const auto& t : disc.tracks) {
		RawTocEntry raw;
		raw.trackNumber = t.trackNumber;
		raw.originalStartLBA = t.startLBA;
		raw.originalEndLBA = t.endLBA;
		disc.rawTocEntries.push_back(raw);
	}
	disc.rawLeadOutLBA = disc.leadOutLBA;

	for (auto& t : disc.tracks) {
		if (t.startLBA > MAX_VALID_CD_LBA) {
			std::cerr << "  WARNING: Track " << t.trackNumber
				<< " startLBA " << t.startLBA
				<< " beyond CD capacity - clamping to " << MAX_VALID_CD_LBA << ".\n";
			t.startLBA = MAX_VALID_CD_LBA;
			tocRepaired = true;
		}
		if (t.endLBA > MAX_VALID_CD_LBA) {
			std::cerr << "  WARNING: Track " << t.trackNumber
				<< " endLBA " << t.endLBA
				<< " beyond CD capacity - clamping to " << MAX_VALID_CD_LBA << ".\n";
			t.endLBA = MAX_VALID_CD_LBA;
			tocRepaired = true;
		}
	}
	if (disc.leadOutLBA > MAX_VALID_CD_LBA) {
		std::cerr << "  WARNING: Lead-out LBA " << disc.leadOutLBA
			<< " beyond CD capacity - clamping to " << MAX_VALID_CD_LBA << ".\n";
		disc.leadOutLBA = MAX_VALID_CD_LBA;
		tocRepaired = true;
	}
	if (disc.audioLeadOutLBA > MAX_VALID_CD_LBA) {
		disc.audioLeadOutLBA = MAX_VALID_CD_LBA;
	}
	if (tocRepaired) {
		// Fix inverted ranges caused by clamping startLBA past endLBA
		// (e.g. bogus startLBA=16M clamped to 450k, but endLBA=19999 from
		// a valid next-track pointer).  Set endLBA = startLBA so the track
		// has zero length rather than a DWORD-wrapping underflow.
		for (auto& t : disc.tracks) {
			if (t.startLBA > t.endLBA) {
				t.endLBA = t.startLBA;
			}
		}
		disc.tocRepaired = true;
		Console::SetColor(Console::Color::Yellow);
		std::cerr << "  TOC contained out-of-range LBAs (corrupt drive response). "
			<< "Values clamped - verify results.\n";
		Console::Reset();
	}

	// ── TOC recovery from Full TOC (Format 2) ──────────────────────────
	// Copy-protection schemes (Cactus Data Shield, Key2Audio, etc.) inject
	// bogus LBA values into the Format 0 TOC response.  The disc still
	// plays because CD players use the raw Q-subchannel TOC in the lead-in,
	// which is physically pressed and harder to forge.  Format 2 (READ TOC
	// with format=2) returns that raw lead-in data with MSF addresses.
	// If Format 0 was corrupt but Format 2 has valid entries, recover.
	if (tocRepaired && hasFullToc && !fullTocTrackStart.empty()) {
		// Save state — revert if recovery produces an invalid layout
		auto savedTracks = disc.tracks;
		DWORD savedLeadOut = disc.leadOutLBA;
		DWORD savedAudioLeadOut = disc.audioLeadOutLBA;
		bool anyRecovered = false;

		// Replace clamped startLBAs with Full TOC MSF-derived values
		for (auto& t : disc.tracks) {
			if (t.startLBA != MAX_VALID_CD_LBA) continue; // not clamped
			auto fit = fullTocTrackStart.find(t.trackNumber);
			if (fit != fullTocTrackStart.end() && fit->second < MAX_VALID_CD_LBA) {
				Console::SetColor(Console::Color::Cyan);
				std::cout << "  TOC recovery: Track " << t.trackNumber
					<< " startLBA " << MAX_VALID_CD_LBA
					<< " -> " << fit->second << " (from Full TOC)\n";
				Console::Reset();
				t.startLBA = fit->second;
				anyRecovered = true;
			}
		}

		// Recover lead-out from Full TOC session data
		if (disc.leadOutLBA == MAX_VALID_CD_LBA && !sessionLeadOut.empty()) {
			DWORD bestLeadOut = sessionLeadOut.rbegin()->second;
			if (bestLeadOut < MAX_VALID_CD_LBA) {
				disc.leadOutLBA = bestLeadOut;
				anyRecovered = true;
			}
		}

		if (anyRecovered) {
			// Recalculate endLBAs from recovered starts
			for (size_t i = 0; i < disc.tracks.size(); i++) {
				if (i + 1 < disc.tracks.size()) {
					disc.tracks[i].endLBA = disc.tracks[i + 1].startLBA > 0
						? disc.tracks[i + 1].startLBA - 1 : 0;
					int nextSess = disc.tracks[i + 1].session;
					if (nextSess != disc.tracks[i].session) {
						auto sit = sessionLeadOut.find(disc.tracks[i].session);
						if (sit != sessionLeadOut.end() && sit->second > 0 &&
							sit->second - 1 < disc.tracks[i].endLBA)
							disc.tracks[i].endLBA = sit->second - 1;
					}
				}
				else {
					disc.tracks[i].endLBA = disc.leadOutLBA > 0 ? disc.leadOutLBA - 1 : 0;
				}
			}

			// Recalculate audioLeadOutLBA
			auto s1it = sessionLeadOut.find(1);
			if (s1it != sessionLeadOut.end() && disc.sessionCount > 1)
				disc.audioLeadOutLBA = s1it->second;
			else
				disc.audioLeadOutLBA = disc.leadOutLBA;

			// Validate: all LBAs in range, start <= end, monotonically increasing
			bool valid = true;
			for (size_t i = 0; i < disc.tracks.size(); i++) {
				if (disc.tracks[i].startLBA >= MAX_VALID_CD_LBA
					|| disc.tracks[i].endLBA >= MAX_VALID_CD_LBA
					|| disc.tracks[i].startLBA > disc.tracks[i].endLBA) {
					valid = false; break;
				}
				if (i > 0 && disc.tracks[i].startLBA <= disc.tracks[i - 1].startLBA) {
					valid = false; break;
				}
			}

			if (valid) {
				// Track positions are now trustworthy — allow pregap scanning
				// and hidden-track detection.  tocRepaired stays true for the
				// protection checker (which uses rawTocEntries to evaluate the
				// original corruption).
				disc.tocLBAsRecovered = true;
				Console::Success("  TOC LBAs recovered from Full TOC data.\n");
			}
			else {
				// Recovery produced invalid layout — revert to clamped values
				disc.tracks = savedTracks;
				disc.leadOutLBA = savedLeadOut;
				disc.audioLeadOutLBA = savedAudioLeadOut;
				Console::SetColor(Console::Color::Yellow);
				std::cerr << "  TOC recovery failed - Full TOC data also appears corrupt.\n";
				Console::Reset();
			}
		}
	}

	for (auto& t : disc.tracks) {
		t.pregapLBA = t.startLBA;  // safe default until pregap scan runs
		t.index01LBA = t.startLBA; // default: INDEX 01 matches TOC start
	}
	disc.tocSignCorrected = anySignCorrected;
	return true;
}
