#pragma once
#include "OpticalDrive.h"
#include <string>

// Drive-independent recovery rip: reads the disc with the consensus engine
// (per-byte majority voting + jitter alignment, hybrid C2 tie-break) and writes
// a .bin/.cue(/.sub) image plus a recovery report. Modeled on RunCopyWorkflow.
bool RunRecoveryRipWorkflow(OpticalDrive& copier, DiscInfo& disc,
	const std::wstring& workDir);
