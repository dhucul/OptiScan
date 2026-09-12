#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <vector>

// Stage a complete artifact set beside its destination. Existing outputs are
// moved to private backups only when all staged streams have closed successfully.
class ArtifactTransaction {
    struct Entry {
        std::filesystem::path target, staged, backup;
        bool backedUp = false, installed = false;
    };
    std::filesystem::path directory;
    std::vector<Entry> entries;
    bool retainRecovery = false;
public:
    explicit ArtifactTransaction(const std::filesystem::path& base) {
        static std::atomic<unsigned long> sequence{0};
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = base.parent_path() / (L".optiscan-stage-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" +
                std::to_wstring(sequence.fetch_add(1)));
            std::error_code ec;
            if (std::filesystem::create_directory(candidate, ec)) {
                directory = candidate;
                break;
            }
            if (ec && ec != std::errc::file_exists) break;
        }
    }
    ArtifactTransaction(const ArtifactTransaction&) = delete;
    ArtifactTransaction& operator=(const ArtifactTransaction&) = delete;
    ~ArtifactTransaction() {
        if (!directory.empty() && !retainRecovery) {
            std::error_code ec;
            std::filesystem::remove_all(directory, ec);
        }
    }
    std::filesystem::path Stage(const std::filesystem::path& target) {
        if (directory.empty()) return {};
        const auto index = std::to_wstring(entries.size());
        Entry entry{target, directory / (L"new-" + index), directory / (L"old-" + index)};
        entries.push_back(entry);
        return entry.staged;
    }
    std::filesystem::path ScratchFile(const std::wstring& name) const {
        if (directory.empty() || name.empty() || name == L"." || name == L".." ||
            std::filesystem::path(name).filename() != name) return {};
        return directory / (L"scratch-" + name);
    }
    bool Commit() {
        if (directory.empty() || entries.empty()) return false;
        for (const auto& entry : entries) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(entry.staged, ec) || ec) return false;
            const bool exists = std::filesystem::exists(entry.target, ec);
            if (ec || (exists && !std::filesystem::is_regular_file(entry.target, ec)) || ec)
                return false;
        }
        for (auto& entry : entries) {
            const DWORD attributes = GetFileAttributesW(entry.target.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) {
                if (!MoveFileExW(entry.target.c_str(), entry.backup.c_str(), MOVEFILE_WRITE_THROUGH))
                    return Rollback();
                entry.backedUp = true;
            }
            if (!MoveFileExW(entry.staged.c_str(), entry.target.c_str(), MOVEFILE_WRITE_THROUGH))
                return Rollback();
            entry.installed = true;
        }
        return true;
    }
private:
    bool Rollback() {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->installed && !DeleteFileW(it->target.c_str())) retainRecovery = true;
            if (it->backedUp && !MoveFileExW(it->backup.c_str(), it->target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) retainRecovery = true;
        }
        if (retainRecovery)
            std::wcerr << L"Output rollback was incomplete. Previous files retained in: "
                       << directory.wstring() << L"\n";
        return false;
    }
};
