#include "OpticalDrive.h"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <vector>

bool OpticalDrive::ReadCDText(DiscInfo& disc) {
	// Diagnostic toggle. Flip to true to print exactly what the drive returns to
	// the CD-Text query -- SCSI sense at each step, the drive-reported data length,
	// and a hex preview of the payload (header packs 0x80/0x81). Confirmed the
	// BDR-S13U reads CD-Text fine, so it's left off for normal operation; flip it
	// back on if a drive ever appears to return no CD-Text again.
	//   * firmware "not supported"  -> CHECK CONDITION (e.g. KEY=05 ASC=24 or 20)
	//   * disc genuinely has no text -> GOOD status, reported length < 4
	//   * real pack data that won't parse -> GOOD status with 0x80/0x81 packs
	bool dbg = false;
	auto hx = [](int b) {
		std::cout << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
			<< (b & 0xFF) << std::dec << std::nouppercase << std::setfill(' ');
	};
	auto showSense = [&](BYTE k, BYTE a, BYTE q) {
		std::cout << "sense KEY="; hx(k);
		std::cout << " ASC="; hx(a);
		std::cout << " ASCQ="; hx(q);
		std::cout << " (" << m_drive.GetSenseDescription(k, a, q) << ")";
	};

	BYTE cdb[10] = { 0x43, 0x00, 5, 0, 0, 0, 0, 0, 4, 0 };
	std::vector<BYTE> buf(4);
	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok1 = m_drive.SendSCSIWithSense(cdb, 10, buf.data(), 4, &sk, &asc, &ascq, true);

	if (dbg) {
		std::cout << "\n[CD-Text debug] READ TOC/PMA/ATIP (0x43) format 0x05\n";
		std::cout << "  Step 1 (4-byte header probe): " << (ok1 ? "OK  " : "FAIL  ");
		showSense(sk, asc, ascq);
		std::cout << "\n    header bytes: ";
		for (int i = 0; i < 4; i++) { hx(buf[i]); std::cout << " "; }
		std::cout << "\n    reported CD-Text data length = "
			<< static_cast<int>((buf[0] << 8) | buf[1]) << "\n";
	}

	if (!ok1) return false;

	WORD dataLen = static_cast<WORD>((buf[0] << 8) | buf[1]);
	if (dataLen < 4) {
		if (dbg) std::cout << "  -> length < 4: drive reports NO CD-Text on this disc.\n\n";
		return false;
	}

	buf.resize(dataLen + 2);
	cdb[7] = static_cast<BYTE>(((dataLen + 2) >> 8) & 0xFF);
	cdb[8] = static_cast<BYTE>((dataLen + 2) & 0xFF);
	sk = asc = ascq = 0;
	bool ok2 = m_drive.SendSCSIWithSense(cdb, 10, buf.data(), dataLen + 2, &sk, &asc, &ascq, true);

	if (dbg) {
		std::cout << "  Step 2 (full read, alloc=" << (dataLen + 2) << " bytes): "
			<< (ok2 ? "OK  " : "FAIL  ");
		showSense(sk, asc, ascq);
		std::cout << "\n    first bytes: ";
		int preview = static_cast<int>(buf.size());
		if (preview > 40) preview = 40;
		for (int i = 0; i < preview; i++) { hx(buf[i]); std::cout << " "; }
		std::cout << "\n";
	}

	if (!ok2) return false;

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

	if (dbg) {
		int filledTitles = 0, filledArtists = 0;
		for (const auto& s : disc.cdText.trackTitles) if (!s.empty()) filledTitles++;
		for (const auto& s : disc.cdText.trackArtists) if (!s.empty()) filledArtists++;
		std::cout << "  Parse result: albumTitle=\"" << disc.cdText.albumTitle
			<< "\"  albumArtist=\"" << disc.cdText.albumArtist << "\"\n";
		std::cout << "    per-track titles populated: " << filledTitles
			<< ", performers: " << filledArtists << "\n\n";
	}

	return !disc.cdText.albumTitle.empty() || !disc.cdText.albumArtist.empty();
}

// Reads the disc's Media Catalog Number (UPC/EAN) via READ SUB-CHANNEL
// sub-command 02h. The MCN is a single disc-level 13-digit value (unlike
// ISRC, which is per-track), so one query with track byte 0 is sufficient.
// Mirrors ReadISRC's parsing: the MCNValid flag is bit 7 of buf[8] and the
// 13 ASCII digits follow at buf[9].
bool OpticalDrive::ReadMCN(DiscInfo& disc) {
	BYTE cdb[10] = { 0x42, 0x00, 0x40, 0x02, 0, 0, 0, 0, 24, 0 };
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
	struct IsrcRow {
		int trackNumber = 0;
		std::string value;
	};
	std::vector<IsrcRow> rows;
	int validCount = 0;
	int zeroPlaceholderCount = 0;

	for (auto& track : disc.tracks) {
		if (!track.isAudio) continue;
		track.isrc.clear();
		IsrcRow row{ track.trackNumber, "Not present" };

		BYTE cdb[10] = { 0x42, 0x00, 0x40, 0x03, 0, 0,
			static_cast<BYTE>(track.trackNumber), 0, 24, 0 };
		std::vector<BYTE> buf(24);

		if (!m_drive.SendSCSI(cdb, 10, buf.data(), 24)) {
			row.value = "Read unavailable";
		}
		else if (buf[8] & 0x80) {
				std::string isrc;
				for (int i = 0; i < 12; i++) {
					char c = buf[9 + i];
					if (c >= '0' && c <= '9') isrc += c;
					else if (c >= 'A' && c <= 'Z') isrc += c;
					else if (c >= 'a' && c <= 'z') isrc += static_cast<char>(c - 'a' + 'A');
				}
				if (isrc == "000000000000") {
					// Some drives assert TCValid while returning an empty firmware
					// placeholder. It is not an ISRC and must not be persisted or
					// written back to a copied disc.
					++zeroPlaceholderCount;
				}
				else if (isrc.length() == 12) {
					track.isrc = isrc;
					row.value = isrc;
					++validCount;
				}
				else {
					row.value = "Invalid drive response";
				}
		}
		rows.push_back(std::move(row));
	}

	if (validCount == 0) {
		std::cout << "  No valid ISRC codes found (" << rows.size()
			<< " audio tracks checked).\n";
		if (zeroPlaceholderCount > 0) {
			std::cout << "  The drive returned " << zeroPlaceholderCount
				<< " all-zero placeholder" << (zeroPlaceholderCount == 1 ? "" : "s")
				<< "; ignored.\n";
		}
		return false;
	}

	std::cout << "\n  Track  ISRC / Status\n"
		<< "  -----  ----------------------\n";
	const auto originalFlags = std::cout.flags();
	for (const auto& row : rows) {
		std::cout << "  " << std::right << std::setw(5) << row.trackNumber
			<< "  " << row.value << "\n";
	}
	std::cout.flags(originalFlags);
	return true;
}
