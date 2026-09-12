#pragma once
#include "OpticalDrive.h"
#include <string>

// Runs a single operation: 0 = completed, 1 = failed/incomplete/unsupported,
// 2 = cancelled. Every nonzero result stops a batch. `choice` is a stable
// operation ID; the GUI maps displayed button numbers onto these IDs.
int DispatchMenuChoice(OpticalDrive& copier, DiscInfo& disc,
                       const std::wstring& workDir, wchar_t& audioDrive,
                       bool& hasTOC, int choice);
