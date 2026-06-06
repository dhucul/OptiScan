#include "FileUtils.h"
#include "ConsoleColors.h"
#include <windows.h>
#include <iostream>

std::wstring GetWorkingDirectory() {
	std::wstring dir(MAX_PATH, L'\0');
	DWORD len = GetModuleFileNameW(nullptr, &dir[0], static_cast<DWORD>(dir.size()));

	if (len == 0) {
		Console::Warning("Failed to get module path, using current directory\n");
		dir.resize(MAX_PATH);
		len = GetCurrentDirectoryW(static_cast<DWORD>(dir.size()), &dir[0]);
		if (len == 0) {
			return L".";
		}
		dir.resize(len);
		return dir;
	}

	// Fix: Check for truncation immediately (len == size means possible truncation)
	if (len >= dir.size()) {
		dir.resize(32767);
		len = GetModuleFileNameW(nullptr, &dir[0], static_cast<DWORD>(dir.size()));
		if (len == 0 || len >= dir.size()) {
			Console::Warning("Path too long, using current directory\n");
			return L".";
		}
	}

	dir.resize(len);
	size_t pos = dir.find_last_of(L"\\/");
	return (pos != std::wstring::npos) ? dir.substr(0, pos) : dir;
}

bool CreateDirectoryRecursive(const std::wstring& path) {
	std::wstring workPath = path;
	if (path.length() > MAX_PATH - 12 && path.substr(0, 4) != L"\\\\?\\") {
		if (path.length() >= 2 && path[1] == L':') {
			workPath = L"\\\\?\\" + path;
		}
		else if (path.substr(0, 2) == L"\\\\") {
			workPath = L"\\\\?\\UNC\\" + path.substr(2);
		}
	}

	size_t startPos = 0;
	if (workPath.substr(0, 4) == L"\\\\?\\") {
		startPos = 4;
		if (workPath.substr(4, 4) == L"UNC\\") {
			startPos = 8;
		}
	}

	size_t pos = startPos;
	while ((pos = workPath.find_first_of(L"\\/", pos + 1)) != std::wstring::npos) {
		std::wstring subPath = workPath.substr(0, pos);
		DWORD attrs = GetFileAttributesW(subPath.c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES) {
			if (!CreateDirectoryW(subPath.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
				return false;
		}
	}
	DWORD attrs = GetFileAttributesW(workPath.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES) {
		return CreateDirectoryW(workPath.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
	}
	return true;
}

std::wstring SanitizeFilename(const std::wstring& name) {
	std::wstring result = name;
	const wchar_t* invalid = L"<>:\"/\\|?*";
	for (wchar_t& c : result) {
		if (wcschr(invalid, c)) c = L'_';
		if (c < 0x20) c = L'_';
	}

	while (!result.empty() && (result.back() == L' ' || result.back() == L'.')) {
		result.pop_back();
	}

	if (result.empty()) {
		result = L"AudioCD";
	}

	return result;
}

std::wstring NormalizePath(const std::wstring& path) {
	std::wstring result = path;
	while (!result.empty() && (result.front() == L' ' || result.front() == L'\t'))
		result.erase(0, 1);
	while (!result.empty() && (result.back() == L' ' || result.back() == L'\t'))
		result.pop_back();
	return result;
}