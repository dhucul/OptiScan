// ============================================================================
// DriveOffsetDatabase.cpp - Full AccurateRip drive offset database loader
// ============================================================================
#include "DriveOffsetDatabase.h"
#include "DriveOffsets.h"
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>

#pragma comment(lib, "winhttp.lib")

DriveOffsetDatabase& DriveOffsetDatabase::Instance() {
	static DriveOffsetDatabase instance;
	return instance;
}

std::string DriveOffsetDatabase::ToUpper(const std::string& s) {
	std::string result = s;
	for (char& c : result) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
	return result;
}

std::string DriveOffsetDatabase::GetCachePath() const {
	wchar_t* appDataPath = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appDataPath))) {
		const int wideLength = static_cast<int>(wcslen(appDataPath));
		const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			appDataPath, wideLength, nullptr, 0, nullptr, nullptr);
		if (length <= 0) { CoTaskMemFree(appDataPath); return "driveoffsets.csv"; }
		std::string narrow(static_cast<size_t>(length), '\0');
		if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, appDataPath, wideLength,
			narrow.data(), length, nullptr, nullptr) != length) {
			CoTaskMemFree(appDataPath);
			return "driveoffsets.csv";
		}
		CoTaskMemFree(appDataPath);

		std::string dir = narrow + "\\OptiScan";
		std::filesystem::create_directories(dir);
		return dir + "\\driveoffsets.csv";
	}
	return "driveoffsets.csv";
}

bool DriveOffsetDatabase::Load() {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_loaded) return true;

	std::string cachePath = GetCachePath();

	// Try loading from local cache first (use if less than 30 days old)
	if (std::filesystem::exists(cachePath)) {
		auto lastWrite = std::filesystem::last_write_time(cachePath);
		auto now = std::filesystem::file_time_type::clock::now();
		auto age = std::chrono::duration_cast<std::chrono::hours>(now - lastWrite);

		if (age.count() < 30 * 24 && LoadCache(cachePath)) {
			m_loaded = true;
			return true;
		}
	}

	// Try downloading fresh database
	std::string html;
	if (DownloadDatabase(html) && ParseHTML(html)) {
		SaveCache(cachePath);
		m_loaded = true;
		return true;
	}

	// Try stale cache as fallback
	if (std::filesystem::exists(cachePath) && LoadCache(cachePath)) {
		m_loaded = true;
		return true;
	}

	// Last resort: load hardcoded entries from DriveOffsets.h
	m_entries.clear();
	for (int i = 0; knownOffsets[i].vendor != nullptr; i++) {
		DriveOffsetRecord rec;
		rec.vendor = knownOffsets[i].vendor;
		rec.model = knownOffsets[i].model;
		rec.offset = knownOffsets[i].offset;
		rec.submissions = 0;
		m_entries.push_back(std::move(rec));
	}
	// Keep retrying cache/download on later lookups; the built-in table remains
	// available in m_entries in the meantime.
	m_loaded = false;
	return true;
}

bool DriveOffsetDatabase::Refresh() {
	std::lock_guard<std::mutex> lock(m_mutex);
	std::string html;
	if (!DownloadDatabase(html) || !ParseHTML(html)) return false;
	SaveCache(GetCachePath());
	m_loaded = true;
	return true;
}

bool DriveOffsetDatabase::DownloadDatabase(std::string& html) {
	HINTERNET hSession = WinHttpOpen(L"OptiScan/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
	if (!hSession) return false;

	WinHttpSetTimeouts(hSession, 5000, 15000, 10000, 10000);

	HINTERNET hConnect = WinHttpConnect(hSession, L"www.accuraterip.com",
		INTERNET_DEFAULT_HTTP_PORT, 0);
	if (!hConnect) {
		WinHttpCloseHandle(hSession);
		return false;
	}

	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/driveoffsets.htm",
		nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);

	bool success = false;
	if (hRequest && WinHttpSendRequest(hRequest, nullptr, 0, nullptr, 0, 0, 0) &&
		WinHttpReceiveResponse(hRequest, nullptr)) {

		DWORD statusCode = 0, statusSize = sizeof(statusCode);
		WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			nullptr, &statusCode, &statusSize, nullptr);

		if (statusCode == 200) {
			html.clear();
			DWORD bytesAvailable = 0;
			while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
				std::vector<char> buffer(bytesAvailable);
				DWORD bytesRead = 0;
				if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
					html.append(buffer.data(), bytesRead);
				}
			}
			success = !html.empty();

			// Save raw HTML for debugging
			if (success) {
				std::string debugPath = GetCachePath();
				debugPath = debugPath.substr(0, debugPath.rfind('\\')) + "\\driveoffsets_raw.htm";
				std::ofstream dbg(debugPath, std::ios::binary);
				if (dbg.is_open()) dbg.write(html.data(), html.size());
			}
		}
	}

	if (hRequest) WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	return success;
}

// Convert HTML to lowercase for case-insensitive tag matching
static std::string ToLowerHTML(const std::string& html) {
	std::string result = html;
	bool inTag = false;
	for (char& c : result) {
		if (c == '<') inTag = true;
		if (inTag) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		if (c == '>') inTag = false;
	}
	return result;
}

// Strip HTML entities and replace with spaces
static std::string DecodeEntities(const std::string& text) {
	std::string result;
	result.reserve(text.size());
	size_t i = 0;
	while (i < text.size()) {
		if (text[i] == '&') {
			size_t semi = text.find(';', i);
			if (semi != std::string::npos && semi - i < 10) {
				std::string entity = text.substr(i, semi - i + 1);
				if (entity == "&nbsp;" || entity == "&#160;") result += ' ';
				else if (entity == "&amp;") result += '&';
				else if (entity == "&lt;") result += '<';
				else if (entity == "&gt;") result += '>';
				else if (entity == "&quot;") result += '"';
				else result += ' ';
				i = semi + 1;
				continue;
			}
		}
		result += text[i++];
	}
	return result;
}

// Extract ALL text content from a cell, skipping inner HTML tags
// Uses lhtml (lowered tags) to find </td> case-insensitively
static std::string ExtractCellText(const std::string& html, const std::string& lhtml, size_t& pos) {
	// Find the opening > of the <td> tag
	size_t start = html.find('>', pos);
	if (start == std::string::npos) return "";
	start++;

	// Find the closing </td> using the lowercase copy for case-insensitive match
	size_t tdEnd = lhtml.find("</td>", start);
	if (tdEnd == std::string::npos) return "";

	// Concatenate all text segments, skipping inner tags
	std::string text;
	size_t cur = start;
	while (cur < tdEnd) {
		size_t tagOpen = html.find('<', cur);
		if (tagOpen == std::string::npos || tagOpen >= tdEnd) {
			text += html.substr(cur, tdEnd - cur);
			break;
		}
		text += html.substr(cur, tagOpen - cur);
		size_t tagClose = html.find('>', tagOpen);
		if (tagClose == std::string::npos || tagClose >= tdEnd) break;
		cur = tagClose + 1;
	}

	pos = tdEnd;

	// Decode HTML entities
	text = DecodeEntities(text);

	// Trim whitespace
	size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return "";
	size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

// Strip non-numeric characters except +, -, and digits for integer parsing
static int ParseInt(const std::string& text, bool& ok) {
	std::string cleaned;
	for (char c : text) {
		if (c == '+' || c == '-' || (c >= '0' && c <= '9')) {
			cleaned += c;
		}
		// Skip commas, brackets, spaces, etc.
	}
	if (cleaned.empty()) { ok = false; return 0; }
	try {
		int val = std::stoi(cleaned);
		ok = true;
		return val;
	}
	catch (...) { ok = false; return 0; }
}

bool DriveOffsetDatabase::ParseHTML(const std::string& html) {
	// Lowercase all HTML tags for case-insensitive matching
	std::string lhtml = ToLowerHTML(html);

	std::vector<DriveOffsetRecord> entries;

	size_t pos = 0;
	while (pos < lhtml.size()) {
		// Find next table row
		size_t rowStart = lhtml.find("<tr", pos);
		if (rowStart == std::string::npos) break;

		size_t rowEnd = lhtml.find("</tr>", rowStart);
		if (rowEnd == std::string::npos) break;

		// Extract all <td> cells in this row
		std::vector<std::string> cells;
		size_t cellPos = rowStart;
		while (cellPos < rowEnd) {
			size_t tdStart = lhtml.find("<td", cellPos);
			if (tdStart == std::string::npos || tdStart >= rowEnd) break;

			// Use original HTML for text content (preserve case in drive names)
			size_t origTdStart = tdStart;
			std::string text = ExtractCellText(html, lhtml, origTdStart);
			cells.push_back(text);

			// Advance in lowercase HTML past this cell
			size_t tdEndTag = lhtml.find("</td>", tdStart);
			cellPos = (tdEndTag != std::string::npos) ? tdEndTag + 5 : tdStart + 1;
		}

		// AccurateRip column order: [DriveName, Offset, Submissions, Percentage]
		// Find the drive name cell (first cell containing alphabetic characters)
		if (cells.size() >= 3) {
			int driveIdx = -1;
			for (int c = 0; c < static_cast<int>(cells.size()); c++) {
				for (char ch : cells[c]) {
					if (std::isalpha(static_cast<unsigned char>(ch))) {
						driveIdx = c;
						break;
					}
				}
				if (driveIdx >= 0) break;
			}

			if (driveIdx < 0) { pos = rowEnd + 5; continue; }

			// Skip header row (contains "Correction Offset" or "CD Drive")
			std::string upperCell = ToUpper(cells[driveIdx]);
			if (upperCell.find("CD DRIVE") != std::string::npos ||
				upperCell.find("CORRECTION OFFSET") != std::string::npos) {
				pos = rowEnd + 5;
				continue;
			}

			// Find offset: first numeric cell after the drive name
			bool ok = false;
			int offsetIdx = -1;
			int offsetVal = 0;
			for (int c = driveIdx + 1; c < static_cast<int>(cells.size()); c++) {
				offsetVal = ParseInt(cells[c], ok);
				if (ok) { offsetIdx = c; break; }
			}
			if (offsetIdx < 0) { pos = rowEnd + 5; continue; }

			// Find submissions: next numeric cell after offset
			int subVal = 0;
			for (int c = offsetIdx + 1; c < static_cast<int>(cells.size()); c++) {
				subVal = ParseInt(cells[c], ok);
				if (ok) break;
			}

			// Split "Vendor - Model" on first " - "
			DriveOffsetRecord rec;
			rec.offset = offsetVal;
			rec.submissions = subVal;

			std::string driveName = cells[driveIdx];
			size_t dashPos = driveName.find(" - ");
			if (dashPos != std::string::npos) {
				rec.vendor = driveName.substr(0, dashPos);
				rec.model = driveName.substr(dashPos + 3);
			}
			else {
				rec.vendor = "";
				rec.model = driveName;
			}

			// Trim vendor/model
			auto trim = [](std::string& s) {
				size_t f = s.find_first_not_of(" \t\r\n");
				size_t l = s.find_last_not_of(" \t\r\n");
				s = (f == std::string::npos) ? "" : s.substr(f, l - f + 1);
			};
			trim(rec.vendor);
			trim(rec.model);

			if (!rec.model.empty()) {
				entries.push_back(std::move(rec));
			}
		}

		pos = rowEnd + 5;
	}

	if (!entries.empty()) {
		m_entries = std::move(entries);
		return true;
	}
	return false;
}

bool DriveOffsetDatabase::SaveCache(const std::string& path) const {
	std::ofstream file(path);
	if (!file.is_open()) return false;

	file << "# OptiScan Drive Offset Cache\n";
	file << "# vendor\tmodel\toffset\tsubmissions\n";
	for (const auto& e : m_entries) {
		auto flatten = [](std::string value) {
			for (char& ch : value) if (ch == '\t' || ch == '\r' || ch == '\n') ch = ' ';
			return value;
		};
		file << flatten(e.vendor) << "\t" << flatten(e.model) << "\t"
			<< e.offset << "\t" << e.submissions << "\n";
	}
	return true;
}

bool DriveOffsetDatabase::LoadCache(const std::string& path) {
	std::ifstream file(path);
	if (!file.is_open()) return false;

	std::vector<DriveOffsetRecord> entries;
	std::string line;
	while (std::getline(file, line)) {
		if (line.empty() || line[0] == '#') continue;

		std::istringstream ss(line);
		DriveOffsetRecord rec;
		std::string offsetStr, submStr;

		if (!std::getline(ss, rec.vendor, '\t')) continue;
		if (!std::getline(ss, rec.model, '\t')) continue;
		if (!std::getline(ss, offsetStr, '\t')) continue;
		std::getline(ss, submStr, '\t');

		try { rec.offset = std::stoi(offsetStr); }
		catch (...) { continue; }

		try { rec.submissions = submStr.empty() ? 0 : std::stoi(submStr); }
		catch (...) { rec.submissions = 0; }

		entries.push_back(std::move(rec));
	}

	if (!entries.empty()) {
		m_entries = std::move(entries);
		return true;
	}
	return false;
}

bool DriveOffsetDatabase::Lookup(const std::string& vendor, const std::string& model,
	DriveOffsetInfo& info) const {
	std::string upperVendor = ToUpper(vendor);
	std::string upperModel = ToUpper(model);
	// Build a combined name to match against unsplit database entries
	std::string upperCombined = upperVendor + " " + upperModel;

	const DriveOffsetRecord* bestMatch = nullptr;

	for (const auto& entry : m_entries) {
		std::string entryVendor = ToUpper(entry.vendor);
		std::string entryModel = ToUpper(entry.model);

		if (entryModel.empty()) continue;

		bool matched = false;

		if (entryVendor.empty()) {
			// Database entry is a combined "Vendor Model" string — match against combined SCSI name
			std::string entryFull = entryModel;
			matched = (entryFull == upperCombined) ||
				entryFull.find(upperCombined) != std::string::npos ||
				upperCombined.find(entryFull) != std::string::npos;
		}
		else {
			// Database entry has separate vendor/model
			// Require vendor match only if both sides are long enough to be meaningful
			bool vendorMatch = false;
			if (upperVendor.size() >= 2 && entryVendor.size() >= 2) {
				vendorMatch = upperVendor.find(entryVendor) != std::string::npos ||
					entryVendor.find(upperVendor) != std::string::npos;
			}

			bool modelMatch = false;
			if (entryModel.size() >= 4) {
				modelMatch = upperModel.find(entryModel) != std::string::npos ||
					entryModel.find(upperModel) != std::string::npos;
			}

			matched = vendorMatch && modelMatch;
		}

		if (matched) {
			if (!bestMatch || entry.submissions > bestMatch->submissions) {
				bestMatch = &entry;
			}
		}
	}

	if (bestMatch) {
		info.readOffset = bestMatch->offset;
		info.fromDatabase = true;
		info.source = "AccurateRip Database (" + std::to_string(bestMatch->submissions) + " submissions)";
		return true;
	}
	return false;
}

bool DriveOffsetDatabase::GenerateHeader(const std::string& outputPath) const {
	if (m_entries.empty()) return false;

	std::ofstream file(outputPath);
	if (!file.is_open()) return false;

	file << "// ============================================================================\n";
	file << "// DriveOffsets.h - AccurateRip drive offset database\n";
	file << "// AUTO-GENERATED from http://www.accuraterip.com/driveoffsets.htm\n";
	file << "// Total entries: " << m_entries.size() << "\n";
	file << "// ============================================================================\n";
	file << "#pragma once\n\n";
	file << "struct DriveOffsetEntry {\n";
	file << "    const char* vendor;\n";
	file << "    const char* model;\n";
	file << "    int offset;\n";
	file << "};\n\n";
	file << "static const DriveOffsetEntry knownOffsets[] = {\n";

	for (const auto& e : m_entries) {
		std::string vendor = e.vendor;
		std::string model = e.model;

		// If vendor is empty, the model field contains "Vendor Model" combined.
		// Split at the first space before a known drive-type keyword.
		if (vendor.empty() && !model.empty()) {
			// Common drive-type prefixes that mark the start of the model string
			static const char* modelPrefixes[] = {
				"BD-R", "BD-RE", "BD-ROM", "BD RW",
				"DVDR", "DVD-R", "DVD-ROM", "DVD+", "DVD_RW", "DVD RW",
				"DVDRAM", "DVDRAW",
				"CDDVDW", "CD-R", "CD-ROM", "CD-RW",
				"BW-", "BC-", "BDR-", "DRW-", "SDRW-", "SBW-",
				"iHAS", "iHBS", "iHOS",
				"LTR-", "LTN",
				"SH-", "SE-",
				"ND-", "ND_",
				"DV-W", "DW",
				"CRW", "CRX",
				"CDD",
				"GDR", "GD-",
				"SD-M", "SD-",
				"PX-", "UJ",
				"Slim",
				nullptr
			};

			std::string upper = ToUpper(model);
			size_t bestSplit = std::string::npos;

			for (int p = 0; modelPrefixes[p] != nullptr; p++) {
				std::string prefix = ToUpper(std::string(modelPrefixes[p]));
				size_t found = upper.find(prefix);
				if (found != std::string::npos && found > 0) {
					// Ensure we split at a word boundary (space before the prefix)
					if (model[found - 1] == ' ') {
						if (bestSplit == std::string::npos || found < bestSplit) {
							bestSplit = found;
						}
					}
				}
			}

			if (bestSplit != std::string::npos && bestSplit > 0) {
				vendor = model.substr(0, bestSplit - 1);  // trim trailing space
				model = model.substr(bestSplit);
			}
			else {
				// Fallback: use first word as vendor
				size_t sp = model.find(' ');
				if (sp != std::string::npos) {
					vendor = model.substr(0, sp);
					model = model.substr(sp + 1);
				}
			}
		}

		// Escape quotes and backslashes
		for (auto it = vendor.begin(); it != vendor.end(); ++it) {
			if (*it == '"' || *it == '\\') { it = vendor.insert(it, '\\'); ++it; }
		}
		for (auto it = model.begin(); it != model.end(); ++it) {
			if (*it == '"' || *it == '\\') { it = model.insert(it, '\\'); ++it; }
		}

		file << "    {\"" << vendor << "\", \"" << model << "\", ";
		if (e.offset >= 0) file << "+";
		file << e.offset << "},";
		if (e.submissions > 0)
			file << "  // " << e.submissions << " submissions";
		file << "\n";
	}

	file << "    {nullptr, nullptr, 0}\n";
	file << "};\n";

	return file.good();
}
