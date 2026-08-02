#pragma once
#include "OpticalDrive.h"
#include <string>
#include <vector>

enum class CopyVerificationStatus {
	NotPerformed,
	Verified,
	Mismatch,
	Inconclusive
};

bool RunCopyWorkflow(OpticalDrive& copier, DiscInfo& disc,
	const std::wstring& workDir,
	CopyVerificationStatus* outVerificationStatus = nullptr);
// `audioDrive` is updated in place when the user picks a different burner drive
// at the start of the workflow (so callers' cached drive letter stays in sync).
void RunWriteDiscWorkflow(OpticalDrive& copier, const std::wstring& workDir,
	                          wchar_t& audioDrive, bool* outCompleted = nullptr);
