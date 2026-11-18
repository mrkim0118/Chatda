#pragma once

#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "SearchIndex.h"

namespace chatda {

class MainWindow {
public:
    int Run(HINSTANCE instance, int commandShow);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SearchEditSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClass();
    void CreateControls();
    void InsertColumns();
    void ResizeChildren();
    void InitializeFonts();
    void InitializeSystemImageList();
    void UpdateSearchEditLayout() const;
    void ReleaseResources();
    void StartWorkers();
    void StopWorkers();
    void OpenExternalUrl(const wchar_t* url) const;
    std::wstring CurrentQuery() const;
    void QueueSearch();
    void IndexLoop();
    void SearchLoop();
    void ApplyResults(std::vector<SearchResultRow> rows);
    void PopulateVirtualItem(LPARAM payload);
    int SelectedRow() const;
    int RowFromScreenPoint(POINT screenPoint) const;
    POINT ContextMenuPointForRow(int row) const;
    void SelectOnlyRow(int row) const;
    void ShowResultContextMenu(POINT screenPoint);
    bool OpenResult(const SearchResultRow& row) const;
    bool OpenContainingFolder(const SearchResultRow& row) const;
    int ImageIndexForResult(const SearchResultRow& row);
    int QuerySystemImageIndex(const SearchResultRow& row) const;
    std::wstring IconCacheKeyForResult(const SearchResultRow& row) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND searchEdit_ = nullptr;
    HWND donateButton_ = nullptr;
    HWND homepageButton_ = nullptr;
    HWND listView_ = nullptr;
    HFONT listFont_ = nullptr;
    HFONT searchFont_ = nullptr;
    HIMAGELIST systemImageList_ = nullptr;
    int searchControlHeight_ = 34;
    int searchTextHeight_ = 18;
    std::unordered_map<std::wstring, int> iconIndexCache_;

    std::mutex indexMutex_;
    std::condition_variable indexCondition_;
    std::shared_ptr<SearchIndex> searchIndex_;

    std::mutex searchMutex_;
    std::condition_variable searchCondition_;
    std::wstring pendingQuery_;
    uint64_t pendingRequestId_ = 0;
    bool searchRequested_ = false;

    std::vector<SearchResultRow> results_;

    std::thread indexThread_;
    std::thread searchThread_;

    std::atomic_bool shutdown_ = false;
    std::atomic_uint64_t nextSearchRequestId_ = 0;
    std::atomic_uint64_t latestRequestedSearchId_ = 0;
};

}  // namespace chatda
