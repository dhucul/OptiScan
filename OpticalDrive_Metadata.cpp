#include "OpticalDrive.h"
#include <iostream>
#include <cstring>
#include <vector>

bool OpticalDrive::ReadCDText(DiscInfo& disc) {
	BYTE cdb[10] = { 0x43, 0x00, 5, 0, 0, 0, 0, 0, 4, 0 };
	std::vector<BYTE> buf(4);
	if (!m_drive.SendSCSI(cdb, 10, buf.data(), 4)) return false;

	WORD dataLen = static_cast<WORD>((buf[0] << 8) | buf[1]);
	if (dataLen < 4) return false;

	buf.resize(dataLen + 2);
	cdb[7] = static_cast<BYTE>(((dataLen + 2) >> 8) & 0xFF);
	cdb[8] = static_cast<BYTE>((dataLen + 2) & 0xFF);
	if (!m_drive.SendSCSI(cdb, 10, buf.data(), dataLen + 2)) return false;

	disc.cdText.trackTitles.resize(disc.tracks.size());
	disc.cdText.trackArtists.resize(disc.tracks.size());

	// CD-Text strings can span multiple 18-byte packs (12 bytes of text each).
	// A pack's 12-byte payload may also contain the tail of one string and the
	// start of the next, separated by '\0'.  We accumulate text per pack-type
	// using a running track counter that advances on each '\0' boundary.

	// Running track index per pack type (0x80=title, 0x81=performer).
	// Starts at 0 (= disc-level); first '\0' advances to track 1, etc.
	int nextTrack[2] = { -1, -1 };   // -1 = not yet seen this pack type

	auto assignText = [&](int packIdx, const std::string& text) {
		int trk = nextTrack[packIdx];
		if (trk < 0) return;
		if (packIdx == 0) { // title
			if (trk == 0) disc.cdText.albumTitle += text;
			else if (static_cast<size_t>(trk) <= disc.tracks.size())
				disc.cdText.trackTitles[trk - 1] += text;
		}
		else { // performer
			if (trk == 0) disc.cdText.albumArtist += text;
			else if (static_cast<size_t>(trk) <= disc.tracks.size())
				disc.cdText.trackArtists[trk - 1] += text;
		}
		};

	BYTE* p = buf.data() + 4;
	BYTE* end = buf.data() + dataLen + 2;
	while (p + 18 <= end) {
		BYTE packType = p[0];
		BYTE trackNum = p[1] & 0x7F;

		int packIdx = -1;
		if (packType == 0x80) packIdx = 0;
		else if (packType == 0x81) packIdx = 1;

		if (packIdx >= 0) {
			// First pack of this type: initialise running counter from the header
			if (nextTrack[packIdx] < 0)
				nextTrack[packIdx] = trackNum;

			// Walk the 12-byte text payload, splitting on '\0'
			const char* txt = reinterpret_cast<const char*>(&p[4]);
			int pos = 0;
			while (pos < 12) {
				// Find extent of the current fragment
				int fragEnd = pos;
				while (fragEnd < 12 && txt[fragEnd] != '\0') ++fragEnd;

				if (fragEnd > pos) {
					assignText(packIdx, std::string(txt + pos, fragEnd - pos));
				}

				if (fragEnd < 12 && txt[fragEnd] == '\0') {
					// String terminated – advance to next track
					nextTrack[packIdx]++;
					pos = fragEnd + 1;
				}
				else {
					break; // reached end of 12-byte payload, string continues in next pack
				}
			}
		}
		p += 18;
	}
	return !disc.cdText.albumTitle.empty() || !disc.cdText.albumArtist.empty();
}

// Reads the disc's Media Catalog Number (UPC/EAN) via READ SUB-CHANNEL
// sub-command 02h. The MCN is a single disc-level 13-digit value (unlike
// ISRC, which is per-track), so one query with track byte 0 is sufficient.
// Mirrors ReadISRC's parsing: the MCNValid flag is bit 7 of buf[8] and the
// 13 ASCII digits follow at buf[9].
bool OpticalDrive::ReadMCN(DiscInfo& disc) {
	BYTE cdb[10] = { 0x42, 0x02, 0x02, 0, 0, 0, 0, 0, 24, 0 };
	std::vector<BYTE> buf(24);

	if (!m_drive.SendSCSI(cdb, 10, buf.data(), 24))
		return false;

	if (!(buf[8] & 0x80))
		return false;   // MCNValid not set -- disc has no catalog number

	std::string mcn;
	for (int i = 0; i < 13; i++) {
		char c = buf[9 + i];
		if (c >= '0' && c <= '9') mcn += c;
	}
	if (mcn.length() != 13)
		return false;          // incomplete / non-numeric -> treat as absent
	if (mcn == "0000000000000")
		return false;          // all-zero placeholder = no real MCN

	disc.mcn = mcn;
	std::cout << "  Catalog (MCN): " << mcn << "\n";
	return true;
}

bool OpticalDrive::ReadISRC(DiscInfo& disc) {
	std::cout << "\nReading ISRC codes...\n";

	for (auto& track : disc.tracks) {
		if (!track.isAudio) continue;

		BYTE cdb[10] = { 0x42, 0x02, 0x03, 0, 0, 0, static_cast<BYTE>(track.trackNumber), 0, 24, 0 };
		std::vector<BYTE> buf(24);

		if (m_drive.SendSCSI(cdb, 10, buf.data(), 24)) {
			if (buf[8] & 0x80) {
				std::string isrc;
				for (int i = 0; i < 12; i++) {
					char c = buf[9 + i];
					if (c >= '0' && c <= '9') isrc += c;
					else if (c >= 'A' && c <= 'Z') isrc += c;
					else if (c >= 'a' && c <= 'z') isrc += static_cast<char>(c - 'a' + 'A');
				}
				if (isrc.length() == 12) {
					track.isrc = isrc;
					std::cout << "  Track " << track.trackNumber << ": " << isrc << "\n";
				}
			}
		}
	}

	return true;
}