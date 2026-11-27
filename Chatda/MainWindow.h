#pragma once

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

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
    // When Chatda already runs, brings its window up and returns true.
    static bool ActivateRunningInstance();
    // Handles keyboard shortcuts before normal dispatch; returns true when the message was used.
    bool PreTranslate(const MSG& message);

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
    int Scale(int value) const;

    void StartWorkers();
    void StopWorkers();
    void StartUpdateThread();
    std::wstring CurrentQuery() const;
    void QueueSearch();
    void IndexLoop();
    void SearchLoop();

    struct SearchPayload;
    void ApplyResults(std::unique_ptr<SearchPayload> payload);
    void SortResults();
    void UpdateSortArrows() const;
    void OnColumnClick(int column);
    void EnsureFileInfo(SearchResultRow& row) const;
    void UpdateStatus();
    void PopulateVirtualItem(LPARAM payload);

    int SelectedRow() const;
    int RowFromScreenPoint(POINT screenPoint) const;
    POINT ContextMenuPointForRow(int row) const;
    void SelectOnlyRow(int row) const;
    void ShowResultContextMenu(POINT screenPoint);
    bool OpenResult(const SearchResultRow& row) const;
    bool OpenContainingFolder(const SearchResultRow& row) const;
    void OpenRow(int row, bool containingFolder) const;
    void CopySelectedPaths() const;
    void FocusSearch() const;
    int ImageIndexForResult(const SearchResultRow& row);
    int QuerySystemImageIndex(const SearchResultRow& row) const;
    std::wstring IconCacheKeyForResult(const SearchResultRow& row) const;

    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT screenPoint);
    void ShowAndFocus();
    void HideToTray();
    void ToggleFromHotkey();
    void EditExclusions() const;
    void ExitApp();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND searchEdit_ = nullptr;
    HWND listView_ = nullptr;
    HWND statusBar_ = nullptr;
    HWND lastFocus_ = nullptr;  // search box or result list, restored when the window is activated
    HFONT listFont_ = nullptr;
    HFONT searchFont_ = nullptr;
    HIMAGELIST systemImageList_ = nullptr;
    HICON trayIcon_ = nullptr;
    UINT dpi_ = 96;
    int searchControlHeight_ = 34;
    int searchTextHeight_ = 18;
    std::unordered_map<std::wstring, int> iconIndexCache_;

    bool trayAdded_ = false;
    bool trayHintShown_ = false;
    bool hotkeyRegistered_ = false;
    bool exiting_ = false;
    UINT taskbarCreatedMessage_ = 0;

    std::mutex indexMutex_;
    std::condition_variable indexCondition_;
    std::shared_ptr<SearchIndex> searchIndex_;
    int indexState_ = 0;  // see kIndex* in MainWindow.cpp

    std::mutex searchMutex_;
    std::condition_variable searchCondition_;
    std::wstring pendingQuery_;
    uint64_t pendingRequestId_ = 0;
    bool searchRequested_ = false;

    std::vector<SearchResultRow> results_;
    std::wstring resultsQuery_;
    size_t totalMatches_ = 0;
    int sortColumn_ = -1;  // -1: relevance order
    bool sortAscending_ = true;

    std::thread indexThread_;
    std::thread searchThread_;
    std::thread updateThread_;
    // Shared with the update thread, which may outlive this window (it is detached on exit).
    std::shared_ptr<std::atomic_bool> updateCancel_ = std::make_shared<std::atomic_bool>(false);

    std::atomic_bool shutdown_ = false;
    std::atomic_uint64_t nextSearchRequestId_ = 0;
    std::atomic_uint64_t latestRequestedSearchId_ = 0;
};

}  // namespace chatda
