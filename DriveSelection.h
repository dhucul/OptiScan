#pragma once
#include <vector>

wchar_t SelectAudioDrive(const std::vector<wchar_t>& audioDrives);

// Drive picker for burning. Lists all CD/DVD drives with vendor/model and
// current media status, and lets the user pick one (with `defaultLetter`
// preselected when present in the list). Returns 0 on cancel.
wchar_t SelectWriterDrive(const std::vector<wchar_t>& cdDrives,
                          wchar_t defaultLetter = 0);