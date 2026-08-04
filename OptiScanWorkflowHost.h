#pragma once

#include "framework.h"
#include "OpticalDrive.h"
#include <string>
#include <vector>

// Disc / drive state retained across GUI button clicks.
extern OpticalDrive g_copier;
extern DiscInfo g_disc;
extern std::wstring g_workDir;
extern wchar_t g_audioDrive;
extern bool g_hasTOC;
extern bool g_driveOpen;

bool EnsureDriveOpen(HWND hOwner, bool* outFreshlyScanned = nullptr,
                     bool needAudioDisc = true, bool readToc = true);
bool ButtonNeedsAudioDisc(int btnIndex);
bool ButtonNeedsDrive(int btnIndex);
bool ButtonDisablesMenu(int btnIndex);
bool ButtonNeedsPrescan(int btnIndex);
void Prescan();
bool RefreshDisc();
bool ReselectSourceDriveIfMultiple();
int ButtonToMenuChoice(int btnIndex);
std::vector<int> ParseBatchChoices(const std::string& input);
