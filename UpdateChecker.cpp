#include "UpdateChecker.h"
#include "ConsoleFormat.h"
#include "GuiInput.h"
#include <windows.h>
#include <winhttp.h>
#include <wininet.h>
#include <shellapi.h>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wininet.lib")

// Current application version — update this with each release.
// Used by MainMenu.cpp via CheckForUpdates(APP_VERSION).
const VersionInfo APP_VERSION = { 3, 25, 0 };

static const wchar_t* GITHUB_HOST = L"api.github.com";
static const wchar_t* RELEASE_PATH = L"/repos/dhucul/OptiScan/releases/latest";
static const wchar_t* USER_AGENT = L"OptiScan-UpdateChecker/1.0";

// Connection and receive timeout in milliseconds.
static const DWORD TIMEOUT_MS = 10000;

bool ParseVersionTag(const std::wstring& tag, VersionInfo& out)
{
    // Expected format: "v1.2" or "v1.2.3" (leading 'v' is optional)
    std::wstring s = tag;
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V'))
        s = s.substr(1);

    int parts[3] = { 0, 0, 0 };
    int count = swscanf_s(s.c_str(), L"%d.%d.%d", &parts[0], &parts[1], &parts[2]);
    if (count < 2)
        return false;

    out.major = parts[0];
    out.minor = parts[1];
    out.patch = parts[2];
    return true;
}

// Converts a wide string to a narrow UTF-8 string for std::cout output.
static std::string WideToNarrow(const std::wstring& wide)
{
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string narrow(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), &narrow[0], len, nullptr, nullptr);
    return narrow;
}

// Extracts a JSON string value for a given key, searching from a starting position.
// Handles simple flat JSON; does not support nested objects or escaped quotes.
static std::wstring ExtractJsonString(const std::wstring& json, const std::wstring& key, size_t startPos = 0)
{
    std::wstring search = L"\"" + key + L"\"";
    size_t pos = json.find(search, startPos);
    if (pos == std::wstring::npos)
        return L"";

    pos = json.find(L':', pos + search.size());
    if (pos == std::wstring::npos)
        return L"";

    pos = json.find(L'"', pos + 1);
    if (pos == std::wstring::npos)
        return L"";

    size_t end = json.find(L'"', pos + 1);
    if (end == std::wstring::npos)
        return L"";

    return json.substr(pos + 1, end - pos - 1);
}

// Maps a WinHTTP error code to a user-friendly message.
static const char* GetConnectionErrorMessage(DWORD error)
{
    switch (error) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return "DNS lookup failed - check your internet connection.\n";
    case ERROR_WINHTTP_CANNOT_CONNECT:
        return "Could not connect to GitHub - server may be down or blocked.\n";
    case ERROR_WINHTTP_TIMEOUT:
        return "Connection timed out - check your internet connection.\n";
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return "Connection dropped - network may be unstable.\n";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return "TLS/SSL error - check your system clock and firewall settings.\n";
    default:
        return "Unable to reach GitHub - check your internet connection.\n";
    }
}

// RAII wrapper for HINTERNET handles — prevents resource leaks on early returns.
struct WinHttpHandle
{
    HINTERNET h = nullptr;
    WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
    explicit operator bool() const { return h != nullptr; }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

bool CheckForUpdates(const VersionInfo& currentVersion)
{
    Console::Info("\nChecking for updates...\n");

    // Quick connectivity pre-check — avoids a long WinHTTP timeout
    // when the system has no internet at all.
    // Note: InternetGetConnectedState is deprecated but still functional;
    // used here only as a fast-fail hint, not as an authoritative check.
    DWORD connectFlags = 0;
    if (!InternetGetConnectedState(&connectFlags, 0))
    {
        Console::Warning("No internet connection detected.\n");
        Console::Info("Connect to the internet and try again.\n");
        return false;
    }

    WinHttpHandle hSession(WinHttpOpen(
        USER_AGENT,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));

    if (!hSession)
    {
        Console::Error("Unable to initialize HTTP session.\n");
        return false;
    }

    // Set timeouts so the check doesn't hang on a dead connection.
    WinHttpSetTimeouts(hSession,
        TIMEOUT_MS,   // DNS resolve timeout
        TIMEOUT_MS,   // connect timeout
        TIMEOUT_MS,   // send timeout
        TIMEOUT_MS);  // receive timeout

    WinHttpHandle hConnect(WinHttpConnect(hSession, GITHUB_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!hConnect)
    {
        Console::Error("Unable to create connection handle.\n");
        return false;
    }

    WinHttpHandle hRequest(WinHttpOpenRequest(
        hConnect,
        L"GET",
        RELEASE_PATH,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));

    if (!hRequest)
    {
        Console::Error("Unable to create HTTP request.\n");
        return false;
    }

    BOOL result = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0,
        0, 0);

    if (!result)
    {
        DWORD err = GetLastError();
        Console::Error(GetConnectionErrorMessage(err));
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr))
    {
        DWORD err = GetLastError();
        Console::Error(GetConnectionErrorMessage(err));
        return false;
    }

    // Check HTTP status code
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    if (statusCode == 404)
    {
        Console::Warning("No releases found on GitHub.\n");
        return false;
    }

    if (statusCode == 403)
    {
        Console::Warning("GitHub API rate limit exceeded. Try again later.\n");
        return false;
    }

    if (statusCode != 200)
    {
        std::string msg = "GitHub returned HTTP " + std::to_string(statusCode) + ".\n";
        Console::Error(msg.c_str());
        return false;
    }

    // Read response body
    std::vector<char> responseBody;
    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;

    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0)
    {
        std::vector<char> buffer(bytesAvailable);
        if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead))
            responseBody.insert(responseBody.end(), buffer.begin(), buffer.begin() + bytesRead);
    }

    if (responseBody.empty())
    {
        Console::Error("Empty response from GitHub.\n");
        return false;
    }

    // Convert UTF-8 response to wide string
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, responseBody.data(), (int)responseBody.size(), nullptr, 0);
    std::wstring json(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, responseBody.data(), (int)responseBody.size(), &json[0], wideLen);

    // Parse tag_name first, then search for html_url *after* tag_name's position
    // to avoid matching the author object's html_url which appears earlier in the JSON.
    std::wstring tagName = ExtractJsonString(json, L"tag_name");
    size_t tagPos = json.find(L"\"tag_name\"");
    std::wstring htmlUrl = ExtractJsonString(json, L"html_url", tagPos);

    if (tagName.empty())
    {
        Console::Error("Could not parse release information.\n");
        return false;
    }

    VersionInfo latestVersion{};
    if (!ParseVersionTag(tagName, latestVersion))
    {
        Console::Error("Could not parse version tag.\n");
        return false;
    }

    if (latestVersion > currentVersion)
    {
        Console::Success("\nA new version is available!\n");
        std::string cur = "  Current version:  " + currentVersion.ToNarrowString() + "\n";
        std::string lat = "  Latest version:   " + latestVersion.ToNarrowString() + "\n";
        std::cout << cur << lat;
        if (!htmlUrl.empty())
        {
            std::string url = WideToNarrow(htmlUrl);
            Console::Info("\n  Download: ");
            std::cout << url << "\n\n";

            if (GuiInput::PromptYesNo("Update available",
                "Open download page in browser?"))
            {
                INT_PTR shellResult = (INT_PTR)ShellExecuteW(
                    nullptr, L"open", htmlUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (shellResult > 32)
                    Console::Success("Opened in browser.\n");
                else
                    Console::Warning("Could not open browser. Visit the URL above to download.\n");
            }
        }
        std::cout << "\n";
    }
    else
    {
        std::string msg = "You are running the latest version (" + currentVersion.ToNarrowString() + ").\n";
        Console::Success(msg.c_str());
    }

    return true;
}
