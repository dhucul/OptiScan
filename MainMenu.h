#pragma once
#include "OpticalDrive.h"
#include <string>

// Runs a single menu action. Returns 0 normally, non-zero on fatal error
// (e.g. drive could not be reopened).  `choice` is the original 1..30 menu
// number from the legacy console menu; the GUI maps button clicks onto it.
int DispatchMenuChoice(OpticalDrive& copier, DiscInfo& disc,
                       const std::wstring& workDir, wchar_t& audioDrive,
                       bool& hasTOC, int choice);