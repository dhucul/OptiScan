// ============================================================================
// ScsiDrive.LiteOnScan.cpp - LiteOn/MediaTek CD quality scan
//
// OLD-method (0xDF) protocol HARDWARE-VERIFIED on a PLEXTOR PX-891SAF PLUS
// (a MediaTek/PLDS drive) by reverse-engineering Vinpower's libqscan_liteon.dll
// and running a live C1/C2 scan of a burned CD-R. The init sequence, the
// per-block command triplet, and the C1/C2/CU field offsets below all match.
//
// CRITICAL: a MediaTek/PLDS drive tallies C1/C2 in its CIRC decoder ONLY for
// sectors the host actively reads — it does NOT scan the disc autonomously
// after the DF/A0 arm. So each interval must READ the disc (LiteOnScanDriveHead)
// before DF/82/05 is read, otherwise the counters stay idle at zero (which is
// exactly the "0xDF accepted but trial reads returned all zeros" failure mode).
//
// NEW method (0xF3):  (from QPXTool; not exercised on the PX-891, which is OLD-path)
//   Init:  seek to startLBA, then 0xF3/0x0E probe (16-byte read)
//   Block: 0xF3/0x0E — returns 16 bytes per time slice:
//          byte[1]=min, byte[2]=sec, byte[3]=frame (MSF position)
//          bytes[4-5]=BLER/C1 (BE16), bytes[6-7]=E22/C2 (BE16)
//   End:   no explicit stop needed
//
// OLD method (0xDF):
//   Init:  seek to startLBA, then 0xDF/0xA3, 0xDF/0xA0 sequence (5 commands)
//   Block: READ the interval (drive the head), then
//          0xDF/0x82/0x09 (latch), 0xDF/0x82/0x05 (getdata), 0xDF/0x97 (reset)
//          bytes[0-1]=C1/BLER (BE16), bytes[2-3]=C2/E22 (BE16), byte[4]=CU
//          LBA += 75 per block
//   End:   0xDF/0xA3/0x01
//
// Both methods read at the current drive speed (not locked to 1x).
// At 8x a 72-min disc takes ~9 minutes — same as BLER scan.
#include "ScsiDrive.h"
#include "CdScanInterval.h"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstring>
#include <thread>
#include <chrono>

// ── Head-driving reads (shared by the C1/C2, jitter and FE/TE scans) ────────
// The MediaTek/PLDS error counters only advance for sectors the host reads, so
// the scan must sweep the disc itself. These reads exist purely to move the
// head; the returned data is discarded (only the drive's internal tally,
// fetched via DF/82/05 etc., matters).

bool ScsiDrive::LiteOnScanDriveHead(DWORD lba, DWORD sectors) {
	// Chunk <= 16 sectors per read (16 audio sectors = 0x9300 B) to stay under
	// the 16-bit ATAPI transfer ceiling (0xFFFE). Keep scanning after a
	// failed read, but do not claim that all requested sectors were measured.
	std::vector<BYTE> buf(kCdScanReadChunkSectors * AUDIO_SECTOR_SIZE);
	return ReadCdScanChunks(lba, sectors, [&](std::uint32_t start, std::uint32_t count) {
		return ReadCdAudio(start, count, 0x00, buf.data(), count * AUDIO_SECTOR_SIZE);
	});
}

bool ScsiDrive::SupportsLiteOnScan() {
	if (m_liteonScanProbed >= 0) return m_liteonScanProbed == 1;
	// Prefer explicit host-driven intervals. A successful F3 response alone
	// cannot establish interval coverage or CU support (notably on PX-891SAF).
	if (m_liteonScanActive && !LiteOnScanStop()) { Close(); return false; }
	const auto selected = ProbeLiteOnScanMethod([&]() {
		m_liteonScanMethod = LiteOnScanMethod::MeasuredIntervals;
		if (!LiteOnScanStart(0, 224)) return false;
		bool accepted = false;
		for (int trial = 0; trial < 3; ++trial) {
			int c1 = 0, c2 = 0, cu = 0;
			DWORD lba = 0, sectors = 0;
			bool done = false, valid = false;
			if (!LiteOnScanPoll(c1, c2, cu, lba, done, &sectors, &valid)) {
				accepted = false;
				break;
			}
			accepted = accepted || valid; // measured zero is a valid response
			if (done) break;
		}
		const bool stopped = LiteOnScanStop();
		return accepted && stopped;
	}, [&]() {
		if (m_liteonScanActive && !LiteOnScanStop()) return false;
		m_liteonScanMethod = LiteOnScanMethod::CounterSamples;
		if (!SeekToLBA(0)) return false;
		bool havePosition = false;
		DWORD previous = 0;
		// A command accepted with empty/stale bytes does not establish support.
		// A healthy disc may have zero counters; advancing positions are enough.
		for (int attempt = 0; attempt < 8; ++attempt) {
			BYTE cdb[12] = {};
			cdb[0] = 0xF3; cdb[1] = 0x0E;
			BYTE buf[16] = {};
			BYTE sk = 0, asc = 0, ascq = 0;
			if (!SendSCSIWithSense(cdb, 12, buf, sizeof(buf), &sk, &asc, &ascq))
				return false;
			if (buf[2] >= 60 || buf[3] >= 75) return false;
			const DWORD msf = DWORD(buf[1]) * 4500 + DWORD(buf[2]) * 75 + buf[3];
			if (msf >= 150) {
				const DWORD position = msf - 150;
				if (havePosition && position < previous) return false;
				if (havePosition && position > previous) return true;
				previous = position;
				havePosition = true;
			}
			if (attempt < 7) std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return false;
	});
	if (selected == LiteOnScanMethod::Unknown) {
		if (m_liteonScanActive) { Close(); return false; }
		m_liteonScanMethod = LiteOnScanMethod::Unknown;
		// Failed probes can be transient; allow a later attempt to retry.
		m_liteonScanProbed = -1;
		return false;
	}
	m_liteonScanMethod = selected;
	m_liteonScanProbed = 1;
	return true;
}

bool ScsiDrive::LiteOnScanStart(DWORD startLBA, DWORD endLBA) {
	if (startLBA > endLBA || m_liteonScanMethod == LiteOnScanMethod::Unknown)
		return false;
	if (m_liteonScanActive && !LiteOnScanStop()) return false;
	m_liteonLBA = startLBA;
	m_liteonEndLBA = endLBA;
	m_liteonPosition.Reset(startLBA, endLBA);
	if (!SeekToLBA(startLBA)) return false;
	m_liteonScanActive = true;

	if (m_liteonScanMethod == LiteOnScanMethod::CounterSamples) {

		BYTE cdb[12] = {};
		cdb[0] = 0xF3;
		cdb[1] = 0x0E;
		std::vector<BYTE> buf(0x10, 0);
		BYTE sk = 0, asc = 0, ascq = 0;
		bool ok = SendSCSIWithSense(cdb, 12, buf.data(), 0x10, &sk, &asc, &ascq);

		char dbg[128];
		snprintf(dbg, sizeof(dbg), "LiteOnScanStart(new): startLBA=%lu ok=%d sk=0x%02X\n",
			(unsigned long)startLBA, ok, sk);
		OutputDebugStringA(dbg);
		if (!ok) LiteOnScanStop();
		return ok;
	}
	else {
		// OLD: full 5-command init sequence from QPXTool

		std::vector<BYTE> buf(256, 0);
		BYTE cdb[12] = {};
		BYTE sk = 0, asc = 0, ascq = 0;

		// Step A: 0xDF/0xA3
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA3;
		if (!SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq))
			{ LiteOnScanStop(); return false; }

		// Step B: 0xDF/0xA0 with byte[4]=0x02
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA0; cdb[4] = 0x02;
		if (!SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq))
			{ LiteOnScanStop(); return false; }

		// Step C: 0xDF/0xA0
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA0;
		if (!SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq))
			{ LiteOnScanStop(); return false; }

		// Step D: 0xDF/0xA0 with byte[4]=0x04
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA0; cdb[4] = 0x04;
		if (!SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq))
			{ LiteOnScanStop(); return false; }

		// Step E: 0xDF/0xA0 with byte[4]=0x02
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0xA0; cdb[4] = 0x02;
		if (!SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq))
			{ LiteOnScanStop(); return false; }

		// Exclude seek/probe activity before the first explicitly read interval.
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0x97;
		if (!SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq)) {
			LiteOnScanStop();
			return false;
		}
		OutputDebugStringA("LiteOnScanStart(old): init sequence complete\n");
		return true;
	}
}

bool ScsiDrive::LiteOnScanPoll(int& c1, int& c2, int& cu,
	DWORD& currentLBA, bool& scanDone, DWORD* measuredSectors, bool* sampleValid) {
	if (measuredSectors) *measuredSectors = 0;
	if (sampleValid) *sampleValid = false;
	c1 = c2 = cu = 0;
	scanDone = false;
	currentLBA = m_liteonLBA;
	if (!m_liteonScanActive) return false;
	if (m_liteonPosition.complete) {
		currentLBA = m_liteonEndLBA;
		scanDone = true;
		return true;
	}
	BYTE sk = 0, asc = 0, ascq = 0;
	constexpr int kCommandAttempts = 5;
	auto sendPollCommand = [&](BYTE* cdb, BYTE cdbLength, BYTE* data,
		DWORD dataSize, const char* stage) {
		for (int attempt = 0; attempt < kCommandAttempts; ++attempt) {
			sk = asc = ascq = 0;
			if (SendSCSIWithSense(cdb, cdbLength, data, dataSize,
				&sk, &asc, &ascq)) {
				return true;
			}
			if (attempt + 1 < kCommandAttempts)
				std::this_thread::sleep_for(
					std::chrono::milliseconds(200 * (attempt + 1)));
		}

		char dbg[192];
		snprintf(dbg, sizeof(dbg),
			"LiteOnScanPoll: %s failed after %d attempts "
			"at LBA %lu (sk=0x%02X asc=0x%02X ascq=0x%02X)\n",
			stage, kCommandAttempts, static_cast<unsigned long>(m_liteonLBA),
			sk, asc, ascq);
		OutputDebugStringA(dbg);
		return false;
	};

	if (m_liteonScanMethod == LiteOnScanMethod::CounterSamples) {
		// NEW: each 0xF3/0x0E call returns one time slice
		BYTE cdb[12] = {};
		cdb[0] = 0xF3;
		cdb[1] = 0x0E;
		std::vector<BYTE> buf(0x10, 0);

		if (!sendPollCommand(cdb, 12, buf.data(), 0x10, "0xF3/0x0E data")) {
			return false;
		}

		if (buf[2] >= 60 || buf[3] >= 75) {
			OutputDebugStringA("LiteOn scan: invalid position response; pass incomplete.\n");
			return false;
		}
		const DWORD rawMsf = static_cast<DWORD>(buf[1]) * 60 * 75
			+ static_cast<DWORD>(buf[2]) * 75 + static_cast<DWORD>(buf[3]);
		const DWORD position = rawMsf >= 150 ? rawMsf - 150 : 0;
		const auto disposition = m_liteonPosition.Observe(position, rawMsf == 0);
		if (disposition == ScanPositionResult::Invalid) {
			OutputDebugStringA("LiteOn scan: position regressed or ended early; pass incomplete.\n");
			return false;
		}
		scanDone = m_liteonPosition.complete;
		// Completion markers and duplicate polls contain no new observation.
		if (disposition != ScanPositionResult::Sample) {
			currentLBA = scanDone ? m_liteonEndLBA : m_liteonPosition.previous;
			return true;
		}
		currentLBA = position;
		m_liteonLBA = position;
		c1 = (static_cast<int>(buf[4]) << 8) | buf[5];
		c2 = (static_cast<int>(buf[6]) << 8) | buf[7];
		// This protocol has no CU counter and no verified interval duration.
		if (sampleValid) *sampleValid = true;
		return true;
	}
	else {
		const auto interval = CdScanInterval::At(m_liteonLBA, m_liteonEndLBA);
		if (interval.sectors == 0) {
			currentLBA = m_liteonEndLBA;
			scanDone = true;
			return true;
		}

		// OLD: drive the head over this interval, then read the tallied counts.
		// Without the read the MediaTek counters never advance (verified on the
		// PX-891SAF PLUS). One interval is at most one CD second (75 sectors),
		// with a shorter final interval so the read never crosses lead-out.
		const bool coverageKnown = LiteOnScanDriveHead(interval.startLba, interval.sectors);

		std::vector<BYTE> buf(256, 0);
		BYTE cdb[12] = {};

		// 1. Latch interval counters: 0xDF/0x82/0x09
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0x82; cdb[2] = 0x09;
		if (!sendPollCommand(cdb, 12, buf.data(), 256, "0xDF/0x82/0x09 latch")) {
			return false;
		}

		// 2. Get data: 0xDF/0x82/0x05
		std::fill(buf.begin(), buf.end(), BYTE(0));
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0x82; cdb[2] = 0x05;
		if (!sendPollCommand(cdb, 12, buf.data(), 256, "0xDF/0x82/0x05 data")) {
			return false;
		}

		c1 = (static_cast<int>(buf[0]) << 8) | buf[1];   // BLER
		c2 = (static_cast<int>(buf[2]) << 8) | buf[3];   // E22
		cu = buf[4];                                       // E32/CU

		// 3. Reset interval: 0xDF/0x97
		memset(cdb, 0, 12); cdb[0] = 0xDF; cdb[1] = 0x97;
		if (!sendPollCommand(cdb, 12, buf.data(), 256, "0xDF/0x97 reset")) {
			return false; // subsequent counts could include earlier intervals
		}

		// All samples use interval starts. Mixing exclusive endpoints with an
		// inclusive final end created a false gap in sustained-C1 analysis.
		if (measuredSectors && coverageKnown) *measuredSectors = interval.sectors;
		if (sampleValid) *sampleValid = true;
		currentLBA = interval.startLba;
		scanDone = interval.final;
		m_liteonPosition.complete = scanDone;
		if (!scanDone)
			m_liteonLBA += interval.sectors;
		return true;
	}
}

bool ScsiDrive::LiteOnScanStop() {
	const bool wasActive = m_liteonScanActive;
	if (wasActive && m_liteonScanMethod == LiteOnScanMethod::MeasuredIntervals) {
		BYTE cdb[12] = {};
		cdb[0] = 0xDF; cdb[1] = 0xA3; cdb[2] = 0x01;
		std::vector<BYTE> buf(256, 0);
		BYTE sk = 0, asc = 0, ascq = 0;
		if (!SendSCSIWithSense(cdb, 12, buf.data(), 256, &sk, &asc, &ascq)) return false;
	}
	m_liteonScanActive = false;
	return true;
}
