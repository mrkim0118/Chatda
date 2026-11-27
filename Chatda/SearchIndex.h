#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chatda {

constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

struct SearchResultRow {
    std::wstring name;
    std::wstring path;
    bool isDirectory = false;

    // Filled by the UI: position in relevance order, and size/date read lazily from disk.
    uint32_t relevance = 0;
    bool infoLoaded = false;
    bool hasInfo = false;
    uint64_t size = 0;
    uint64_t modified = 0;  // FILETIME as a 64-bit value
};

struct SearchOutcome {
    std::vector<SearchResultRow> rows;
    size_t totalMatches = 0;
};

// Lowercase rules from exclude.txt. A name hides every item with that exact name and
// everything inside it; a full path hides that folder and everything inside it.
struct ExcludeRules {
    std::vector<std::wstring> names;
    std::vector<std::wstring> paths;
    uint64_t version = 0;

    bool Empty() const { return names.empty() && paths.empty(); }
};

ExcludeRules ParseExcludeRules(const std::wstring& text);
std::wstring DataDirectory();
std::wstring ExcludeFilePath();
// Creates exclude.txt with `defaultText` when it does not exist yet.
void EnsureExcludeFile(const std::wstring& defaultText);
// Last write time of exclude.txt, or 0 when it does not exist.
uint64_t ExcludeFileStamp();
ExcludeRules LoadExcludeRules();

// One change read from an NTFS USN journal.
struct JournalChange {
    uint64_t fileId = 0;
    uint64_t parentId = 0;
    std::wstring name;
    bool isDirectory = false;
    bool deleted = false;
};

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    ~UniqueHandle() { Reset(); }

    HANDLE Get() const { return handle_; }
    void Reset() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

private:
    HANDLE handle_ = nullptr;
};

class SearchIndex {
public:
    enum class PollResult { None, Changed, Rebuild };

    // Returns false when nothing was indexed or `cancel` became true.
    bool Build(const std::atomic_bool& cancel);
    bool LoadCache();
    bool SaveCache() const;
    // Number of files and folders currently indexed.
    size_t Size() const { return liveCount_.load(std::memory_order_relaxed); }
    // Returns no rows when the request was superseded by a newer one.
    SearchOutcome Search(const std::wstring& query, uint64_t expectedRequestId, const std::atomic_uint64_t& latestRequestId, const ExcludeRules& rules) const;

    // Applies new USN journal records of NTFS volumes. Rebuild means a journal could no longer be
    // followed; that volume then only changes with the next full rebuild.
    PollResult PollJournals();
    // True when some volume is not followed through its journal, so only a full rebuild picks up its changes.
    bool NeedsPeriodicRebuild() const;
    // True when a drive was added or removed since Build.
    bool DrivesChanged() const;

    static std::wstring CachePath();

private:
    struct IndexEntry {
        uint32_t nameOffset = 0;   // into pool_
        uint32_t lowerOffset = 0;  // equals nameOffset when the name is already lowercase
        uint32_t parentIndex = kInvalidIndex;
        uint16_t nameLength = 0;
        uint16_t volumeIndex = 0;
        uint8_t flags = 0;

        bool IsDirectory() const { return (flags & kDirectoryFlag) != 0; }
        bool IsDeleted() const { return (flags & kDeletedFlag) != 0; }
    };
    static constexpr uint8_t kDirectoryFlag = 1;
    static constexpr uint8_t kDeletedFlag = 2;

    // NTFS file id -> entry index: a sorted array from the build plus a map for files created later.
    struct IdLookup {
        std::vector<uint64_t> ids;
        std::vector<uint32_t> indices;
        std::unordered_map<uint64_t, uint32_t> added;

        uint32_t Find(uint64_t id) const;
    };

    struct Volume {
        std::wstring root;
        bool live = false;  // followed through the USN journal
        uint64_t rootId = 0;
        uint64_t journalId = 0;
        int64_t nextUsn = 0;
        UniqueHandle handle;
        IdLookup lookup;
    };

    struct VolumeBuildResult {
        Volume volume;
        std::vector<IndexEntry> entries;
        std::wstring pool;
    };

    static VolumeBuildResult BuildSingleVolume(const std::wstring& root, const std::atomic_bool& cancel);
    static bool TryBuildFromUsn(const std::wstring& root, VolumeBuildResult& result, const std::atomic_bool& cancel);
    static void BuildFromRecursiveScan(const std::wstring& root, VolumeBuildResult& result, const std::atomic_bool& cancel);
    // Links entries to their parents and drops those not reachable from the volume root.
    static IdLookup LinkNtfsEntries(std::vector<IndexEntry>& entries, const std::vector<uint64_t>& fileIds, const std::vector<uint64_t>& parentIds, uint64_t rootId);
    static void AppendName(std::wstring& pool, std::wstring_view name, IndexEntry& entry);
    static uint32_t EligibleDriveMask();
    static bool IsNtfs(const std::wstring& root);
    static std::wstring DevicePathForRoot(const std::wstring& root);

    void ApplyChanges(uint16_t volumeIndex, const std::vector<JournalChange>& changes);
    std::wstring_view Name(const IndexEntry& entry) const { return {pool_.data() + entry.nameOffset, entry.nameLength}; }
    std::wstring_view Lower(const IndexEntry& entry) const { return {pool_.data() + entry.lowerOffset, entry.nameLength}; }
    std::wstring BuildPath(uint32_t index) const;
    void BuildLowerPath(uint32_t index, std::wstring& path) const;
    bool IsExcluded(uint32_t index, const ExcludeRules& rules, std::unordered_map<uint32_t, bool>& memo) const;
    void RefreshExcludedPaths(const ExcludeRules& rules) const;

    std::vector<Volume> volumes_;
    std::vector<IndexEntry> entries_;
    std::wstring pool_;
    uint32_t driveMask_ = 0;
    std::atomic_size_t liveCount_ = 0;
    uint64_t generation_ = 0;  // bumped by every change, invalidates the excluded-path cache

    // Readers (Search) take it shared; ApplyChanges takes it exclusively.
    mutable std::shared_mutex mutex_;

    // Index entries and volumes matched by full-path exclude rules; only used by the search thread.
    mutable uint64_t excludedKeyRules_ = UINT64_MAX;
    mutable uint64_t excludedKeyGeneration_ = UINT64_MAX;
    mutable std::vector<uint32_t> excludedEntries_;
    mutable std::vector<uint16_t> excludedVolumes_;
};

}  // namespace chatda
