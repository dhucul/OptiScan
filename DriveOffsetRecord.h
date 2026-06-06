// ============================================================================
// DriveOffsetDatabase.h - Full AccurateRip drive offset database loader
// Downloads & caches the complete database from accuraterip.com/driveoffsets.htm
// ============================================================================
#pragma once

#include "DriveOffsets.h"
#include <string>
#include <vector>
#include <mutex>

struct DriveOffsetRecord {
	std::string vendor;
	std::string model;
	int offset;
	int submissions;
};

class DriveOffsetDatabase {
public:
	// Get the singleton instance
	static DriveOffsetDatabase& Instance();

	// Load the full database (downloads if needed, otherwise uses cache)
	bool Load();

	// Lookup a drive by vendor and model (case-insensitive partial match)
	bool Lookup(const std::string& vendor, const std::string& model, DriveOffsetInfo& info) const;

	// Force re-download from AccurateRip
	bool Refresh();

	// Number of entries loaded
	size_t Count() const { return m_entries.size(); }
	bool IsLoaded() const { return m_loaded; }

private:
	DriveOffsetDatabase() = default;
	DriveOffsetDatabase(const DriveOffsetDatabase&) = delete;
	DriveOffsetDatabase& operator=(const DriveOffsetDatabase&) = delete;

	bool DownloadDatabase(std::string& html);
	bool ParseHTML(const std::string& html);
	bool SaveCache(const std::string& path) const;
	bool LoadCache(const std::string& path);
	std::string GetCachePath() const;

	static std::string ToUpper(const std::string& s);

	std::vector<DriveOffsetRecord> m_entries;
	bool m_loaded = false;
	mutable std::mutex m_mutex;
};