#include "SearchIndex.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace ufe {
namespace {

constexpr size_t kMaxResults = 2000;
constexpr uint32_t kCacheVersion = 1;
constexpr wchar_t kCacheFolderName[] = L"UltraFastFileExplorer";
constexpr wchar_t kCacheFileName[] = L"index-cache-v1.bin";
constexpr char kCacheMagic[] = "UFECIDX1";

template <typename T>
bool WriteValue(std::ofstream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return static_cast<bool>(stream);
}

template <typename T>
bool ReadValue(std::ifstream& stream, T& value) {
    stream.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(stream);
}

bool WriteString(std::ofstream& stream, const std::wstring& value) {
    const uint32_t length = static_cast<uint32_t>(value.size());
    if (!WriteValue(stream, length)) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    stream.write(reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(length * sizeof(wchar_t)));
    return static_cast<bool>(stream);
}

bool ReadString(std::ifstream& stream, std::wstring& value) {
    uint32_t length = 0;
    if (!ReadValue(stream, length)) {
        return false;
    }

    value.assign(length, L'\0');
    if (length == 0) {
        return true;
    }

    stream.read(reinterpret_cast<char*>(value.data()), static_cast<std::streamsize>(length * sizeof(wchar_t)));
    return static_cast<bool>(stream);
}

std::wstring Trim(std::wstring value) {
    auto is_space = [](wchar_t ch) { return iswspace(ch) != 0; };
    const auto start = std::find_if_not(value.begin(), value.end(), is_space);
    const auto end = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    if (start >= end) {
        return {};
    }
    return std::wstring(start, end);
}

std::wstring ToLower(std::wstring value) {
    if (!value.empty()) {
        CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    }
    return value;
}

std::vector<std::wstring> TokenizeQuery(const std::wstring& loweredQuery) {
    std::vector<std::wstring> tokens;
    size_t start = 0;
    while (start < loweredQuery.size()) {
        while (start < loweredQuery.size() && iswspace(loweredQuery[start])) {
            ++start;
        }

        size_t end = start;
        while (end < loweredQuery.size() && !iswspace(loweredQuery[end])) {
            ++end;
        }

        if (end > start) {
            tokens.emplace_back(loweredQuery.substr(start, end - start));
        }
        start = end;
    }
    return tokens;
}

bool ContainsAllTokens(const std::wstring& haystack, const std::vector<std::wstring>& tokens) {
    for (const auto& token : tokens) {
        if (haystack.find(token) == std::wstring::npos) {
            return false;
        }
    }
    return true;
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == L'\\') {
        return left + right;
    }
    return left + L'\\' + right;
}

std::wstring ToExtendedPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        return path;
    }
    if (path.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

}  // namespace

bool SearchIndex::Build() {
    std::vector<std::wstring> roots;
    const DWORD driveMask = GetLogicalDrives();
    for (wchar_t drive = L'A'; drive <= L'Z'; ++drive) {
        const DWORD bit = 1u << (drive - L'A');
        if ((driveMask & bit) == 0) {
            continue;
        }

        std::wstring root;
        root.push_back(drive);
        root.append(L":\\");

        const UINT driveType = GetDriveTypeW(root.c_str());
        if (driveType == DRIVE_FIXED || driveType == DRIVE_REMOVABLE || driveType == DRIVE_RAMDISK) {
            roots.push_back(root);
        }
    }

    if (roots.empty()) {
        return false;
    }

    std::vector<VolumeBuildResult> volumeResults(roots.size());
    std::vector<std::thread> workers;
    workers.reserve(roots.size());

    for (size_t index = 0; index < roots.size(); ++index) {
        workers.emplace_back([&, index]() {
            volumeResults[index] = BuildSingleVolume(roots[index]);
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    volumes_.clear();
    entries_.clear();

    for (auto& result : volumeResults) {
        if (result.root.empty()) {
            continue;
        }

        const uint16_t volumeIndex = static_cast<uint16_t>(volumes_.size());
        const uint32_t offset = static_cast<uint32_t>(entries_.size());
        volumes_.push_back(result.root);

        entries_.reserve(entries_.size() + result.entries.size());
        for (auto& localEntry : result.entries) {
            IndexEntry entry;
            entry.name = std::move(localEntry.name);
            entry.lowerName = std::move(localEntry.lowerName);
            entry.parentIndex = localEntry.parentIndex == kInvalidIndex ? kInvalidIndex : localEntry.parentIndex + offset;
            entry.isDirectory = localEntry.isDirectory;
            entry.volumeIndex = volumeIndex;
            entries_.push_back(std::move(entry));
        }
    }

    return !entries_.empty();
}

std::wstring SearchIndex::CachePath() {
    wchar_t localAppData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (length == 0 || length >= std::size(localAppData)) {
        return {};
    }

    std::filesystem::path cacheDirectory(localAppData);
    cacheDirectory /= kCacheFolderName;
    cacheDirectory /= kCacheFileName;
    return cacheDirectory.wstring();
}

bool SearchIndex::LoadCache() {
    const std::wstring path = CachePath();
    if (path.empty()) {
        return false;
    }

    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) {
        return false;
    }

    char magic[sizeof(kCacheMagic)] = {};
    stream.read(magic, sizeof(magic));
    if (!stream || std::memcmp(magic, kCacheMagic, sizeof(kCacheMagic)) != 0) {
        return false;
    }

    uint32_t version = 0;
    uint32_t volumeCount = 0;
    uint64_t entryCount = 0;
    if (!ReadValue(stream, version) || version != kCacheVersion || !ReadValue(stream, volumeCount) || !ReadValue(stream, entryCount)) {
        return false;
    }

    if (volumeCount == 0 || volumeCount > 64 || entryCount == 0 || entryCount > 50000000ULL) {
        return false;
    }

    std::vector<std::wstring> loadedVolumes;
    std::vector<IndexEntry> loadedEntries;
    loadedVolumes.reserve(volumeCount);
    loadedEntries.reserve(static_cast<size_t>(entryCount));

    for (uint32_t index = 0; index < volumeCount; ++index) {
        std::wstring root;
        if (!ReadString(stream, root) || root.empty()) {
            return false;
        }
        loadedVolumes.push_back(std::move(root));
    }

    for (uint64_t index = 0; index < entryCount; ++index) {
        IndexEntry entry;
        uint8_t flags = 0;
        if (!ReadString(stream, entry.name) ||
            !ReadValue(stream, entry.parentIndex) ||
            !ReadValue(stream, entry.volumeIndex) ||
            !ReadValue(stream, flags)) {
            return false;
        }

        if (entry.name.empty() || entry.volumeIndex >= loadedVolumes.size()) {
            return false;
        }

        entry.lowerName = ToLower(entry.name);
        entry.isDirectory = (flags & 1u) != 0;
        loadedEntries.push_back(std::move(entry));
    }

    volumes_ = std::move(loadedVolumes);
    entries_ = std::move(loadedEntries);
    return !entries_.empty();
}

bool SearchIndex::SaveCache() const {
    if (volumes_.empty() || entries_.empty()) {
        return false;
    }

    const std::wstring path = CachePath();
    if (path.empty()) {
        return false;
    }

    std::filesystem::path cachePath(path);
    std::filesystem::path tempPath = cachePath;
    tempPath += L".tmp";

    std::error_code error;
    std::filesystem::create_directories(cachePath.parent_path(), error);
    if (error) {
        return false;
    }

    {
        std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return false;
        }

        stream.write(kCacheMagic, sizeof(kCacheMagic));
        const uint32_t volumeCount = static_cast<uint32_t>(volumes_.size());
        const uint64_t entryCount = static_cast<uint64_t>(entries_.size());

        if (!WriteValue(stream, kCacheVersion) || !WriteValue(stream, volumeCount) || !WriteValue(stream, entryCount)) {
            return false;
        }

        for (const auto& root : volumes_) {
            if (!WriteString(stream, root)) {
                return false;
            }
        }

        for (const auto& entry : entries_) {
            const uint8_t flags = entry.isDirectory ? 1u : 0u;
            if (!WriteString(stream, entry.name) ||
                !WriteValue(stream, entry.parentIndex) ||
                !WriteValue(stream, entry.volumeIndex) ||
                !WriteValue(stream, flags)) {
                return false;
            }
        }
    }

    if (!MoveFileExW(tempPath.c_str(), cachePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }

    return true;
}

size_t SearchIndex::Size() const {
    return entries_.size();
}

std::vector<SearchResultRow> SearchIndex::Search(const std::wstring& query, uint64_t expectedRequestId, const std::atomic_uint64_t& latestRequestId) const {
    const std::wstring lowered = ToLower(Trim(query));
    const auto tokens = TokenizeQuery(lowered);
    if (tokens.empty()) {
        return {};
    }

    std::vector<uint32_t> matches;
    matches.reserve(std::min(entries_.size(), kMaxResults));

    for (uint32_t index = 0; index < entries_.size(); ++index) {
        if ((index & 2047u) == 0u && latestRequestId.load(std::memory_order_relaxed) != expectedRequestId) {
            return {};
        }

        if (ContainsAllTokens(entries_[index].lowerName, tokens)) {
            matches.push_back(index);
            if (matches.size() >= kMaxResults) {
                break;
            }
        }
    }

    std::sort(matches.begin(), matches.end(), [&](uint32_t left, uint32_t right) {
        const auto& a = entries_[left];
        const auto& b = entries_[right];
        if (a.isDirectory != b.isDirectory) {
            return a.isDirectory && !b.isDirectory;
        }
        const int byName = _wcsicmp(a.name.c_str(), b.name.c_str());
        if (byName != 0) {
            return byName < 0;
        }
        return BuildPath(left) < BuildPath(right);
    });

    std::vector<SearchResultRow> rows;
    rows.reserve(matches.size());
    for (const uint32_t matchIndex : matches) {
        if (latestRequestId.load(std::memory_order_relaxed) != expectedRequestId) {
            return {};
        }

        SearchResultRow row;
        row.name = entries_[matchIndex].name;
        row.path = BuildPath(matchIndex);
        row.type = entries_[matchIndex].isDirectory ? L"Folder" : L"File";
        row.isDirectory = entries_[matchIndex].isDirectory;
        rows.push_back(std::move(row));
    }

    return rows;
}

SearchIndex::VolumeBuildResult SearchIndex::BuildSingleVolume(const std::wstring& root) {
    VolumeBuildResult result;
    result.root = root;
    if (IsNtfs(root) && TryBuildFromUsn(root, result)) {
        return result;
    }

    BuildFromRecursiveScan(root, result);
    return result;
}

bool SearchIndex::TryBuildFromUsn(const std::wstring& root, VolumeBuildResult& result) {
    struct RawNtfsEntry {
        uint64_t fileId = 0;
        uint64_t parentFileId = 0;
        std::wstring name;
        bool isDirectory = false;
    };

    const std::wstring devicePath = DevicePathForRoot(root);
    HANDLE volume = CreateFileW(
        devicePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (volume == INVALID_HANDLE_VALUE) {
        return false;
    }

    USN_JOURNAL_DATA_V0 journalData = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(volume, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journalData, sizeof(journalData), &bytesReturned, nullptr)) {
        CloseHandle(volume);
        return false;
    }

    MFT_ENUM_DATA_V0 enumData = {};
    enumData.StartFileReferenceNumber = 0;
    enumData.LowUsn = 0;
    enumData.HighUsn = journalData.NextUsn;

    std::vector<BYTE> buffer(1 << 20);
    std::vector<RawNtfsEntry> rawEntries;
    rawEntries.reserve(512 * 1024);

    while (DeviceIoControl(
        volume,
        FSCTL_ENUM_USN_DATA,
        &enumData,
        sizeof(enumData),
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &bytesReturned,
        nullptr)) {
        if (bytesReturned <= sizeof(USN)) {
            break;
        }

        DWORD offset = sizeof(USN);
        while (offset + sizeof(USN_RECORD_V2) <= bytesReturned) {
            auto* record = reinterpret_cast<const USN_RECORD_V2*>(buffer.data() + offset);
            if (record->RecordLength == 0) {
                break;
            }
            if (record->MajorVersion == 2 && record->FileNameLength > 0) {
                RawNtfsEntry entry;
                entry.fileId = record->FileReferenceNumber;
                entry.parentFileId = record->ParentFileReferenceNumber;
                entry.isDirectory = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                entry.name.assign(record->FileName, record->FileNameLength / sizeof(WCHAR));
                rawEntries.push_back(std::move(entry));
            }
            offset += record->RecordLength;
        }

        enumData.StartFileReferenceNumber = *reinterpret_cast<USN*>(buffer.data());
    }

    CloseHandle(volume);

    if (rawEntries.empty()) {
        return false;
    }

    result.entries.clear();
    result.entries.reserve(rawEntries.size());
    std::unordered_map<uint64_t, uint32_t> fileIdToIndex;
    fileIdToIndex.reserve(rawEntries.size());

    for (uint32_t index = 0; index < rawEntries.size(); ++index) {
        LocalIndexEntry entry;
        entry.name = std::move(rawEntries[index].name);
        entry.lowerName = ToLower(entry.name);
        entry.isDirectory = rawEntries[index].isDirectory;
        result.entries.push_back(std::move(entry));
        fileIdToIndex.emplace(rawEntries[index].fileId, index);
    }

    for (uint32_t index = 0; index < rawEntries.size(); ++index) {
        const auto it = fileIdToIndex.find(rawEntries[index].parentFileId);
        if (it != fileIdToIndex.end() && it->second != index) {
            result.entries[index].parentIndex = it->second;
        }
    }

    return !result.entries.empty();
}

void SearchIndex::BuildFromRecursiveScan(const std::wstring& root, VolumeBuildResult& result) {
    struct DirectoryWorkItem {
        std::wstring scanPath;
        uint32_t parentIndex = kInvalidIndex;
    };

    std::deque<DirectoryWorkItem> queue;
    queue.push_back({ToExtendedPath(root), kInvalidIndex});

    while (!queue.empty()) {
        DirectoryWorkItem work = std::move(queue.front());
        queue.pop_front();

        WIN32_FIND_DATAW findData = {};
        const std::wstring searchPattern = JoinPath(work.scanPath, L"*");
        HANDLE findHandle = FindFirstFileExW(
            searchPattern.c_str(),
            FindExInfoBasic,
            &findData,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH);

        if (findHandle == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            const std::wstring_view name(findData.cFileName);
            if (name == L"." || name == L"..") {
                continue;
            }

            LocalIndexEntry entry;
            entry.name.assign(name.begin(), name.end());
            entry.lowerName = ToLower(entry.name);
            entry.parentIndex = work.parentIndex;
            entry.isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            const uint32_t currentIndex = static_cast<uint32_t>(result.entries.size());
            result.entries.push_back(std::move(entry));

            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                queue.push_back({JoinPath(work.scanPath, std::wstring(name)), currentIndex});
            }
        } while (FindNextFileW(findHandle, &findData));

        FindClose(findHandle);
    }
}

bool SearchIndex::IsNtfs(const std::wstring& root) {
    wchar_t fileSystemName[MAX_PATH] = {};
    return GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fileSystemName, static_cast<DWORD>(std::size(fileSystemName))) != FALSE &&
           _wcsicmp(fileSystemName, L"NTFS") == 0;
}

std::wstring SearchIndex::DevicePathForRoot(const std::wstring& root) {
    return L"\\\\.\\" + root.substr(0, 2);
}

std::wstring SearchIndex::BuildPath(uint32_t index) const {
    if (index >= entries_.size()) {
        return {};
    }

    const IndexEntry* entry = &entries_[index];
    std::vector<std::wstring_view> segments;
    segments.reserve(16);
    while (entry != nullptr) {
        segments.push_back(entry->name);
        if (entry->parentIndex == kInvalidIndex || entry->parentIndex >= entries_.size()) {
            break;
        }
        entry = &entries_[entry->parentIndex];
    }

    std::wstring path = volumes_[entries_[index].volumeIndex];
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        if (!path.empty() && path.back() != L'\\') {
            path.push_back(L'\\');
        }
        path.append(it->data(), it->size());
    }
    return path;
}

}  // namespace ufe
