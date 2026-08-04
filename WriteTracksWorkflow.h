// ============================================================================
// WriteTracksWorkflow.h - Write ripped track files to disc using the
// pregap layout of the disc currently in the drive.
// ============================================================================
#pragma once

#include "OpticalDrive.h"
#include <string>

// Builds a temporary .bin/.cue from the WAV/FLAC files in a folder, using
// pregap durations from the source disc's TOC, then writes a new disc.
//
// The source disc must be inserted when the workflow starts (its TOC is read
// from `disc`). After the source-disc reads complete, the user is offered a
// burner-drive picker; on a same-drive swap they're prompted to insert a
// blank, on a cross-drive swap `audioDrive` is updated in place. When supplied,
// `outDriveOrMediaChanged` tells the caller whether its cached TOC is still safe
// after a cancellation or failure.
void RunWriteTracksWorkflow(OpticalDrive& copier, DiscInfo& disc,
	const std::wstring& workDir, wchar_t& audioDrive,
	bool* outCompleted = nullptr,
	bool* outDriveOrMediaChanged = nullptr);
