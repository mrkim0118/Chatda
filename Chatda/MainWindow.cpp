#include "MainWindow.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <windowsx.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlguid.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <utility>

#include "resource.h"

namespace chatda {
namespace {

constexpr auto kIndexRefreshInterval = std::chrono::minutes(10);

// The app runs as admin (needed for the fast NTFS index). Launch through the desktop
// Explorer so opened files and URLs run with normal user rights, not as admin.
bool ShellExecuteAsUser(HWND owner, const wchar_t* file, const wchar_t* parameters) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IShellWindows> shellWindows;
    ComPtr<IDispatch> desktopDispatch;
    ComPtr<IServiceProvider> serviceProvider;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> background;
    ComPtr<IShellFolderViewDual> folderView;
    ComPtr<IDispatch> application;
    ComPtr<IShellDispatch2> shell;
    VARIANT empty = {};
    long desktopHwnd = 0;

    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shellWindows))) &&
        shellWindows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &desktopHwnd, SWFO_NEEDDISPATCH, &desktopDispatch) == S_OK &&
        SUCCEEDED(desktopDispatch.As(&serviceProvider)) &&
        SUCCEEDED(serviceProvider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) &&
        SUCCEEDED(browser->QueryActiveShellView(&view)) &&
        SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) &&
        SUCCEEDED(background.As(&folderView)) &&
        SUCCEEDED(folderView->get_Application(&application)) &&
        SUCCEEDED(application.As(&shell))) {
        BSTR fileText = SysAllocString(file);
        VARIANT args = {};
        if (parameters != nullptr) {
            args.vt = VT_BSTR;
            args.bstrVal = SysAllocString(parameters);
        }
        VARIANT show = {};
        show.vt = VT_I4;
        show.lVal = SW_SHOWNORMAL;
        const HRESULT result = shell->ShellExecute(fileText, args, empty, empty, show);
        SysFreeString(fileText);
        VariantClear(&args);
        if (SUCCEEDED(result)) {
            return true;
        }
    }

    // No desktop Explorer (e.g. a replacement shell): fall back to a direct launch.
    const HINSTANCE result = ShellExecuteW(owner, L"open", file, parameters, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

std::wstring LowerExtension(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    if (!extension.empty()) {
        CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
    }
    return extension;
}

// These file types carry their own icon, so the icon must come from the file itself.
bool HasOwnIcon(const std::wstring& lowerExtension) {
    return lowerExtension == L".exe" || lowerExtension == L".ico" || lowerExtension == L".lnk" || lowerExtension == L".url";
}

constexpr wchar_t kWindowClassName[] = L"ChatdaWindow";
constexpr wchar_t kAppTitle[] = L"Chatda";
constexpr wchar_t kIndexingTitle[] = L"Chatda - Indexing...";

constexpr int kControlMargin = 12;
constexpr int kDefaultSearchHeight = 34;
constexpr int kHeaderButtonWidth = 108;
constexpr int kHeaderButtonGap = 8;
constexpr UINT_PTR kSearchDebounceTimerId = 1001;
constexpr UINT kSearchDebounceMs = 55;
constexpr UINT kMessageIndexReady = WM_APP + 1;
constexpr UINT kMessageSearchComplete = WM_APP + 2;
constexpr UINT kContextCommandOpen = 1;
constexpr UINT kContextCommandOpenLocation = 2;
constexpr UINT kShellCommandBase = 100;
constexpr int kDonateButtonId = 201;
constexpr int kHomepageButtonId = 202;
constexpr wchar_t kDonateUrl[] = L"https://www.paypal.com/ncp/payment/T2KAST2XQC6BS";
constexpr wchar_t kHomepageUrl[] = L"https://tech.trendarchive.io/";

struct SearchPayload {
    uint64_t requestId = 0;
    std::vector<SearchResultRow> rows;
};

}  // namespace

int MainWindow::Run(HINSTANCE instance, int commandShow) {
    instance_ = instance;
    if (!RegisterWindowClass()) {
        return 0;
    }

    hwnd_ = CreateWindowExW(
        0,
        kWindowClassName,
        kIndexingTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1080,
        720,
        nullptr,
        nullptr,
        instance_,
        this);

    if (hwnd_ == nullptr) {
        return 0;
    }

    ShowWindow(hwnd_, commandShow);
    UpdateWindow(hwnd_);
    StartWorkers();

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    StopWorkers();
    ReleaseResources();
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(createStruct->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::SearchEditSubclassProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR refData) {
    auto* self = reinterpret_cast<MainWindow*>(refData);
    if (self == nullptr) {
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_CHAR:
        if (wParam == VK_RETURN) {
            return 0;
        }
        break;

    case WM_PASTE:
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            break;
        }

        if (OpenClipboard(hwnd) != FALSE) {
            HANDLE dataHandle = GetClipboardData(CF_UNICODETEXT);
            if (dataHandle != nullptr) {
                const auto* rawText = static_cast<const wchar_t*>(GlobalLock(dataHandle));
                if (rawText != nullptr) {
                    std::wstring normalized(rawText);
                    for (auto& ch : normalized) {
                        if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
                            ch = L' ';
                        }
                    }
                    GlobalUnlock(dataHandle);
                    CloseClipboard();
                    SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(normalized.c_str()));
                    return 0;
                }
                GlobalUnlock(dataHandle);
            }
            CloseClipboard();
        }
        break;
    }

    return DefSubclassProc(hwnd, message, wParam, lParam);
}

bool MainWindow::RegisterWindowClass() {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 20, 20, LR_DEFAULTCOLOR));
    return RegisterClassExW(&windowClass) != 0;
}

void MainWindow::CreateControls() {
    searchEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_EDITW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_AUTOVSCROLL | ES_MULTILINE,
        0,
        0,
        0,
        0,
        hwnd_,
        reinterpret_cast<HMENU>(101),
        instance_,
        nullptr);

    SetWindowSubclass(searchEdit_, SearchEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(searchEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Search file or folder name"));

    donateButton_ = CreateWindowExW(
        0,
        WC_BUTTONW,
        L"Donate",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0,
        0,
        0,
        0,
        hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDonateButtonId)),
        instance_,
        nullptr);

    homepageButton_ = CreateWindowExW(
        0,
        WC_BUTTONW,
        L"Homepage",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0,
        0,
        0,
        0,
        hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHomepageButtonId)),
        instance_,
        nullptr);

    listView_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS | WS_TABSTOP,
        0,
        0,
        0,
        0,
        hwnd_,
        reinterpret_cast<HMENU>(102),
        instance_,
        nullptr);

    ListView_SetUnicodeFormat(listView_, TRUE);
    ListView_SetExtendedListViewStyle(listView_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    InitializeFonts();
    InitializeSystemImageList();
    InsertColumns();
    ResizeChildren();
    SetFocus(searchEdit_);
}

void MainWindow::InsertColumns() {
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    column.pszText = const_cast<LPWSTR>(L"Name");
    column.cx = 320;
    ListView_InsertColumn(listView_, 0, &column);

    column.pszText = const_cast<LPWSTR>(L"Path");
    column.cx = 620;
    column.iSubItem = 1;
    ListView_InsertColumn(listView_, 1, &column);

    column.pszText = const_cast<LPWSTR>(L"Type");
    column.cx = 100;
    column.iSubItem = 2;
    ListView_InsertColumn(listView_, 2, &column);
}

void MainWindow::ResizeChildren() {
    if (hwnd_ == nullptr || searchEdit_ == nullptr || listView_ == nullptr || donateButton_ == nullptr || homepageButton_ == nullptr) {
        return;
    }

    RECT rect = {};
    GetClientRect(hwnd_, &rect);

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int topRowY = kControlMargin;
    const int buttonsWidth = (kHeaderButtonWidth * 2) + kHeaderButtonGap;
    const int searchWidth = std::max(240, width - (kControlMargin * 2) - buttonsWidth - kHeaderButtonGap);

    MoveWindow(searchEdit_, kControlMargin, topRowY, searchWidth, searchControlHeight_, TRUE);
    MoveWindow(
        donateButton_,
        width - kControlMargin - buttonsWidth,
        topRowY,
        kHeaderButtonWidth,
        searchControlHeight_,
        TRUE);
    MoveWindow(
        homepageButton_,
        width - kControlMargin - kHeaderButtonWidth,
        topRowY,
        kHeaderButtonWidth,
        searchControlHeight_,
        TRUE);
    MoveWindow(
        listView_,
        kControlMargin,
        kControlMargin + searchControlHeight_ + 10,
        width - (kControlMargin * 2),
        height - (kControlMargin * 2) - searchControlHeight_ - 10,
        TRUE);

    const int listWidth = (width - (kControlMargin * 2));
    ListView_SetColumnWidth(listView_, 0, std::max(220, listWidth / 4));
    ListView_SetColumnWidth(listView_, 1, std::max(360, listWidth - 360));
    ListView_SetColumnWidth(listView_, 2, 100);
    UpdateSearchEditLayout();
}

void MainWindow::InitializeFonts() {
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        return;
    }

    const UINT dpi = hwnd_ != nullptr ? GetDpiForWindow(hwnd_) : 96;

    LOGFONTW listFontSpec = metrics.lfMessageFont;
    listFontSpec.lfHeight = -MulDiv(9, dpi, 72);

    LOGFONTW searchFontSpec = metrics.lfMessageFont;
    searchFontSpec.lfHeight = -MulDiv(12, dpi, 72);

    if (listFont_ != nullptr) {
        DeleteObject(listFont_);
        listFont_ = nullptr;
    }
    if (searchFont_ != nullptr) {
        DeleteObject(searchFont_);
        searchFont_ = nullptr;
    }

    listFont_ = CreateFontIndirectW(&listFontSpec);
    searchFont_ = CreateFontIndirectW(&searchFontSpec);

    if (listFont_ != nullptr) {
        SendMessageW(listView_, WM_SETFONT, reinterpret_cast<WPARAM>(listFont_), TRUE);
        SendMessageW(donateButton_, WM_SETFONT, reinterpret_cast<WPARAM>(listFont_), TRUE);
        SendMessageW(homepageButton_, WM_SETFONT, reinterpret_cast<WPARAM>(listFont_), TRUE);
    }
    if (searchFont_ != nullptr) {
        SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(searchFont_), TRUE);
    }

    SendMessageW(searchEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));

    HDC dc = GetDC(searchEdit_);
    if (dc != nullptr) {
        HFONT previousFont = searchFont_ != nullptr ? reinterpret_cast<HFONT>(SelectObject(dc, searchFont_)) : nullptr;
        TEXTMETRICW metricsInfo = {};
        if (GetTextMetricsW(dc, &metricsInfo)) {
            searchTextHeight_ = static_cast<int>(metricsInfo.tmHeight + metricsInfo.tmExternalLeading);
            const int measuredHeight = searchTextHeight_ + 18;
            searchControlHeight_ = std::max(kDefaultSearchHeight, measuredHeight);
        } else {
            searchTextHeight_ = 18;
            searchControlHeight_ = kDefaultSearchHeight;
        }
        if (searchFont_ != nullptr && previousFont != nullptr) {
            SelectObject(dc, previousFont);
        }
        ReleaseDC(searchEdit_, dc);
    } else {
        searchTextHeight_ = 18;
        searchControlHeight_ = kDefaultSearchHeight;
    }
}

void MainWindow::InitializeSystemImageList() {
    SHFILEINFOW fileInfo = {};
    systemImageList_ = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(
        L".txt",
        FILE_ATTRIBUTE_NORMAL,
        &fileInfo,
        sizeof(fileInfo),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));

    if (systemImageList_ != nullptr) {
        ListView_SetImageList(listView_, systemImageList_, LVSIL_SMALL);
    }
}

void MainWindow::UpdateSearchEditLayout() const {
    if (searchEdit_ == nullptr) {
        return;
    }

    RECT clientRect = {};
    GetClientRect(searchEdit_, &clientRect);

    const int clientHeight = clientRect.bottom - clientRect.top;
    const int verticalPadding = std::max(2, (clientHeight - searchTextHeight_) / 2);
    RECT textRect = {
        10,
        verticalPadding - 1,
        std::max(11L, clientRect.right - 10),
        std::max(verticalPadding + searchTextHeight_ + 1, static_cast<int>(clientRect.bottom) - verticalPadding + 1)
    };

    SendMessageW(searchEdit_, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&textRect));
    InvalidateRect(searchEdit_, nullptr, TRUE);
}

void MainWindow::ReleaseResources() {
    if (searchEdit_ != nullptr) {
        RemoveWindowSubclass(searchEdit_, SearchEditSubclassProc, 1);
    }
    if (searchFont_ != nullptr) {
        DeleteObject(searchFont_);
        searchFont_ = nullptr;
    }
    if (listFont_ != nullptr) {
        DeleteObject(listFont_);
        listFont_ = nullptr;
    }
}

void MainWindow::StartWorkers() {
    shutdown_.store(false, std::memory_order_relaxed);
    searchThread_ = std::thread([this]() { SearchLoop(); });
    indexThread_ = std::thread([this]() { IndexLoop(); });
}

void MainWindow::StopWorkers() {
    shutdown_.store(true, std::memory_order_relaxed);
    {
        std::lock_guard lock(searchMutex_);
        searchRequested_ = true;
    }
    searchCondition_.notify_all();
    {
        // Taking the lock makes sure IndexLoop is either waiting or will see shutdown_.
        std::lock_guard lock(indexMutex_);
    }
    indexCondition_.notify_all();

    if (indexThread_.joinable()) {
        indexThread_.join();
    }
    if (searchThread_.joinable()) {
        searchThread_.join();
    }
}

void MainWindow::OpenExternalUrl(const wchar_t* url) const {
    if (url == nullptr || *url == L'\0') {
        return;
    }
    ShellExecuteAsUser(hwnd_, url, nullptr);
}

std::wstring MainWindow::CurrentQuery() const {
    if (searchEdit_ == nullptr) {
        return {};
    }

    const int length = GetWindowTextLengthW(searchEdit_);
    if (length <= 0) {
        return {};
    }

    std::wstring query(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(searchEdit_, query.data(), length + 1);
    query.resize(static_cast<size_t>(length));
    query.erase(std::remove(query.begin(), query.end(), L'\r'), query.end());
    query.erase(std::remove(query.begin(), query.end(), L'\n'), query.end());
    return query;
}

void MainWindow::QueueSearch() {
    const uint64_t requestId = nextSearchRequestId_.fetch_add(1, std::memory_order_relaxed) + 1;
    latestRequestedSearchId_.store(requestId, std::memory_order_relaxed);

    {
        std::lock_guard lock(searchMutex_);
        pendingQuery_ = CurrentQuery();
        pendingRequestId_ = requestId;
        searchRequested_ = true;
    }

    searchCondition_.notify_one();
}

void MainWindow::IndexLoop() {
    bool hasIndex = false;
    auto cachedIndex = std::make_shared<SearchIndex>();
    if (cachedIndex->LoadCache()) {
        hasIndex = true;
        if (shutdown_.load(std::memory_order_relaxed)) {
            return;
        }

        {
            std::lock_guard lock(indexMutex_);
            searchIndex_ = cachedIndex;
        }

        PostMessageW(hwnd_, kMessageIndexReady, 1, 0);
    }
    cachedIndex.reset();

    // shortcut: full rebuild on a timer, so new files show up within kIndexRefreshInterval.
    // Upgrade to reading USN journal changes (FSCTL_READ_USN_JOURNAL) if instant updates are needed.
    for (bool firstBuild = true;; firstBuild = false) {
        auto index = std::make_shared<SearchIndex>();
        const bool buildSucceeded = index->Build(shutdown_);

        if (shutdown_.load(std::memory_order_relaxed)) {
            return;
        }

        if (buildSucceeded) {
            if (firstBuild) {
                index->SaveCache();  // once per run: the cache only speeds up the next start
            }

            {
                std::lock_guard lock(indexMutex_);
                searchIndex_ = std::move(index);
            }

            hasIndex = true;
            PostMessageW(hwnd_, kMessageIndexReady, 2, 0);
        } else if (!hasIndex) {
            PostMessageW(hwnd_, kMessageIndexReady, 0, 0);
        }

        std::unique_lock lock(indexMutex_);
        if (indexCondition_.wait_for(lock, kIndexRefreshInterval, [&]() { return shutdown_.load(std::memory_order_relaxed); })) {
            return;
        }
    }
}

void MainWindow::SearchLoop() {
    while (true) {
        std::wstring query;
        uint64_t requestId = 0;

        {
            std::unique_lock lock(searchMutex_);
            searchCondition_.wait(lock, [&]() { return searchRequested_ || shutdown_.load(std::memory_order_relaxed); });
            if (shutdown_.load(std::memory_order_relaxed)) {
                break;
            }
            query = pendingQuery_;
            requestId = pendingRequestId_;
            searchRequested_ = false;
        }

        auto payload = std::make_unique<SearchPayload>();
        payload->requestId = requestId;

        std::shared_ptr<SearchIndex> index;
        {
            std::lock_guard lock(indexMutex_);
            index = searchIndex_;
        }

        if (index != nullptr) {
            payload->rows = index->Search(query, requestId, latestRequestedSearchId_);
        }

        if (shutdown_.load(std::memory_order_relaxed)) {
            break;
        }

        PostMessageW(hwnd_, kMessageSearchComplete, 0, reinterpret_cast<LPARAM>(payload.release()));
    }
}

void MainWindow::ApplyResults(std::vector<SearchResultRow> rows) {
    results_ = std::move(rows);
    ListView_SetItemCountEx(listView_, static_cast<int>(results_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(listView_, nullptr, TRUE);
}

void MainWindow::PopulateVirtualItem(LPARAM payload) {
    auto* info = reinterpret_cast<NMLVDISPINFOW*>(payload);
    const int row = info->item.iItem;
    if (row < 0 || static_cast<size_t>(row) >= results_.size()) {
        return;
    }

    const auto& item = results_[static_cast<size_t>(row)];
    if ((info->item.mask & LVIF_TEXT) != 0 && info->item.pszText != nullptr) {
        const wchar_t* value = L"";
        switch (info->item.iSubItem) {
        case 0:
            value = item.name.c_str();
            break;
        case 1:
            value = item.path.c_str();
            break;
        case 2:
            value = item.type.c_str();
            break;
        default:
            break;
        }
        StringCchCopyW(info->item.pszText, info->item.cchTextMax, value);
    }

    if ((info->item.mask & LVIF_IMAGE) != 0) {
        info->item.iImage = ImageIndexForResult(item);
    }
}

int MainWindow::SelectedRow() const {
    if (listView_ == nullptr) {
        return -1;
    }
    return ListView_GetNextItem(listView_, -1, LVNI_SELECTED);
}

int MainWindow::RowFromScreenPoint(POINT screenPoint) const {
    if (listView_ == nullptr) {
        return -1;
    }

    POINT clientPoint = screenPoint;
    ScreenToClient(listView_, &clientPoint);

    LVHITTESTINFO hit = {};
    hit.pt = clientPoint;
    const int row = ListView_SubItemHitTest(listView_, &hit);
    if (row < 0 || (hit.flags & LVHT_ONITEM) == 0) {
        return -1;
    }
    return row;
}

POINT MainWindow::ContextMenuPointForRow(int row) const {
    POINT point = {};
    if (listView_ == nullptr || row < 0) {
        return point;
    }

    RECT itemRect = {};
    itemRect.left = LVIR_BOUNDS;
    if (ListView_GetItemRect(listView_, row, &itemRect, LVIR_BOUNDS)) {
        point.x = itemRect.left + 24;
        point.y = itemRect.top + ((itemRect.bottom - itemRect.top) / 2);
        ClientToScreen(listView_, &point);
    }
    return point;
}

void MainWindow::SelectOnlyRow(int row) const {
    if (listView_ == nullptr || row < 0) {
        return;
    }

    ListView_SetItemState(listView_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(listView_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(listView_, row, FALSE);
}

bool MainWindow::OpenResult(const SearchResultRow& row) const {
    return ShellExecuteAsUser(hwnd_, row.path.c_str(), nullptr);
}

bool MainWindow::OpenContainingFolder(const SearchResultRow& row) const {
    if (row.isDirectory) {
        return OpenResult(row);
    }

    std::wstring parameters = L"/select,\"";
    parameters += row.path;
    parameters += L"\"";

    return ShellExecuteAsUser(hwnd_, L"explorer.exe", parameters.c_str());
}

int MainWindow::ImageIndexForResult(const SearchResultRow& row) {
    const std::wstring cacheKey = IconCacheKeyForResult(row);
    const auto found = iconIndexCache_.find(cacheKey);
    if (found != iconIndexCache_.end()) {
        return found->second;
    }

    const int imageIndex = QuerySystemImageIndex(row);
    iconIndexCache_.emplace(cacheKey, imageIndex);
    return imageIndex;
}

int MainWindow::QuerySystemImageIndex(const SearchResultRow& row) const {
    SHFILEINFOW fileInfo = {};
    DWORD_PTR result = 0;

    if (row.isDirectory) {
        result = SHGetFileInfoW(
            row.path.c_str(),
            FILE_ATTRIBUTE_DIRECTORY,
            &fileInfo,
            sizeof(fileInfo),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (result == 0) {
            result = SHGetFileInfoW(
                L"folder",
                FILE_ATTRIBUTE_DIRECTORY,
                &fileInfo,
                sizeof(fileInfo),
                SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        }
    } else {
        const std::wstring extension = LowerExtension(row.path);
        if (HasOwnIcon(extension)) {
            result = SHGetFileInfoW(
                row.path.c_str(),
                FILE_ATTRIBUTE_NORMAL,
                &fileInfo,
                sizeof(fileInfo),
                SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        }

        if (result == 0 && !extension.empty()) {
            result = SHGetFileInfoW(
                extension.c_str(),
                FILE_ATTRIBUTE_NORMAL,
                &fileInfo,
                sizeof(fileInfo),
                SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        }

        if (result == 0) {
            result = SHGetFileInfoW(
                L"*",
                FILE_ATTRIBUTE_NORMAL,
                &fileInfo,
                sizeof(fileInfo),
                SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        }
    }

    return result != 0 ? fileInfo.iIcon : 0;
}

std::wstring MainWindow::IconCacheKeyForResult(const SearchResultRow& row) const {
    if (row.isDirectory) {
        return L"<folder>";
    }

    const std::wstring extension = LowerExtension(row.path);
    if (HasOwnIcon(extension)) {
        std::wstring loweredPath = row.path;
        CharLowerBuffW(loweredPath.data(), static_cast<DWORD>(loweredPath.size()));
        return L"path:" + loweredPath;
    }

    if (!extension.empty()) {
        return L"ext:" + extension;
    }

    return L"<file>";
}

void MainWindow::ShowResultContextMenu(POINT screenPoint) {
    if (listView_ == nullptr || results_.empty()) {
        return;
    }

    int row = -1;
    if (screenPoint.x == -1 && screenPoint.y == -1) {
        row = SelectedRow();
        if (row < 0) {
            return;
        }
        screenPoint = ContextMenuPointForRow(row);
    } else {
        row = RowFromScreenPoint(screenPoint);
        if (row < 0) {
            return;
        }
        SelectOnlyRow(row);
    }

    if (static_cast<size_t>(row) >= results_.size()) {
        return;
    }

    const auto& rowData = results_[static_cast<size_t>(row)];

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    InsertMenuW(menu, 0, MF_BYPOSITION | MF_STRING, kContextCommandOpen, L"Open");
    InsertMenuW(menu, 1, MF_BYPOSITION | MF_STRING, kContextCommandOpenLocation, rowData.isDirectory ? L"Open Folder" : L"Open File Location");

    PIDLIST_ABSOLUTE absolutePidl = nullptr;
    IContextMenu* contextMenu = nullptr;
    const HRESULT parseResult = SHParseDisplayName(rowData.path.c_str(), nullptr, &absolutePidl, 0, nullptr);
    if (SUCCEEDED(parseResult) && absolutePidl != nullptr) {
        IShellFolder* parentFolder = nullptr;
        PCUITEMID_CHILD child = nullptr;
        const HRESULT bindResult = SHBindToParent(absolutePidl, IID_PPV_ARGS(&parentFolder), &child);
        if (SUCCEEDED(bindResult) && parentFolder != nullptr && child != nullptr) {
            PCUITEMID_CHILD childArray[1] = { child };
            const HRESULT menuResult =
                parentFolder->GetUIObjectOf(hwnd_, 1, childArray, IID_IContextMenu, nullptr, reinterpret_cast<void**>(&contextMenu));
            if (FAILED(menuResult)) {
                contextMenu = nullptr;
            }
            parentFolder->Release();
        }
    }

    if (contextMenu != nullptr) {
        InsertMenuW(menu, 2, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        contextMenu->QueryContextMenu(menu, 3, kShellCommandBase, 0x7FFF, CMF_NORMAL);
    }

    const UINT selectedCommand = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        hwnd_,
        nullptr);

    if (selectedCommand == kContextCommandOpen) {
        OpenResult(rowData);
    } else if (selectedCommand == kContextCommandOpenLocation) {
        OpenContainingFolder(rowData);
    } else if (contextMenu != nullptr && selectedCommand >= kShellCommandBase) {
        CMINVOKECOMMANDINFOEX invoke = {};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = hwnd_;
        invoke.lpVerb = MAKEINTRESOURCEA(selectedCommand - kShellCommandBase);
        invoke.lpVerbW = MAKEINTRESOURCEW(selectedCommand - kShellCommandBase);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
    }

    DestroyMenu(menu);
    if (contextMenu != nullptr) {
        contextMenu->Release();
    }
    if (absolutePidl != nullptr) {
        CoTaskMemFree(absolutePidl);
    }
}

LRESULT MainWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        CreateControls();
        return 0;

    case WM_SIZE:
        ResizeChildren();
        return 0;

    case WM_SETFOCUS:
        SetFocus(searchEdit_);
        return 0;

    case WM_TIMER:
        if (wParam == kSearchDebounceTimerId) {
            KillTimer(hwnd_, kSearchDebounceTimerId);
            QueueSearch();
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == 101 && HIWORD(wParam) == EN_CHANGE) {
            SetTimer(hwnd_, kSearchDebounceTimerId, kSearchDebounceMs, nullptr);
            return 0;
        }
        if (LOWORD(wParam) == kDonateButtonId && HIWORD(wParam) == BN_CLICKED) {
            OpenExternalUrl(kDonateUrl);
            return 0;
        }
        if (LOWORD(wParam) == kHomepageButtonId && HIWORD(wParam) == BN_CLICKED) {
            OpenExternalUrl(kHomepageUrl);
            return 0;
        }
        return 0;

    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == listView_) {
            POINT menuPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ShowResultContextMenu(menuPoint);
            return 0;
        }
        break;

    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->idFrom == 102 && header->code == LVN_GETDISPINFOW) {
            PopulateVirtualItem(lParam);
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 760;
        info->ptMinTrackSize.y = 480;
        return 0;
    }

    case kMessageIndexReady:
        SetWindowTextW(hwnd_, kAppTitle);
        QueueSearch();
        return 0;

    case kMessageSearchComplete: {
        std::unique_ptr<SearchPayload> payload(reinterpret_cast<SearchPayload*>(lParam));
        if (payload != nullptr && payload->requestId == latestRequestedSearchId_.load(std::memory_order_relaxed)) {
            ApplyResults(std::move(payload->rows));
        }
        return 0;
    }

    case WM_DESTROY:
        shutdown_.store(true, std::memory_order_relaxed);
        {
            std::lock_guard lock(searchMutex_);
            searchRequested_ = true;
        }
        searchCondition_.notify_all();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace chatda
