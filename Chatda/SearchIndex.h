#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace chatda {

constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

struct SearchResultRow {
    std::wstring name;
    std::wstring path;
    std::wstring type;
    bool isDirectory = false;
};

class SearchIndex {
public:
    // Returns false when nothing was indexed or `cancel` became true.
    bool Build(const std::atomic_bool& cancel);
    bool LoadCache();
    bool SaveCache() const;
    size_t Size() const;
    std::vector<SearchResultRow> Search(const std::wstring& query, uint64_t expectedRequestId, const std::atomic_uint64_t& latestRequestId) const;
    static std::wstring CachePath();

private:
    struct IndexEntry {
        std::wstring name;
        std::wstring lowerName;  // empty when name is already lowercase, to save memory
        uint32_t parentIndex = kInvalidIndex;
        uint16_t volumeIndex = 0;
        bool isDirectory = false;

        const std::wstring& Lower() const { return lowerName.empty() ? name : lowerName; }
    };

    struct VolumeBuildResult {
        std::wstring root;
        std::vector<IndexEntry> entries;
    };

    static VolumeBuildResult BuildSingleVolume(const std::wstring& root, const std::atomic_bool& cancel);
    static bool TryBuildFromUsn(const std::wstring& root, VolumeBuildResult& result, const std::atomic_bool& cancel);
    static void BuildFromRecursiveScan(const std::wstring& root, VolumeBuildResult& result, const std::atomic_bool& cancel);
    static bool IsNtfs(const std::wstring& root);
    static std::wstring DevicePathForRoot(const std::wstring& root);
    std::wstring BuildPath(uint32_t index) const;

    std::vector<std::wstring> volumes_;
    std::vector<IndexEntry> entries_;
};

}  // namespace chatda
