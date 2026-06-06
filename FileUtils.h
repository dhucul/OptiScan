#pragma once
#include <string>

std::wstring GetWorkingDirectory();
bool CreateDirectoryRecursive(const std::wstring& path);
std::wstring SanitizeFilename(const std::wstring& name);
std::wstring NormalizePath(const std::wstring& path);