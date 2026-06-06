#pragma once

#include <string>

struct VersionInfo
{
    int major = 0;
    int minor = 0;
    int patch = 0;

    bool operator>(const VersionInfo& other) const
    {
        if (major != other.major) return major > other.major;
        if (minor != other.minor) return minor > other.minor;
        return patch > other.patch;
    }

    std::string ToNarrowString() const
    {
        std::string s = std::to_string(major) + "." + std::to_string(minor);
        if (patch > 0) s += "." + std::to_string(patch);
        return s;
    }
};

// Application version — defined in UpdateChecker.cpp.
extern const VersionInfo APP_VERSION;

// Checks the GitHub Releases API for a newer version.
// Returns true if the check succeeded (regardless of whether an update exists).
bool CheckForUpdates(const VersionInfo& currentVersion);

// Parses a version tag like "v7.31" or "v7.31.1" into a VersionInfo struct.
bool ParseVersionTag(const std::wstring& tag, VersionInfo& out);