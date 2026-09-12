#pragma once
#include <cwctype>
#include <string>

inline bool ParseBoundedInteger(const std::wstring& text, int minimum, int maximum, int& value) {
    try {
        size_t end = 0;
        const int parsed = std::stoi(text, &end);
        while (end < text.size() && std::iswspace(text[end])) ++end;
        if (end != text.size() || parsed < minimum || parsed > maximum) return false;
        value = parsed;
        return true;
    } catch (...) { return false; }
}
