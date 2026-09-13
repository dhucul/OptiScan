#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

// Resolve the single BINARY FILE named by a UTF-8 image CUE. Never select
// independent first-of-extension files from a directory.
inline bool ResolveCueImage(const std::wstring& cue, std::wstring& image, std::string& error) {
    image.clear();
    std::ifstream input(std::filesystem::path(cue), std::ios::binary);
    if (!input) { error = "Cannot open CUE sheet."; return false; }
    std::string line;
    unsigned files = 0;
    while (std::getline(input, line)) {
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        std::istringstream row(line);
        std::string command, name, kind;
        row >> command;
        if (command != "FILE") continue;
        if (++files != 1 || !(row >> std::quoted(name, '"', '\0') >> kind) || kind != "BINARY" || name.empty()) {
            error = "Select a single-image BINARY CUE; use Write from CUE for audio-file layouts.";
            return false;
        }
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
            static_cast<int>(name.size()), nullptr, 0);
        if (count <= 0) { error = "Invalid UTF-8 image filename."; return false; }
        std::wstring wide(count, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
            static_cast<int>(name.size()), wide.data(), count);
        image = (std::filesystem::path(cue).parent_path() / wide).lexically_normal().wstring();
    }
    std::error_code ec;
    if (files != 1 || input.bad() || !std::filesystem::is_regular_file(image, ec) || ec) {
        error = "The CUE must name one existing BIN image.";
        return false;
    }
    return true;
}
