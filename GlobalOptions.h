#pragma once

#include <windows.h>
#include <atomic>

#pragma comment(lib, "advapi32.lib")

namespace GlobalOptions {
inline constexpr bool kDefaultDisableIsrcScanning = true;

inline bool LoadDisableIsrcScanning()
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\OptiScan",
        L"DisableIsrcScanning", RRF_RT_REG_DWORD, nullptr, &value, &size)
        != ERROR_SUCCESS || value > 1)
        return kDefaultDisableIsrcScanning;
    return value != 0;
}

inline std::atomic<bool>& DisableIsrcScanningState()
{
    static std::atomic<bool> disabled{LoadDisableIsrcScanning()};
    return disabled;
}

inline bool IsIsrcScanningDisabled()
{
    return DisableIsrcScanningState().load(std::memory_order_relaxed);
}

inline void SetIsrcScanningDisabled(bool disabled)
{
    DisableIsrcScanningState().store(disabled, std::memory_order_relaxed);
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\OptiScan", 0, nullptr,
        0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        const DWORD value = disabled ? 1 : 0;
        RegSetValueExW(key, L"DisableIsrcScanning", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
}
}
