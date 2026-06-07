#pragma once
#include "OpticalDrive.h"
#include <string>
#include <vector>
#include <windows.h>

namespace WriteDiscInternal {
	// Drive readiness and cache
	bool WaitForDriveReady(ScsiDrive& drive, int timeoutSeconds);
	bool SynchronizeCache(ScsiDrive& drive);

	// Subchannel helpers
	void DeinterleaveSubchannel(const BYTE* raw, BYTE* packed);
	size_t FindTrackForSector(const std::vector<OpticalDrive::TrackWriteInfo>& tracks,
		DWORD binSector, bool& isInPregap);

	// Drive configuration
	bool PrepareDriveForWrite(ScsiDrive& drive, int subchannelMode, bool quiet = false);
	bool BuildAndSendCueSheet(ScsiDrive& drive,
		const std::vector<OpticalDrive::TrackWriteInfo>& tracks,
		DWORD totalSectors, int subchannelMode, bool verbose = true, bool quiet = false,
		bool cdTextInLeadIn = false);

	// CD-Text
	bool HasCDTextContent(const std::string& discTitle,
		const std::string& discPerformer,
		const std::vector<OpticalDrive::TrackWriteInfo>& tracks);

	std::vector<BYTE> BuildCDTextPacks(const std::string& discTitle,
		const std::string& discPerformer,
		const std::vector<OpticalDrive::TrackWriteInfo>& tracks);

	bool SendCDTextToDevice(ScsiDrive& drive, const std::vector<BYTE>& packs);
	bool SendCDTextLeadInToDevice(ScsiDrive& drive, const std::vector<BYTE>& packs,
		bool* wroteAnyLeadInFrame = nullptr);
}
