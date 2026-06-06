#define NOMINMAX
#include "OpticalDrive.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>

// ============================================================================
// Speed Comparison Test - Compares read errors at slow vs fast speed
// ============================================================================

bool OpticalDrive::RunSpeedComparisonTest(DiscInfo& disc, std::vector<SpeedComparisonResult>& results) {
	std::cout << "\n=== Speed Comparison Test ===\n";
	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: C2 support required.\n";
		return false;
	}

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) return false;

	// Don't trust requested multipliers — many drives ignore them and clamp to
	// their own floor. Resolve the two operating points the drive will actually
	// honor, then read back what each request really became (KB/s -> ~x) and
	// label the report with the *actual* speeds, not the requested ones.
	auto kbpsToX = [](WORD kbps) -> int {
		if (kbps == 0 || CD_SPEED_1X == 0) return 0;
		return (static_cast<int>(kbps) + CD_SPEED_1X / 2) / CD_SPEED_1X;
	};

	int lowReq = m_drive.GetLowestHonoredSpeed(/*apply=*/false);

	// lowReq is already the rounded floor multiplier the drive honors, so use it
	// directly as the low operating point — no need to re-probe it. Only the high
	// point needs a readback to see what "max" actually resolves to.
	int lowActualX = lowReq;

	WORD highActualKbps = 0, dummyWrite = 0;
	m_drive.TrySetSpeedAndVerify(0, -1, &highActualKbps, &dummyWrite);  // 0 = max
	Sleep(100);
	int highActualX = kbpsToX(highActualKbps);

	// If the drive clamped both requests to the same actual speed, a slow-vs-fast
	// comparison is meaningless. Report that and stop rather than fabricating a
	// ratio between two identical reads.
	if (highActualX > 0 && lowActualX > 0 && highActualX == lowActualX) {
		m_drive.SetSpeed(0);
		std::cout << "This drive runs every request at ~" << lowActualX
		          << "x regardless of the requested speed, so a slow-vs-fast\n"
		          << "comparison is not possible on this hardware.\n";
		results.clear();
		return true;
	}

	// Label the high point honestly: if the readback failed, call it "max".
	char lowLabel[16], highLabel[16];
	snprintf(lowLabel, sizeof(lowLabel), "%dx", lowActualX);
	if (highActualX > 0) snprintf(highLabel, sizeof(highLabel), "%dx", highActualX);
	else                 snprintf(highLabel, sizeof(highLabel), "max");

	int sampleInterval = std::max(1, static_cast<int>(totalSectors / 100));
	int totalSamples = std::max(1, static_cast<int>(totalSectors / sampleInterval) + 1);
	results.clear();
	int tested = 0;

	std::cout << "Testing ~" << totalSamples << " sample sectors at "
	          << lowLabel << " vs " << highLabel << "...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Speed Test");
	progress.Start();

	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		for (DWORD lba = start; lba <= t.endLBA; lba += sampleInterval) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				std::cout << "\n\n*** Test cancelled by user ***\n";
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			SpeedComparisonResult r = { lba, 0, 0, false };
			bool lowOk = false, highOk = false;

			DefeatDriveCache(lba, t.endLBA);

			m_drive.SetSpeed(lowReq);
			Sleep(100);
			lowOk = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, r.lowSpeedC2);

			DefeatDriveCache(lba, t.endLBA);

			m_drive.SetSpeed(0);  // max
			Sleep(100);
			highOk = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, r.highSpeedC2);

			if (!lowOk) r.lowSpeedC2 = -1;
			if (!highOk) r.highSpeedC2 = -1;

			// Improved inconsistency detection: more sensitive to small error counts
			r.inconsistent =
				(!lowOk || !highOk) ||
				(r.lowSpeedC2 == 0 && r.highSpeedC2 > 0) ||
				(r.highSpeedC2 == 0 && r.lowSpeedC2 > 0) ||
				(r.highSpeedC2 > 0 && r.highSpeedC2 > r.lowSpeedC2 * 2) ||
				(r.lowSpeedC2 > 0 && r.lowSpeedC2 > r.highSpeedC2 * 2);

			if (r.lowSpeedC2 != 0 || r.highSpeedC2 != 0) results.push_back(r);
			tested++;
			progress.Update(tested, totalSamples);
		}
	}
	progress.Finish(true); m_drive.SetSpeed(0);
	m_drive.SpinDown();

	int lowSpeedErrors = 0, highSpeedErrors = 0, inconsistentCount = 0;
	int lowFailures = 0, highFailures = 0;
	for (const auto& r : results) {
		if (r.lowSpeedC2 < 0) lowFailures++;
		else lowSpeedErrors += r.lowSpeedC2;
		if (r.highSpeedC2 < 0) highFailures++;
		else highSpeedErrors += r.highSpeedC2;
		if (r.inconsistent) inconsistentCount++;
	}

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              SPEED COMPARISON REPORT\n";
	std::cout << std::string(60, '=') << "\n";
	std::cout << "  (Compares read errors at slow vs fast speed to find optimal rip speed)\n";
	std::cout << "  Speeds shown are what the drive ACTUALLY ran, not what was requested.\n\n";

	std::cout << "--- Results ---\n";
	std::cout << "  Sectors tested:        " << tested << "\n";
	std::cout << "  Low speed  (" << lowLabel << ") C2:  " << lowSpeedErrors;
	if (lowFailures > 0) std::cout << " (+" << lowFailures << " read failures)";
	std::cout << "\n";
	std::cout << "  High speed (" << highLabel << ") C2:  " << highSpeedErrors;
	if (highFailures > 0) std::cout << " (+" << highFailures << " read failures)";
	std::cout << "\n";
	std::cout << "  Inconsistent sectors:  " << inconsistentCount;
	if (tested > 0)
		std::cout << " (" << std::fixed << std::setprecision(1)
		<< (inconsistentCount * 100.0 / tested) << "%)";
	std::cout << "\n";

	// Improved error ratio reporting: handles asymmetric failures
	if (highSpeedErrors > 0 || lowSpeedErrors > 0) {
		if (highSpeedErrors > 0 && lowSpeedErrors > 0) {
			double ratio = static_cast<double>(highSpeedErrors) / lowSpeedErrors;
			std::cout << "  Error ratio (" << highLabel << "/" << lowLabel << "): "
			          << std::fixed << std::setprecision(1) << ratio << "x";
			if (ratio > 3.0) std::cout << "  (high speed significantly worse)";
			else if (ratio < 0.5) std::cout << "  (low speed worse - unusual)";
			else std::cout << "  (similar at both speeds)";
			std::cout << "\n";
		}
		else if (highSpeedErrors > 0 && lowSpeedErrors == 0) {
			std::cout << "  Error ratio (" << highLabel << "/" << lowLabel
			          << "): HIGH (errors only at high speed)\n";
		}
		else if (lowSpeedErrors > 0 && highSpeedErrors == 0) {
			std::cout << "  Error ratio (" << highLabel << "/" << lowLabel
			          << "): <0.1 (errors only at low speed - unusual)\n";
		}
	}

	std::cout << "\n--- Recommendation ---\n  Optimal rip speed: ";
	if (lowFailures > 0 || highFailures > 0) {
		std::cout << lowLabel << "\n";
		std::cout << "  Read failures detected. Use the slowest reliable speed.\n";
	}
	else if (results.empty() || (lowSpeedErrors == 0 && highSpeedErrors == 0)) {
		std::cout << "Any (" << highLabel << " safe)\n";
		std::cout << "  Disc reads cleanly at all speeds. No benefit from slowing down.\n";
	}
	else if (highSpeedErrors > lowSpeedErrors * 2 || inconsistentCount > tested / 10) {
		std::cout << lowLabel << "\n";
		std::cout << "  Significant error increase at high speed. Slow rip strongly recommended.\n";
	}
	else if (highSpeedErrors > lowSpeedErrors) {
		std::cout << lowLabel << "\n";
		std::cout << "  Error increase at high speed. Speeds above " << lowLabel << " not recommended.\n";
	}
	else {
		std::cout << "Any (" << highLabel << " safe)\n";
		std::cout << "  No significant benefit from slower speeds.\n";
	}
	std::cout << std::string(60, '=') << "\n";

	return true;
}