#include "FileUtils.h"
#include "ConsoleColors.h"
#include <windows.h>
#include <shlobj.h>
#include <iostream>

namespace {

bool DirectoryIsWritable(const std::wstring& directory) {
	if (directory.empty()) return false;
	wchar_t suffix[80] = {};
	swprintf_s(suffix, L"\\.optiscan-write-test-%lu-%llu.tmp",
		GetCurrentProcessId(), GetTickCount64());
	const std::wstring probePath = directory + suffix;
	HANDLE probe = CreateFileW(probePath.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (probe == INVALID_HANDLE_VALUE) return false;
	CloseHandle(probe);
	return true;
}

std::wstring KnownFolderOutput(REFKNOWNFOLDERID folderId,
	const wchar_t* childPath) {
	wchar_t* base = nullptr;
	if (FAILED(SHGetKnownFolderPath(folderId, KF_FLAG_CREATE, nullptr, &base)))
		return {};
	std::wstring path(base);
	CoTaskMemFree(base);
	path += childPath;
	if (!CreateDirectoryRecursive(path) || !DirectoryIsWritable(path))
		return {};
	return path;
}

} // namespace

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
		if (DirectoryIsWritable(dir)) return dir;
	}

	// Fix: Check for truncation immediately (len == size means possible truncation)
	if (len >= dir.size()) {
		dir.resize(32767);
		len = GetModuleFileNameW(nullptr, &dir[0], static_cast<DWORD>(dir.size()));
		if (len == 0 || len >= dir.size()) {
			Console::Warning("Path too long, using current directory\n");
			dir.clear();
		}
	}

	if (len > 0 && len < dir.size()) {
		dir.resize(len);
		size_t pos = dir.find_last_of(L"\\/");
		if (pos != std::wstring::npos) dir.resize(pos);
		if (DirectoryIsWritable(dir)) return dir;
	}

	// Installed builds normally live under Program Files, which is read-only
	// for a non-elevated user. Keep portable builds writing beside the EXE, but
	// place installed-build reports in a visible per-user folder.
	std::wstring output = KnownFolderOutput(FOLDERID_Documents, L"\\OptiScan");
	if (!output.empty()) return output;
	output = KnownFolderOutput(FOLDERID_LocalAppData, L"\\OptiScan\\Output");
	if (!output.empty()) return output;

	wchar_t tempPath[MAX_PATH] = {};
	DWORD tempLen = GetTempPathW(MAX_PATH, tempPath);
	if (tempLen > 0 && tempLen < MAX_PATH) {
		output.assign(tempPath, tempLen);
		while (!output.empty() && (output.back() == L'\\' || output.back() == L'/'))
			output.pop_back();
		output += L"\\OptiScan";
		if (CreateDirectoryRecursive(output) && DirectoryIsWritable(output))
			return output;
	}

	return L".";
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
	const bool uncPath = workPath.compare(0, 2, L"\\\\") == 0 &&
		workPath.compare(0, 4, L"\\\\?\\") != 0;
	const bool extendedUnc = workPath.compare(0, 8, L"\\\\?\\UNC\\") == 0;
	if (uncPath || extendedUnc) {
		// Do not try to create the UNC server itself.  For an ordinary UNC
		// path, begin the component walk just after the leading two slashes;
		// extended UNC paths already start after "\\?\UNC\" above.
		if (uncPath) startPos = 2;
		const size_t serverEnd = workPath.find_first_of(L"\\/", startPos);
		if (serverEnd == std::wstring::npos) return false;
		const size_t shareEnd = workPath.find_first_of(L"\\/", serverEnd + 1);
		if (shareEnd == std::wstring::npos) {
			DWORD attrs = GetFileAttributesW(workPath.c_str());
			return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
		}
		startPos = shareEnd;
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
