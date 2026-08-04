#define NOMINMAX
#include "WriteDiscInternal.h"
#include "ConsoleColors.h"
#include "InterruptHandler.h"
#include "Progress.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <vector>

namespace {
	DWORD ReadLeadInLength(ScsiDrive& drive) {
		// Lead-in start comes from READ TOC/PMA/ATIP, Format 0100b (ATIP), per the
		// libburn SAO cookbook. The lead-in start MSF lives in response bytes 8/9/10;
		// its minute is >= 90, so LBA = (M*60 + S)*75 + F - 450150. The MSF values
		// may be returned as binary or BCD depending on the drive, so decode both
		// and accept whichever yields a minute in the valid 90..99 lead-in range.
		// Returns the number of frames from the lead-in start up to LBA -150.
		BYTE cdb[10] = { 0x43, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 28, 0x00 };
		BYTE atip[28] = {};
		BYTE sk = 0, asc = 0, ascq = 0;

		constexpr int32_t TYPICAL_LEADIN_START = -11635;  // fallback if ATIP fails
		int32_t leadInStart = TYPICAL_LEADIN_START;

		if (drive.SendSCSIWithSense(cdb, sizeof(cdb), atip, sizeof(atip), &sk, &asc, &ascq, true)) {
			auto toLba = [](int m, int s, int f) { return (m * 60 + s) * 75 + f - 450150; };
			const int rm = atip[8], rs = atip[9], rf = atip[10];
			const int bm = BcdToBin(atip[8]), bs = BcdToBin(atip[9]), bf = BcdToBin(atip[10]);
			if (rm >= 90 && rm <= 99 && rs < 60 && rf < 75)
				leadInStart = toLba(rm, rs, rf);
			else if (bm >= 90 && bm <= 99 && bs < 60 && bf < 75)
				leadInStart = toLba(bm, bs, bf);
		}

		// Guard against an implausible read (lead-in must precede the pre-gap).
		if (leadInStart >= -150 || leadInStart < -50000) leadInStart = TYPICAL_LEADIN_START;
		return static_cast<DWORD>(-150 - leadInStart);
	}

	void SetRawRWData(BYTE* raw96, const BYTE* rw72) {
		memset(raw96, 0, SUBCHANNEL_SIZE);
		for (int i = 0; i < SUBCHANNEL_SIZE; i += 4) {
			raw96[i]     |= static_cast<BYTE>((rw72[0] >> 2) & 0x3F);
			raw96[i + 1] |= static_cast<BYTE>(((rw72[0] << 4) & 0x30) | ((rw72[1] >> 4) & 0x0F));
			raw96[i + 2] |= static_cast<BYTE>(((rw72[1] << 2) & 0x3C) | ((rw72[2] >> 6) & 0x03));
			raw96[i + 3] |= static_cast<BYTE>(rw72[2] & 0x3F);
			rw72 += 3;
		}
	}

	std::vector<BYTE> BuildCDTextLeadInSubchannels(const std::vector<BYTE>& packs) {
		const size_t packCount = packs.size() / 18;
		if (packCount == 0) return {};

		size_t subchannelCount = 0;
		switch (packCount % 4) {
		case 0:  subchannelCount = packCount / 4; break;
		case 2:  subchannelCount = packCount / 2; break;
		default: subchannelCount = packCount;     break;
		}

		std::vector<BYTE> subchannels(subchannelCount * SUBCHANNEL_SIZE, 0);
		size_t packIndex = 0;
		for (size_t sc = 0; sc < subchannelCount; sc++) {
			BYTE rw72[72] = {};
			for (int p = 0; p < 4; p++) {
				memcpy(rw72 + p * 18, packs.data() + packIndex * 18, 18);
				packIndex = (packIndex + 1) % packCount;
			}
			SetRawRWData(subchannels.data() + sc * SUBCHANNEL_SIZE, rw72);
		}
		return subchannels;
	}
}

// ============================================================================
// Helper: Calculate CRC-16 for CD-Text packs (CRC-CCITT, poly 0x1021)
// ============================================================================
static uint16_t CDTextCRC(const BYTE* data, int len) {
	uint16_t crc = 0;
	for (int i = 0; i < len; i++) {
		crc ^= static_cast<uint16_t>(data[i]) << 8;
		for (int b = 0; b < 8; b++) {
			if (crc & 0x8000)
				crc = (crc << 1) ^ 0x1021;
			else
				crc <<= 1;
		}
	}
	return ~crc;
}

// ============================================================================
// Helper: Build CD-Text packs from CUE metadata
// ============================================================================
std::vector<BYTE> WriteDiscInternal::BuildCDTextPacks(
	const std::string& discTitle,
	const std::string& discPerformer,
	const std::vector<OpticalDrive::TrackWriteInfo>& tracks) {

	std::vector<BYTE> packs;
	bool sequenceOverflow = false;

	auto buildPacksForType = [&](BYTE packType,
		const std::string& discStr,
		const std::vector<std::string>& trackStrs) {

			std::vector<BYTE> textBuf;
			std::vector<BYTE> trackNumAtString;

			trackNumAtString.push_back(0x00);
			for (char c : discStr) textBuf.push_back(static_cast<BYTE>(c));
			textBuf.push_back(0x00);

			for (size_t ti = 0; ti < trackStrs.size(); ti++) {
				BYTE tno = static_cast<BYTE>(tracks[ti].trackNumber);
				trackNumAtString.push_back(tno);
				for (char c : trackStrs[ti]) textBuf.push_back(static_cast<BYTE>(c));
				textBuf.push_back(0x00);
			}

			size_t seqBase = packs.size() / 18;
			size_t offset = 0;
			int stringIdx = 0;
			int charInString = 0;

			while (offset < textBuf.size()) {
				// Three 0x8F size packs are appended below; sequence numbers are
				// one byte, so reserve 253..255 for them.
				if (seqBase >= 253) {
					sequenceOverflow = true;
					return;
				}
				BYTE pack[18] = { 0 };
				pack[0] = packType;
				pack[1] = trackNumAtString[stringIdx];
				pack[2] = static_cast<BYTE>(seqBase++);
				// Byte 3 high nibble is the language block number.  We only
				// emit block 0, so cap the character-position nibble at 0x0F
				// instead of letting long strings spill into the block bits.
				pack[3] = static_cast<BYTE>(charInString > 15 ? 0x0F : charInString);

				for (int i = 0; i < 12 && offset < textBuf.size(); i++, offset++) {
					pack[4 + i] = textBuf[offset];
					if (textBuf[offset] == 0x00) {
						stringIdx++;
						charInString = 0;
					}
					else {
						charInString++;
					}
				}

				uint16_t crc = CDTextCRC(pack, 16);
				pack[16] = static_cast<BYTE>((crc >> 8) & 0xFF);
				pack[17] = static_cast<BYTE>(crc & 0xFF);

				packs.insert(packs.end(), pack, pack + 18);
			}
		};

	std::vector<std::string> trackTitles, trackPerformers;
	for (const auto& t : tracks) {
		trackTitles.push_back(t.title.empty() ? "" : t.title);
		trackPerformers.push_back(t.performer.empty() ? "" : t.performer);
	}

	buildPacksForType(0x80, discTitle, trackTitles);
	buildPacksForType(0x81, discPerformer, trackPerformers);
	if (sequenceOverflow) {
		Console::Warning("CD-Text exceeds the 256-pack sequence limit; metadata was not emitted\n");
		return {};
	}

	{
		int totalPacks = static_cast<int>(packs.size() / 18);
		int titlePacks = 0, performerPacks = 0;
		for (int i = 0; i < totalPacks; i++) {
			BYTE pt = packs[i * 18];
			if (pt == 0x80) titlePacks++;
			else if (pt == 0x81) performerPacks++;
		}

		BYTE firstTrack = static_cast<BYTE>(tracks.empty() ? 1 : tracks.front().trackNumber);
		BYTE lastTrack = static_cast<BYTE>(tracks.empty() ? 0 : tracks.back().trackNumber);

		BYTE sizeInfo[36] = { 0 };
		sizeInfo[0] = 0x01;  // ISO-646 / ASCII
		sizeInfo[1] = firstTrack;
		sizeInfo[2] = lastTrack;
		sizeInfo[3] = 0x00;
		sizeInfo[4] = static_cast<BYTE>(titlePacks);
		sizeInfo[5] = static_cast<BYTE>(performerPacks);
		sizeInfo[19] = 3;  // pack type 0x8F (size information)
		sizeInfo[20] = static_cast<BYTE>(totalPacks + 2); // block 0 last sequence
		sizeInfo[28] = 0x09; // block 0 language: English

		size_t seqBase = packs.size() / 18;
		for (int p = 0; p < 3; p++) {
			BYTE pack[18] = { 0 };
			pack[0] = 0x8F;
			pack[1] = static_cast<BYTE>(p);
			pack[2] = static_cast<BYTE>(seqBase++);
			pack[3] = 0x00;
			memcpy(&pack[4], &sizeInfo[p * 12], 12);

			uint16_t crc = CDTextCRC(pack, 16);
			pack[16] = static_cast<BYTE>((crc >> 8) & 0xFF);
			pack[17] = static_cast<BYTE>(crc & 0xFF);

			packs.insert(packs.end(), pack, pack + 18);
		}
	}

	return packs;
}

// ============================================================================
// Helper: Check if any CD-Text content exists
// ============================================================================
bool WriteDiscInternal::HasCDTextContent(const std::string& discTitle,
	const std::string& discPerformer,
	const std::vector<OpticalDrive::TrackWriteInfo>& tracks) {
	if (!discTitle.empty() || !discPerformer.empty()) return true;
	for (const auto& t : tracks) {
		if (!t.title.empty() || !t.performer.empty()) return true;
	}
	return false;
}

// ============================================================================
// Helper: Send CD-Text packs to drive via WRITE BUFFER (0x3B)
// ============================================================================
bool WriteDiscInternal::SendCDTextToDevice(ScsiDrive& drive, const std::vector<BYTE>& packs) {
	if (packs.empty()) return false;

	DWORD packDataLen = static_cast<DWORD>(packs.size());
	DWORD headerPayloadLen = 4 + packDataLen;
	std::vector<BYTE> headerPayload(headerPayloadLen, 0);

	WORD dataLen = static_cast<WORD>(headerPayloadLen - 2);
	headerPayload[0] = static_cast<BYTE>((dataLen >> 8) & 0xFF);
	headerPayload[1] = static_cast<BYTE>(dataLen & 0xFF);
	memcpy(headerPayload.data() + 4, packs.data(), packDataLen);
	std::vector<BYTE> packPayload(packs.begin(), packs.end());

	// Different drives accept different WRITE BUFFER mode / buffer-ID
	// combinations for CD-Text.  MMC CD-Text uses buffer ID 0x08; keep the
	// older ID 0 fallbacks for drives/bridges that expose the buffer through
	// the generic WRITE BUFFER selector instead.
	struct WriteBufferVariant {
		BYTE mode;
		BYTE bufferId;
		bool includeHeader;
		const char* label;
	};

	static const WriteBufferVariant variants[] = {
		{ 0x00, 0x08, true,  "mode 0 / id 0x08 (CD-Text header+packs)" },
		{ 0x02, 0x08, false, "mode 2 / id 0x08 (CD-Text packs)"        },
		{ 0x00, 0x00, true,  "mode 0 / id 0x00 (legacy header+packs)"  },
		{ 0x02, 0x00, false, "mode 2 / id 0x00 (legacy packs)"         },
		{ 0x01, 0x00, true,  "mode 1 / id 0x00 (vendor header+packs)"  },
		{ 0x05, 0x00, true,  "mode 5 / id 0x00 (download+save)"        },
	};

	for (const auto& v : variants) {
		BYTE* payload = v.includeHeader
			? headerPayload.data()
			: packPayload.data();
		DWORD payloadLen = v.includeHeader ? headerPayloadLen : packDataLen;

		BYTE cdb[10] = { 0x3B, v.mode, v.bufferId, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		cdb[6] = static_cast<BYTE>((payloadLen >> 16) & 0xFF);
		cdb[7] = static_cast<BYTE>((payloadLen >> 8) & 0xFF);
		cdb[8] = static_cast<BYTE>(payloadLen & 0xFF);

		BYTE senseKey = 0, asc = 0, ascq = 0;
		if (drive.SendSCSIWithSense(cdb, sizeof(cdb), payload, payloadLen,
			&senseKey, &asc, &ascq, false)) {
			return true;  // success -- caller reports it
		}

		// "Invalid field in CDB" / "invalid parameter list" just means this
		// selector or payload layout isn't supported -- expected on non-Plextor
		// drives -- so try the next variant quietly. Any other sense (medium,
		// hardware, command-sequence) is a real problem worth surfacing.
		if (senseKey != 0x05 || (asc != 0x24 && asc != 0x26)) {
			Console::Warning("CD-Text WRITE BUFFER error (");
			std::cout << drive.GetSenseDescription(senseKey, asc, ascq) << ")\n";
			return false;
		}
	}

	return false;  // no WRITE BUFFER variant accepted; caller falls back
}

// ============================================================================
// Helper: Send CD-Text as raw R-W subchannel frames in the lead-in.
// ============================================================================
bool WriteDiscInternal::SendCDTextLeadInToDevice(ScsiDrive& drive, const std::vector<BYTE>& packs,
	bool* wroteAnyLeadInFrame) {
	if (wroteAnyLeadInFrame) *wroteAnyLeadInFrame = false;

	std::vector<BYTE> subchannels = BuildCDTextLeadInSubchannels(packs);
	if (subchannels.empty()) return false;

	DWORD leadInLen = ReadLeadInLength(drive);
	int32_t currentLba = -150 - static_cast<int32_t>(leadInLen);
	DWORD remaining = leadInLen;
	size_t subchannelIndex = 0;

	Console::Info("Writing CD-Text into lead-in (");
	std::cout << leadInLen << " frames)...\n";
	// The first burn write of a session can return "not ready, becoming ready"
	// (KEY=02 ASC=04) for several seconds while the drive spins up and performs
	// automatic power calibration. Give it a head start before the first WRITE.
	WaitForDriveReady(drive, 15);

	constexpr DWORD FRAMES_PER_WRITE = 64;
	constexpr DWORD LEADIN_WRITE_TIMEOUT_SEC = 15;
	std::vector<BYTE> buffer(FRAMES_PER_WRITE * SUBCHANNEL_SIZE);

	ProgressIndicator progress(35);
	progress.SetLabel("CD-Text");
	progress.SetUnitBytes(SUBCHANNEL_SIZE);
	progress.Start();
	progress.Update(0, static_cast<int>(leadInLen));

	DWORD framesWritten = 0;

	while (remaining > 0) {
		if (InterruptHandler::Instance().IsInterrupted()) {
			Console::Error("\nCD-Text lead-in write cancelled by user\n");
			progress.Finish(false);
			return false;
		}

		const bool firstWrite = (framesWritten == 0);
		DWORD batchFrames = firstWrite ? 1 : (std::min)(remaining, FRAMES_PER_WRITE);
		for (DWORD i = 0; i < batchFrames; i++) {
			const BYTE* src = subchannels.data() + subchannelIndex * SUBCHANNEL_SIZE;
			memcpy(buffer.data() + i * SUBCHANNEL_SIZE, src, SUBCHANNEL_SIZE);
			subchannelIndex++;
			if (subchannelIndex >= subchannels.size() / SUBCHANNEL_SIZE)
				subchannelIndex = 0;
		}

		BYTE cdb[10] = { 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		DWORD lba = static_cast<DWORD>(currentLba);
		cdb[2] = static_cast<BYTE>((lba >> 24) & 0xFF);
		cdb[3] = static_cast<BYTE>((lba >> 16) & 0xFF);
		cdb[4] = static_cast<BYTE>((lba >> 8) & 0xFF);
		cdb[5] = static_cast<BYTE>(lba & 0xFF);
		cdb[7] = static_cast<BYTE>((batchFrames >> 8) & 0xFF);
		cdb[8] = static_cast<BYTE>(batchFrames & 0xFF);

		DWORD transferBytes = batchFrames * SUBCHANNEL_SIZE;
		bool wrote = false;
		// The first burn write may report "not ready, becoming ready" (KEY=02
		// ASC=04) repeatedly while the drive spins up and auto-calibrates laser
		// power (no OPC was run). Poll readiness properly -- as the audio path does
		// -- for up to a real ~30s budget, instead of a fixed handful of short
		// sleeps, before treating the write as a genuine stall.
		constexpr long long READY_BUDGET_MS = 30000;
		auto batchStart = std::chrono::steady_clock::now();
		for (int attempt = 0; !wrote; attempt++) {
			BYTE sk = 0, asc = 0, ascq = 0;
			if (drive.SendSCSIWithSense(cdb, sizeof(cdb), buffer.data(), transferBytes,
				&sk, &asc, &ascq, false, LEADIN_WRITE_TIMEOUT_SEC)) {
				wrote = true;
				break;
			}

			if (sk == 0 && asc == 0 && ascq == 0) {
				progress.Finish(false);
				Console::Warning("CD-Text lead-in WRITE did not complete at LBA ");
				std::cout << currentLba << " within " << LEADIN_WRITE_TIMEOUT_SEC
					<< "s (no SCSI sense returned)\n";
				return false;
			}

			// Not ready / becoming ready: wait for the drive instead of failing.
			if (sk == 0x02 && asc == 0x04) {
				if (attempt == 0) progress.AddRetries(1);
				long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - batchStart).count();
				if (elapsedMs > READY_BUDGET_MS) break;  // genuine stall -- give up
				WaitForDriveReady(drive, 5);
				Sleep(200);
				continue;
			}

			progress.Finish(false);
			Console::Warning("CD-Text lead-in WRITE failed at LBA ");
			std::cout << currentLba << " (" << drive.GetSenseDescription(sk, asc, ascq)
				<< " [KEY=" << std::hex << std::uppercase << std::setfill('0')
				<< std::setw(2) << static_cast<int>(sk)
				<< " ASC=" << std::setw(2) << static_cast<int>(asc)
				<< " ASCQ=" << std::setw(2) << static_cast<int>(ascq)
				<< std::dec << std::nouppercase << std::setfill(' ') << "])\n";
			return false;
		}

		if (!wrote) {
			progress.Finish(false);
			Console::Warning("CD-Text lead-in WRITE timed out waiting for drive readiness\n");
			return false;
		}

		currentLba += static_cast<int32_t>(batchFrames);
		remaining -= batchFrames;
		framesWritten += batchFrames;
		if (wroteAnyLeadInFrame) *wroteAnyLeadInFrame = true;
		progress.Update(static_cast<int>(framesWritten), static_cast<int>(leadInLen));
	}

	progress.Finish(true);
	Console::Success("CD-Text lead-in written\n");
	return true;
}
