// ============================================================================
// WriteFromCueWorkflow.h - Write an audio CD from a CUE sheet plus the WAV or
// FLAC files it references.
// ============================================================================
#pragma once

#include "OpticalDrive.h"
#include <string>

// Burns an audio CD from a CUE sheet that names WAV/FLAC audio files. No source
// disc is read at any point, so this runs with nothing but a blank in the
// drive. Both CUE layouts are accepted: multi-FILE (one audio file per track,
// with index times relative to each file) and single-FILE (one image split by
// INDEX offsets).
//
// The CUE is resolved into a temporary .bin/.cue pair in `workDir` and handed
// to the existing WriteDisc() pipeline, so blanking, capacity checks, CD-Text,
// the raw cue-sheet burn and the IMAPI fallback all behave exactly as they do
// for a hand-supplied image. `audioDrive` is updated in place when the user
// picks a different burner.
void RunWriteFromCueWorkflow(OpticalDrive& copier, const std::wstring& workDir,
	wchar_t& audioDrive, bool* outCompleted = nullptr);
