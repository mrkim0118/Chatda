#include "SearchIndex.h"

#include <winioctl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <thread>
#include <utility>

#include "StringUtil.h"

namespace chatda {
namespace {

constexpr size_t kMaxResults = 2000;
constexpr uint32_t kCacheVersion = 1;
constexpr wchar_t kDataFolderName[] = L"Chatda";
constexpr wchar_t kCacheFileName[] = L"index-cache-v1.bin";
constexpr wchar_t kExcludeFileName[] = L"exclude.txt";
constexpr char kCacheMagic[] = "CHATIDX1";

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

bool WriteString(std::ofstream& stream, std::wstring_view value) {
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
    if (!ReadValue(stream, length) || length > 0xFFFF) {
        return false;
    }

    value.assign(length, L'\0');
    if (length == 0) {
        return true;
    }

    stream.read(reinterpret_cast<char*>(value.data()), static_cast<std::streamsize>(length * sizeof(wchar_t)));
    return static_cast<bool>(stream);
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

// Initial consonants of Hangul syllables, as compatibility jamo (ㄱ ㄲ ㄴ ...).
constexpr wchar_t kChosung[] = {
    0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, 0x3145,
    0x3146, 0x3147, 0x3148, 0x3149, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E,
};

bool IsChosungOnly(std::wstring_view token) {
    return !token.empty() && std::all_of(token.begin(), token.end(), [](wchar_t ch) { return ch >= 0x3131 && ch <= 0x314E; });
}

// Replaces every Hangul syllable with its initial consonant: "보고서.hwp" -> "ㅂㄱㅅ.hwp".
void ToChosung(std::wstring_view text, std::wstring& output) {
    output.clear();
    for (const wchar_t ch : text) {
        output.push_back(ch >= 0xAC00 && ch <= 0xD7A3 ? kChosung[(ch - 0xAC00) / 588] : ch);
    }
}

// `*` matches any run of characters, `?` one character; the whole text must match.
bool WildcardMatch(std::wstring_view text, std::wstring_view pattern) {
    size_t t = 0;
    size_t p = 0;
    size_t star = std::wstring_view::npos;
    size_t mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == L'?' || pattern[p] == text[t])) {
            ++t;
            ++p;
        } else if (p < pattern.size() && pattern[p] == L'*') {
            star = p++;
            mark = t;
        } else if (star != std::wstring_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*') {
        ++p;
    }
    return p == pattern.size();
}

struct ParsedQuery {
    enum Type { kAny, kFilesOnly, kFoldersOnly };

    std::vector<std::wstring> words;       // must appear in the name
    std::vector<std::wstring> paths;       // contain '\', must appear in the full path
    std::vector<std::wstring> wildcards;   // *.pdf, report?.txt: must match the whole name
    std::vector<std::wstring> chosungs;    // ㅂㄱㅅ: matched against the initial consonants
    std::vector<std::wstring> extensions;  // ext:pdf,docx -> ".pdf", ".docx"
    Type type = kAny;
    std::wstring exact;  // the whole query, when a plain name equal to it should rank first

    bool Empty() const {
        return words.empty() && paths.empty() && wildcards.empty() && chosungs.empty() && extensions.empty() && type == kAny;
    }
};

ParsedQuery ParseQuery(const std::wstring& lowered) {
    ParsedQuery parsed;
    for (auto& token : TokenizeQuery(lowered)) {
        if (token == L"file:" || token == L"파일:") {
            parsed.type = ParsedQuery::kFilesOnly;
        } else if (token == L"folder:" || token == L"폴더:") {
            parsed.type = ParsedQuery::kFoldersOnly;
        } else if (token.starts_with(L"ext:")) {
            size_t start = 4;
            while (start <= token.size()) {
                const size_t end = std::min(token.find_first_of(L",;", start), token.size());
                std::wstring extension = token.substr(start, end - start);
                if (!extension.empty()) {
                    parsed.extensions.push_back(extension.front() == L'.' ? extension : L"." + extension);
                }
                start = end + 1;
            }
        } else if (token.find_first_of(L"\\/") != std::wstring::npos) {
            std::replace(token.begin(), token.end(), L'/', L'\\');
            parsed.paths.push_back(std::move(token));
        } else if (token.find_first_of(L"*?") != std::wstring::npos) {
            parsed.wildcards.push_back(std::move(token));
        } else if (IsChosungOnly(token)) {
            parsed.chosungs.push_back(std::move(token));
        } else {
            parsed.words.push_back(std::move(token));
        }
    }

    if (!parsed.words.empty() && parsed.paths.empty() && parsed.wildcards.empty() && parsed.chosungs.empty() &&
        parsed.extensions.empty() && parsed.type == ParsedQuery::kAny) {
        parsed.exact = lowered;
    }
    return parsed;
}

// Checks everything that only needs the lowercase name. `scratch` is reused between calls.
bool MatchesName(std::wstring_view lower, const ParsedQuery& query, std::wstring& scratch) {
    if (!query.extensions.empty() &&
        std::none_of(query.extensions.begin(), query.extensions.end(), [&](const std::wstring& extension) { return lower.ends_with(extension); })) {
        return false;
    }
    for (const auto& word : query.words) {
        if (lower.find(word) == std::wstring_view::npos) {
            return false;
        }
    }
    for (const auto& pattern : query.wildcards) {
        if (!WildcardMatch(lower, pattern)) {
            return false;
        }
    }
    if (!query.chosungs.empty()) {
        ToChosung(lower, scratch);
        for (const auto& chosung : query.chosungs) {
            if (scratch.find(chosung) == std::wstring::npos) {
                return false;
            }
        }
    }
    return true;
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

uint64_t RootFileId(const std::wstring& root) {
    UniqueHandle directory(CreateFileW(
        root.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    BY_HANDLE_FILE_INFORMATION info = {};
    if (directory.Get() == nullptr || !GetFileInformationByHandle(directory.Get(), &info)) {
        return 0;
    }
    return (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}

std::wstring Utf8OrUtf16ToWide(const std::string& bytes) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / sizeof(wchar_t));
    }

    size_t start = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        start = 3;
    }
    const int length = static_cast<int>(bytes.size() - start);
    if (length <= 0) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + start, length, nullptr, 0);
    std::wstring text(static_cast<size_t>(std::max(required, 0)), L'\0');
    if (required > 0) {
        MultiByteToWideChar(CP_UTF8, 0, bytes.data() + start, length, text.data(), required);
    }
    return text;
}

}  // namespace

ExcludeRules ParseExcludeRules(const std::wstring& text) {
    static std::atomic_uint64_t nextVersion = 0;

    ExcludeRules rules;
    rules.version = ++nextVersion;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find_first_of(L"\r\n", start);
        if (end == std::wstring::npos) {
            end = text.size();
        }
        std::wstring line = ToLower(Trim(text.substr(start, end - start)));
        start = end + 1;
        if (line.empty() || line.front() == L'#') {
            continue;
        }

        std::replace(line.begin(), line.end(), L'/', L'\\');
        if (line.find(L'\\') == std::wstring::npos && !(line.size() == 2 && line[1] == L':')) {
            rules.names.push_back(std::move(line));
            continue;
        }
        if (line.size() == 2) {
            line.push_back(L'\\');  // "c:" -> "c:\"
        }
        while (line.size() > 3 && line.back() == L'\\') {
            line.pop_back();
        }
        rules.paths.push_back(std::move(line));
    }
    return rules;
}

std::wstring DataDirectory() {
    wchar_t localAppData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (length == 0 || length >= std::size(localAppData)) {
        return {};
    }
    return JoinPath(localAppData, kDataFolderName);
}

std::wstring ExcludeFilePath() {
    const std::wstring directory = DataDirectory();
    return directory.empty() ? std::wstring{} : JoinPath(directory, kExcludeFileName);
}

void EnsureExcludeFile(const std::wstring& defaultText) {
    const std::wstring path = ExcludeFilePath();
    if (path.empty() || GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(DataDirectory(), error);

    const int required = WideCharToMultiByte(CP_UTF8, 0, defaultText.data(), static_cast<int>(defaultText.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes = "\xEF\xBB\xBF";  // BOM, so Notepad keeps the file in UTF-8
    const size_t bomSize = bytes.size();
    bytes.resize(bomSize + static_cast<size_t>(std::max(required, 0)));
    if (required > 0) {
        WideCharToMultiByte(CP_UTF8, 0, defaultText.data(), static_cast<int>(defaultText.size()), bytes.data() + bomSize, required, nullptr, nullptr);
    }

    std::ofstream stream(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

uint64_t ExcludeFileStamp() {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    const std::wstring path = ExcludeFilePath();
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

ExcludeRules LoadExcludeRules() {
    const std::wstring path = ExcludeFilePath();
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (path.empty() || !stream) {
        return ParseExcludeRules({});
    }
    const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return ParseExcludeRules(Utf8OrUtf16ToWide(bytes));
}

uint32_t SearchIndex::IdLookup::Find(uint64_t id) const {
    const auto it = std::lower_bound(ids.begin(), ids.end(), id);
    if (it != ids.end() && *it == id) {
        return indices[static_cast<size_t>(it - ids.begin())];
    }
    const auto found = added.find(id);
    return found == added.end() ? kInvalidIndex : found->second;
}

void SearchIndex::AppendName(std::wstring& pool, std::wstring_view name, IndexEntry& entry) {
    thread_local std::wstring lower;
    name = name.substr(0, std::min<size_t>(name.size(), 0xFFFF));
    entry.nameOffset = static_cast<uint32_t>(pool.size());
    entry.nameLength = static_cast<uint16_t>(name.size());
    pool.append(name);

    lower.assign(name);
    if (!lower.empty()) {
        CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    }
    if (lower == name) {
        entry.lowerOffset = entry.nameOffset;
    } else {
        entry.lowerOffset = static_cast<uint32_t>(pool.size());
        pool.append(lower);
    }
}

uint32_t SearchIndex::EligibleDriveMask() {
    uint32_t mask = 0;
    const DWORD drives = GetLogicalDrives();
    for (int drive = 0; drive < 26; ++drive) {
        const uint32_t bit = 1u << drive;
        if ((drives & bit) == 0) {
            continue;
        }
        const wchar_t root[] = {static_cast<wchar_t>(L'A' + drive), L':', L'\\', L'\0'};
        const UINT type = GetDriveTypeW(root);
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK) {
            mask |= bit;
        }
    }
    return mask;
}

bool SearchIndex::Build(const std::atomic_bool& cancel) {
    const uint32_t mask = EligibleDriveMask();
    std::vector<std::wstring> roots;
    for (int drive = 0; drive < 26; ++drive) {
        if ((mask & (1u << drive)) != 0) {
            roots.push_back(std::wstring{static_cast<wchar_t>(L'A' + drive), L':', L'\\'});
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
            volumeResults[index] = BuildSingleVolume(roots[index], cancel);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    if (cancel.load(std::memory_order_relaxed)) {
        return false;
    }

    driveMask_ = mask;
    volumes_.clear();
    entries_.clear();
    pool_.clear();

    for (auto& result : volumeResults) {
        if (result.entries.empty()) {
            continue;
        }

        const uint16_t volumeIndex = static_cast<uint16_t>(volumes_.size());
        const uint32_t entryBase = static_cast<uint32_t>(entries_.size());
        const uint32_t poolBase = static_cast<uint32_t>(pool_.size());
        pool_.append(result.pool);
        result.pool = {};

        entries_.reserve(entries_.size() + result.entries.size());
        for (auto entry : result.entries) {
            if (entry.parentIndex != kInvalidIndex) {
                entry.parentIndex += entryBase;
            }
            entry.nameOffset += poolBase;
            entry.lowerOffset += poolBase;
            entry.volumeIndex = volumeIndex;
            entries_.push_back(entry);
        }
        result.entries = {};  // free this volume's buffer before merging the next one

        for (auto& index : result.volume.lookup.indices) {
            index += entryBase;
        }
        volumes_.push_back(std::move(result.volume));
    }

    liveCount_.store(entries_.size(), std::memory_order_relaxed);
    return !entries_.empty();
}

std::wstring SearchIndex::CachePath() {
    const std::wstring directory = DataDirectory();
    return directory.empty() ? std::wstring{} : JoinPath(directory, kCacheFileName);
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

    std::vector<Volume> loadedVolumes(volumeCount);
    for (auto& volume : loadedVolumes) {
        if (!ReadString(stream, volume.root) || volume.root.empty()) {
            return false;
        }
    }

    std::vector<IndexEntry> loadedEntries;
    std::wstring loadedPool;
    loadedEntries.reserve(static_cast<size_t>(entryCount));
    std::wstring name;
    for (uint64_t index = 0; index < entryCount; ++index) {
        IndexEntry entry;
        uint8_t flags = 0;
        if (!ReadString(stream, name) || !ReadValue(stream, entry.parentIndex) || !ReadValue(stream, entry.volumeIndex) || !ReadValue(stream, flags)) {
            return false;
        }
        if (name.empty() || entry.volumeIndex >= loadedVolumes.size() ||
            (entry.parentIndex != kInvalidIndex && entry.parentIndex >= entryCount)) {
            return false;
        }
        AppendName(loadedPool, name, entry);
        entry.flags = (flags & 1u) != 0 ? kDirectoryFlag : 0;
        loadedEntries.push_back(entry);
    }

    std::unique_lock lock(mutex_);
    volumes_ = std::move(loadedVolumes);
    entries_ = std::move(loadedEntries);
    pool_ = std::move(loadedPool);
    liveCount_.store(entries_.size(), std::memory_order_relaxed);
    return !entries_.empty();
}

bool SearchIndex::SaveCache() const {
    std::shared_lock lock(mutex_);
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

        for (const auto& volume : volumes_) {
            if (!WriteString(stream, volume.root)) {
                return false;
            }
        }

        for (const auto& entry : entries_) {
            const uint8_t flags = entry.IsDirectory() ? 1u : 0u;
            if (!WriteString(stream, Name(entry)) || !WriteValue(stream, entry.parentIndex) || !WriteValue(stream, entry.volumeIndex) ||
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

SearchOutcome SearchIndex::Search(const std::wstring& query, uint64_t expectedRequestId, const std::atomic_uint64_t& latestRequestId, const ExcludeRules& rules) const {
    const std::wstring lowered = ToLower(Trim(query));
    const ParsedQuery parsed = ParseQuery(lowered);
    if (parsed.Empty()) {
        return {};
    }

    auto superseded = [&]() { return latestRequestId.load(std::memory_order_relaxed) != expectedRequestId; };

    std::shared_lock lock(mutex_);
    RefreshExcludedPaths(rules);
    const bool checkExcludes = !rules.Empty();
    std::unordered_map<uint32_t, bool> excludeMemo;
    std::wstring scratch;

    // Collect every match, then keep the best kMaxResults:
    // exact name first, then names starting with the first word, then the rest.
    struct Match {
        uint32_t index;
        uint8_t rank;
    };
    std::vector<Match> matches;

    for (uint32_t index = 0; index < entries_.size(); ++index) {
        if ((index & 2047u) == 0u && superseded()) {
            return {};
        }

        const IndexEntry& entry = entries_[index];
        if (entry.IsDeleted() || (parsed.type == ParsedQuery::kFilesOnly && entry.IsDirectory()) ||
            (parsed.type == ParsedQuery::kFoldersOnly && !entry.IsDirectory())) {
            continue;
        }

        const std::wstring_view lower = Lower(entry);
        if (!MatchesName(lower, parsed, scratch)) {
            continue;
        }
        if (!parsed.paths.empty()) {
            BuildLowerPath(index, scratch);
            if (std::any_of(parsed.paths.begin(), parsed.paths.end(), [&](const std::wstring& part) { return scratch.find(part) == std::wstring::npos; })) {
                continue;
            }
        }
        if (checkExcludes && IsExcluded(index, rules, excludeMemo)) {
            continue;
        }

        const uint8_t rank = !parsed.exact.empty() && lower == parsed.exact ? 0
                             : !parsed.words.empty() && lower.starts_with(parsed.words.front()) ? 1
                                                                                                : 2;
        matches.push_back({index, rank});
    }

    SearchOutcome outcome;
    outcome.totalMatches = matches.size();

    // A short query can match millions of names; the comparator bails out once a newer search starts.
    struct Superseded {};
    size_t comparisons = 0;
    const size_t keep = std::min(matches.size(), kMaxResults);
    try {
        std::partial_sort(matches.begin(), matches.begin() + static_cast<ptrdiff_t>(keep), matches.end(), [&](const Match& left, const Match& right) {
            if ((++comparisons & 4095u) == 0u && superseded()) {
                throw Superseded{};
            }
            if (left.rank != right.rank) {
                return left.rank < right.rank;
            }
            const IndexEntry& a = entries_[left.index];
            const IndexEntry& b = entries_[right.index];
            if (a.IsDirectory() != b.IsDirectory()) {
                return a.IsDirectory();
            }
            const int byName = Lower(a).compare(Lower(b));
            if (byName != 0) {
                return byName < 0;
            }
            return left.index < right.index;
        });
    } catch (const Superseded&) {
        return {};
    }
    matches.resize(keep);

    outcome.rows.reserve(matches.size());
    for (const auto& match : matches) {
        if (superseded()) {
            return {};
        }
        const IndexEntry& entry = entries_[match.index];
        SearchResultRow row;
        row.name.assign(Name(entry));
        row.path = BuildPath(match.index);
        row.isDirectory = entry.IsDirectory();
        outcome.rows.push_back(std::move(row));
    }
    return outcome;
}

SearchIndex::PollResult SearchIndex::PollJournals() {
    PollResult result = PollResult::None;
    std::vector<BYTE> buffer(64 * 1024);

    // volumes_ is never resized after Build, and live/nextUsn are only touched by this (index) thread.
    for (uint16_t volumeIndex = 0; volumeIndex < volumes_.size(); ++volumeIndex) {
        Volume& volume = volumes_[volumeIndex];
        if (!volume.live) {
            continue;
        }

        std::vector<JournalChange> changes;
        USN next = volume.nextUsn;
        bool failed = false;
        for (int round = 0; round < 256; ++round) {
            READ_USN_JOURNAL_DATA_V0 read = {};
            read.StartUsn = next;
            read.ReasonMask = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE | USN_REASON_RENAME_NEW_NAME;
            read.UsnJournalID = volume.journalId;

            DWORD bytes = 0;
            if (!DeviceIoControl(volume.handle.Get(), FSCTL_READ_USN_JOURNAL, &read, sizeof(read), buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) {
                failed = true;  // journal deleted, recreated or wrapped past `next`
                break;
            }
            if (bytes < sizeof(USN)) {
                break;
            }

            const USN after = *reinterpret_cast<const USN*>(buffer.data());
            DWORD offset = sizeof(USN);
            while (offset + sizeof(USN_RECORD_V2) <= bytes) {
                const auto* record = reinterpret_cast<const USN_RECORD_V2*>(buffer.data() + offset);
                if (record->RecordLength == 0) {
                    break;
                }
                if (record->MajorVersion == 2 && record->FileNameLength > 0) {
                    JournalChange change;
                    change.fileId = record->FileReferenceNumber;
                    change.parentId = record->ParentFileReferenceNumber;
                    change.name.assign(record->FileName, record->FileNameLength / sizeof(WCHAR));
                    change.isDirectory = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    change.deleted = (record->Reason & USN_REASON_FILE_DELETE) != 0;
                    changes.push_back(std::move(change));
                }
                offset += record->RecordLength;
            }

            const bool more = bytes > sizeof(USN) && after != next;
            next = after;
            if (!more) {
                break;
            }
        }

        if (!changes.empty()) {
            ApplyChanges(volumeIndex, changes);
            result = PollResult::Changed;
        }
        if (failed) {
            volume.live = false;
            return PollResult::Rebuild;
        }
        volume.nextUsn = next;
    }
    return result;
}

bool SearchIndex::NeedsPeriodicRebuild() const {
    return std::any_of(volumes_.begin(), volumes_.end(), [](const Volume& volume) { return !volume.live; });
}

bool SearchIndex::DrivesChanged() const {
    return EligibleDriveMask() != driveMask_;
}

void SearchIndex::ApplyChanges(uint16_t volumeIndex, const std::vector<JournalChange>& changes) {
    std::unique_lock lock(mutex_);
    Volume& volume = volumes_[volumeIndex];

    auto remove = [&](uint32_t index) {
        if (index != kInvalidIndex && !entries_[index].IsDeleted()) {
            entries_[index].flags |= kDeletedFlag;
            liveCount_.fetch_sub(1, std::memory_order_relaxed);
        }
    };

    for (const auto& change : changes) {
        uint32_t index = volume.lookup.Find(change.fileId);
        if (change.deleted) {
            remove(index);
            continue;
        }

        uint32_t parent = kInvalidIndex;  // directly under the volume root
        if (change.parentId != volume.rootId) {
            parent = volume.lookup.Find(change.parentId);
            if (parent == kInvalidIndex || parent == index || entries_[parent].IsDeleted()) {
                remove(index);  // created in or moved to a place that is not indexed
                continue;
            }
        }

        const uint8_t typeFlag = change.isDirectory ? kDirectoryFlag : 0;
        if (index == kInvalidIndex) {
            IndexEntry entry;
            entry.parentIndex = parent;
            entry.volumeIndex = volumeIndex;
            entry.flags = typeFlag;
            AppendName(pool_, change.name, entry);
            volume.lookup.added[change.fileId] = static_cast<uint32_t>(entries_.size());
            entries_.push_back(entry);
            liveCount_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        IndexEntry& entry = entries_[index];
        if (entry.IsDeleted()) {
            liveCount_.fetch_add(1, std::memory_order_relaxed);
        }
        if (Name(entry) != change.name) {
            AppendName(pool_, change.name, entry);  // shortcut: the old name stays in pool_ until the next full rebuild
        }
        entry.parentIndex = parent;
        entry.flags = typeFlag;
    }
    ++generation_;
}

SearchIndex::VolumeBuildResult SearchIndex::BuildSingleVolume(const std::wstring& root, const std::atomic_bool& cancel) {
    VolumeBuildResult result;
    result.volume.root = root;
    if (IsNtfs(root) && TryBuildFromUsn(root, result, cancel)) {
        return result;
    }

    result.entries.clear();
    result.pool.clear();
    result.volume.lookup = {};
    BuildFromRecursiveScan(root, result, cancel);
    return result;
}

bool SearchIndex::TryBuildFromUsn(const std::wstring& root, VolumeBuildResult& result, const std::atomic_bool& cancel) {
    UniqueHandle volume(CreateFileW(
        DevicePathForRoot(root).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (volume.Get() == nullptr) {
        return false;
    }

    USN_JOURNAL_DATA_V0 journalData = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(volume.Get(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journalData, sizeof(journalData), &bytesReturned, nullptr)) {
        return false;
    }

    MFT_ENUM_DATA_V0 enumData = {};
    enumData.StartFileReferenceNumber = 0;
    enumData.LowUsn = 0;
    enumData.HighUsn = journalData.NextUsn;

    std::vector<BYTE> buffer(1 << 20);
    std::vector<uint64_t> fileIds;
    std::vector<uint64_t> parentIds;
    fileIds.reserve(512 * 1024);
    parentIds.reserve(512 * 1024);
    result.entries.reserve(512 * 1024);

    while (DeviceIoControl(volume.Get(), FSCTL_ENUM_USN_DATA, &enumData, sizeof(enumData), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesReturned, nullptr)) {
        if (bytesReturned <= sizeof(USN) || cancel.load(std::memory_order_relaxed)) {
            break;
        }

        DWORD offset = sizeof(USN);
        while (offset + sizeof(USN_RECORD_V2) <= bytesReturned) {
            const auto* record = reinterpret_cast<const USN_RECORD_V2*>(buffer.data() + offset);
            if (record->RecordLength == 0) {
                break;
            }
            if (record->MajorVersion == 2 && record->FileNameLength > 0) {
                IndexEntry entry;
                entry.flags = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? kDirectoryFlag : 0;
                AppendName(result.pool, std::wstring_view(record->FileName, record->FileNameLength / sizeof(WCHAR)), entry);
                result.entries.push_back(entry);
                fileIds.push_back(record->FileReferenceNumber);
                parentIds.push_back(record->ParentFileReferenceNumber);
            }
            offset += record->RecordLength;
        }

        enumData.StartFileReferenceNumber = *reinterpret_cast<const USN*>(buffer.data());
    }

    if (cancel.load(std::memory_order_relaxed)) {
        return true;  // nothing more to do; Build() discards the result
    }
    if (result.entries.empty()) {
        return false;
    }

    const uint64_t rootId = RootFileId(root);
    result.volume.lookup = LinkNtfsEntries(result.entries, fileIds, parentIds, rootId);
    // Without the root's id, parents cannot be told apart from unindexed places: rebuild periodically instead.
    result.volume.live = rootId != 0;
    result.volume.rootId = rootId;
    result.volume.journalId = journalData.UsnJournalID;
    result.volume.nextUsn = journalData.NextUsn;
    result.volume.handle = std::move(volume);
    return !result.entries.empty();
}

SearchIndex::IdLookup SearchIndex::LinkNtfsEntries(
    std::vector<IndexEntry>& entries,
    const std::vector<uint64_t>& fileIds,
    const std::vector<uint64_t>& parentIds,
    uint64_t rootId) {
    const uint32_t count = static_cast<uint32_t>(entries.size());

    std::vector<uint32_t> order(count);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t left, uint32_t right) { return fileIds[left] < fileIds[right]; });
    std::vector<uint64_t> sortedIds(count);
    for (uint32_t k = 0; k < count; ++k) {
        sortedIds[k] = fileIds[order[k]];
    }
    auto find = [&](uint64_t id) {
        const auto it = std::lower_bound(sortedIds.begin(), sortedIds.end(), id);
        return it != sortedIds.end() && *it == id ? order[static_cast<size_t>(it - sortedIds.begin())] : kInvalidIndex;
    };

    enum : uint8_t { kUnknown, kReachable, kOrphan, kVisiting };
    std::vector<uint8_t> state(count, kUnknown);
    std::vector<uint32_t> parent(count, kInvalidIndex);
    for (uint32_t index = 0; index < count; ++index) {
        if (rootId != 0 && fileIds[index] == rootId) {
            state[index] = kOrphan;  // the root folder itself
            continue;
        }
        if (rootId != 0 && parentIds[index] == rootId) {
            continue;  // directly under the root
        }
        const uint32_t found = find(parentIds[index]);
        if (found == index) {
            state[index] = kOrphan;
        } else if (found != kInvalidIndex) {
            parent[index] = found;
        } else if (rootId != 0) {
            state[index] = kOrphan;  // parent is not a listed folder (system metadata and the like)
        }
    }

    // Everything must lead to the root through listed folders.
    std::vector<uint32_t> chain;
    for (uint32_t index = 0; index < count; ++index) {
        chain.clear();
        uint32_t current = index;
        uint8_t outcome = kReachable;
        while (true) {
            if (state[current] == kReachable || state[current] == kOrphan) {
                outcome = state[current];
                break;
            }
            if (state[current] == kVisiting) {
                outcome = kOrphan;  // cycle
                break;
            }
            state[current] = kVisiting;
            chain.push_back(current);
            if (parent[current] == kInvalidIndex) {
                break;
            }
            current = parent[current];
        }
        for (const uint32_t item : chain) {
            state[item] = outcome;
        }
    }

    std::vector<uint32_t> remap(count, kInvalidIndex);
    uint32_t kept = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (state[index] == kReachable) {
            remap[index] = kept++;
        }
    }

    std::vector<IndexEntry> linked;
    linked.reserve(kept);
    for (uint32_t index = 0; index < count; ++index) {
        if (state[index] == kReachable) {
            IndexEntry entry = entries[index];
            entry.parentIndex = parent[index] == kInvalidIndex ? kInvalidIndex : remap[parent[index]];
            linked.push_back(entry);
        }
    }
    entries.swap(linked);

    IdLookup lookup;
    lookup.ids.reserve(kept);
    lookup.indices.reserve(kept);
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t index = order[k];
        if (state[index] == kReachable) {
            lookup.ids.push_back(sortedIds[k]);
            lookup.indices.push_back(remap[index]);
        }
    }
    return lookup;
}

void SearchIndex::BuildFromRecursiveScan(const std::wstring& root, VolumeBuildResult& result, const std::atomic_bool& cancel) {
    struct DirectoryWorkItem {
        std::wstring scanPath;
        uint32_t parentIndex = kInvalidIndex;
    };

    std::deque<DirectoryWorkItem> queue;
    queue.push_back({ToExtendedPath(root), kInvalidIndex});

    while (!queue.empty() && !cancel.load(std::memory_order_relaxed)) {
        DirectoryWorkItem work = std::move(queue.front());
        queue.pop_front();

        WIN32_FIND_DATAW findData = {};
        const std::wstring searchPattern = JoinPath(work.scanPath, L"*");
        HANDLE findHandle = FindFirstFileExW(searchPattern.c_str(), FindExInfoBasic, &findData, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (findHandle == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            const std::wstring_view name(findData.cFileName);
            if (name == L"." || name == L"..") {
                continue;
            }

            const bool isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            IndexEntry entry;
            entry.parentIndex = work.parentIndex;
            entry.flags = isDirectory ? kDirectoryFlag : 0;
            AppendName(result.pool, name, entry);

            const uint32_t currentIndex = static_cast<uint32_t>(result.entries.size());
            result.entries.push_back(entry);

            if (isDirectory && (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
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

    thread_local std::vector<std::wstring_view> segments;
    segments.clear();
    for (uint32_t current = index; current != kInvalidIndex && segments.size() < 4096; current = entries_[current].parentIndex) {
        segments.push_back(Name(entries_[current]));
    }

    std::wstring path = volumes_[entries_[index].volumeIndex].root;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        if (!path.empty() && path.back() != L'\\') {
            path.push_back(L'\\');
        }
        path.append(*it);
    }
    return path;
}

void SearchIndex::BuildLowerPath(uint32_t index, std::wstring& path) const {
    thread_local std::vector<std::wstring_view> segments;
    segments.clear();
    for (uint32_t current = index; current != kInvalidIndex && segments.size() < 4096; current = entries_[current].parentIndex) {
        segments.push_back(Lower(entries_[current]));
    }

    path = volumes_[entries_[index].volumeIndex].root;
    if (!path.empty()) {
        path[0] = static_cast<wchar_t>(towlower(path[0]));
    }
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        if (!path.empty() && path.back() != L'\\') {
            path.push_back(L'\\');
        }
        path.append(*it);
    }
}

void SearchIndex::RefreshExcludedPaths(const ExcludeRules& rules) const {
    if (rules.version == excludedKeyRules_ && generation_ == excludedKeyGeneration_) {
        return;
    }
    excludedKeyRules_ = rules.version;
    excludedKeyGeneration_ = generation_;
    excludedEntries_.clear();
    excludedVolumes_.clear();

    // Full-path rules become entry indices once, so the search only has to compare indices.
    std::vector<std::pair<std::wstring, const std::wstring*>> lastParts;
    for (const auto& rule : rules.paths) {
        if (rule.size() == 3 && rule[1] == L':') {
            for (uint16_t volumeIndex = 0; volumeIndex < volumes_.size(); ++volumeIndex) {
                if (ToLower(volumes_[volumeIndex].root) == rule) {
                    excludedVolumes_.push_back(volumeIndex);
                }
            }
            continue;
        }
        lastParts.emplace_back(rule.substr(rule.find_last_of(L'\\') + 1), &rule);
    }

    std::wstring path;
    if (!lastParts.empty()) {
        for (uint32_t index = 0; index < entries_.size(); ++index) {
            if (entries_[index].IsDeleted()) {
                continue;
            }
            const std::wstring_view lower = Lower(entries_[index]);
            for (const auto& [last, rule] : lastParts) {
                if (lower == last) {
                    BuildLowerPath(index, path);
                    if (path == *rule) {
                        excludedEntries_.push_back(index);
                    }
                }
            }
        }
    }
    std::sort(excludedEntries_.begin(), excludedEntries_.end());
    std::sort(excludedVolumes_.begin(), excludedVolumes_.end());
}

bool SearchIndex::IsExcluded(uint32_t index, const ExcludeRules& rules, std::unordered_map<uint32_t, bool>& memo) const {
    if (std::binary_search(excludedVolumes_.begin(), excludedVolumes_.end(), entries_[index].volumeIndex)) {
        return true;
    }

    thread_local std::vector<uint32_t> chain;
    chain.clear();
    bool excluded = false;
    for (uint32_t current = index; current != kInvalidIndex && chain.size() < 4096; current = entries_[current].parentIndex) {
        if (const auto found = memo.find(current); found != memo.end()) {
            excluded = found->second;
            break;
        }
        chain.push_back(current);
        const std::wstring_view lower = Lower(entries_[current]);
        if (std::find(rules.names.begin(), rules.names.end(), lower) != rules.names.end() ||
            std::binary_search(excludedEntries_.begin(), excludedEntries_.end(), current)) {
            excluded = true;
            break;
        }
    }

    for (const uint32_t item : chain) {
        if (entries_[item].IsDirectory()) {
            memo[item] = excluded;
        }
    }
    return excluded;
}

}  // namespace chatda
