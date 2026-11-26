#include "UpdateCheck.h"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace chatda {
namespace {

#ifndef CHATDA_VERSION
#error CHATDA_VERSION must be defined by the project (read from version.txt).
#endif
constexpr wchar_t kCurrentVersion[] = L"" CHATDA_VERSION;
constexpr wchar_t kUpdateTitle[] = L"Update Available";
constexpr wchar_t kUpdateErrorTitle[] = L"Update Failed";
constexpr wchar_t kLatestReleaseUrl[] = L"https://api.github.com/repos/mrkim0118/Chatda/releases/latest";

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == L'\\') {
        return left + right;
    }
    return left + L'\\' + right;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required == 0) {
        required = MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (required == 0) {
            return {};
        }
        std::wstring converted(static_cast<size_t>(required), L'\0');
        MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), converted.data(), required);
        if (!converted.empty() && converted.front() == 0xFEFF) {
            converted.erase(converted.begin());
        }
        return converted;
    }

    std::wstring converted(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), converted.data(), required);
    if (!converted.empty() && converted.front() == 0xFEFF) {
        converted.erase(converted.begin());
    }
    return converted;
}

std::wstring FormatUpdateMessage(const std::wstring& currentVersion, const std::wstring& newestVersion) {
    std::wstring message = L"New version: ";
    message += newestVersion;
    message += L"\nCurrent version: ";
    message += currentVersion;
    message += L"\n\nInstall now?";
    return message;
}

std::wstring FormatDownloadFailedMessage() {
    return L"Could not download the installer.";
}

std::wstring FormatInstallStartFailedMessage() {
    return L"The installer could not be started.";
}

int CompareVersionStrings(const std::wstring& left, const std::wstring& right) {
    auto parse = [](const std::wstring& value) {
        std::vector<int> parts;
        size_t start = 0;
        while (start <= value.size()) {
            const size_t dot = value.find(L'.', start);
            const std::wstring part = value.substr(start, dot == std::wstring::npos ? value.size() - start : dot - start);
            parts.push_back(part.empty() ? 0 : _wtoi(part.c_str()));
            if (dot == std::wstring::npos) {
                break;
            }
            start = dot + 1;
        }
        return parts;
    };

    auto leftParts = parse(left);
    auto rightParts = parse(right);
    const size_t count = std::max(leftParts.size(), rightParts.size());
    leftParts.resize(count, 0);
    rightParts.resize(count, 0);

    for (size_t index = 0; index < count; ++index) {
        if (leftParts[index] < rightParts[index]) {
            return -1;
        }
        if (leftParts[index] > rightParts[index]) {
            return 1;
        }
    }

    return 0;
}

bool DownloadBytes(const std::wstring& url, std::vector<unsigned char>& output, int receiveTimeoutMs) {
    URL_COMPONENTS components = {};
    components.dwStructSize = sizeof(components);

    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
        return false;
    }

    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET session = WinHttpOpen(L"Chatda-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        return false;
    }

    WinHttpSetTimeouts(session, 2000, 2000, 2000, receiveTimeoutMs);

    HINTERNET connection = WinHttpConnect(session, std::wstring(host, components.dwHostNameLength).c_str(), components.nPort, 0);
    if (connection == nullptr) {
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring requestPath(path, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0 && components.lpszExtraInfo != nullptr) {
        requestPath.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (requestPath.empty()) {
        requestPath = L"/";
    }

    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"GET",
        requestPath.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (request == nullptr) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    bool success = false;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX) &&
            statusCode == 200) {
            DWORD available = 0;
            do {
                available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) {
                    break;
                }
                if (available == 0) {
                    success = true;
                    break;
                }
                const size_t start = output.size();
                output.resize(start + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, output.data() + start, available, &read)) {
                    success = false;
                    break;
                }
                output.resize(start + read);
            } while (available > 0);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return success && !output.empty();
}

bool DownloadText(const std::wstring& url, std::wstring& output) {
    std::vector<unsigned char> bytes;
    if (!DownloadBytes(url, bytes, 2500)) {
        return false;
    }

    output = Utf8ToWide(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    return !output.empty();
}

bool DownloadFile(const std::wstring& url, const std::wstring& path) {
    std::vector<unsigned char> bytes;
    if (!DownloadBytes(url, bytes, 30000)) {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path destination(path);
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) {
            return false;
        }
    }

    std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }

    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

// Reads the string value of the first "key": "value" pair at or after `from`; returns the position after it, or npos.
size_t FindJsonString(const std::wstring& json, const std::wstring& key, size_t from, std::wstring& value) {
    const std::wstring quotedKey = L"\"" + key + L"\"";
    size_t pos = json.find(quotedKey, from);
    if (pos == std::wstring::npos || (pos = json.find(L':', pos + quotedKey.size())) == std::wstring::npos ||
        (pos = json.find(L'"', pos)) == std::wstring::npos) {
        return std::wstring::npos;
    }
    const size_t end = json.find(L'"', pos + 1);
    if (end == std::wstring::npos) {
        return std::wstring::npos;
    }
    value = json.substr(pos + 1, end - pos - 1);
    return end + 1;
}

struct LatestRelease {
    std::wstring version;
    std::wstring setupUrl;
};

// shortcut: scans the GitHub JSON for two keys instead of parsing it; enough while each release has one Setup .exe.
bool FetchLatestRelease(LatestRelease& release) {
    std::wstring json;
    std::wstring tag;
    if (!DownloadText(kLatestReleaseUrl, json) || FindJsonString(json, L"tag_name", 0, tag) == std::wstring::npos) {
        return false;
    }
    release.version = !tag.empty() && (tag.front() == L'v' || tag.front() == L'V') ? tag.substr(1) : tag;

    std::wstring url;
    for (size_t pos = 0; (pos = FindJsonString(json, L"browser_download_url", pos, url)) != std::wstring::npos;) {
        std::wstring fileName = url.substr(url.find_last_of(L'/') + 1);
        std::transform(fileName.begin(), fileName.end(), fileName.begin(), towlower);
        if (fileName.size() > 4 && fileName.ends_with(L".exe") && fileName.find(L"setup") != std::wstring::npos) {
            release.setupUrl = url;
            return true;
        }
    }
    return false;
}

std::wstring SanitizeFileNamePart(std::wstring value) {
    for (auto& ch : value) {
        if ((ch >= L'a' && ch <= L'z') ||
            (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9') ||
            ch == L'.' ||
            ch == L'-' ||
            ch == L'_') {
            continue;
        }
        ch = L'_';
    }
    return value;
}

std::wstring BuildTempInstallerPath(const std::wstring& version) {
    wchar_t tempPath[MAX_PATH] = {};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    if (length == 0 || length >= std::size(tempPath)) {
        return {};
    }

    std::wstring fileName = L"Chatda-Setup-";
    fileName += SanitizeFileNamePart(version.empty() ? std::wstring(L"latest") : version);
    fileName += L".exe";
    return JoinPath(tempPath, fileName);
}

bool StartInstaller(HWND owner, const std::wstring& installerPath) {
    const HINSTANCE result = ShellExecuteW(owner, L"open", installerPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

}  // namespace

std::wstring FindNewerVersion() {
    LatestRelease release;
    if (!FetchLatestRelease(release) || CompareVersionStrings(kCurrentVersion, release.version) >= 0) {
        return {};
    }
    return release.version;
}

bool ConfirmUpdate(HWND owner, const std::wstring& version) {
    const std::wstring message = FormatUpdateMessage(kCurrentVersion, version);
    return MessageBoxW(owner, message.c_str(), kUpdateTitle, MB_YESNO | MB_ICONINFORMATION) == IDYES;
}

std::wstring DownloadAndStartInstaller(const std::wstring& version) {
    LatestRelease release;
    const std::wstring installerPath = BuildTempInstallerPath(version);
    if (!FetchLatestRelease(release) || installerPath.empty() || !DownloadFile(release.setupUrl, installerPath)) {
        return FormatDownloadFailedMessage();
    }
    if (!StartInstaller(nullptr, installerPath)) {
        return FormatInstallStartFailedMessage();
    }
    return {};
}

void ShowUpdateError(HWND owner, const std::wstring& error) {
    MessageBoxW(owner, error.c_str(), kUpdateErrorTitle, MB_OK | MB_ICONERROR);
}

}  // namespace chatda
