#include "MainWindow.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <windowsx.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlguid.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <utility>

#include "StringUtil.h"
#include "Strings.h"
#include "UpdateCheck.h"
#include "resource.h"

namespace chatda {
namespace {

// NTFS volumes follow their USN journal; others (and journals that stop working) are rebuilt periodically.
constexpr auto kJournalPollInterval = std::chrono::seconds(1);
constexpr auto kPeriodicRebuildInterval = std::chrono::minutes(10);
constexpr auto kJournalRetryDelay = std::chrono::minutes(1);

constexpr wchar_t kWindowClassName[] = L"ChatdaWindow";
constexpr wchar_t kActivateMessageName[] = L"Chatda.Activate";
constexpr wchar_t kAppTitle[] = L"Chatda";
constexpr wchar_t kHotkeyText[] = L"Ctrl+Alt+F";
constexpr int kHotkeyId = 1;

constexpr int kSearchEditId = 101;
constexpr int kListViewId = 102;
constexpr int kStatusBarId = 103;
constexpr UINT_PTR kSearchDebounceTimerId = 1001;
constexpr UINT kSearchDebounceMs = 55;

constexpr UINT kMessageIndexReady = WM_APP + 1;  // wParam: kIndex* state
constexpr UINT kMessageSearchComplete = WM_APP + 2;
constexpr UINT kMessageUpdateAvailable = WM_APP + 3;
constexpr UINT kMessageUpdateFailed = WM_APP + 4;
constexpr UINT kMessageIndexChanged = WM_APP + 5;
constexpr UINT kMessageExitApp = WM_APP + 6;
constexpr UINT kTrayMessage = WM_APP + 7;

constexpr int kIndexNone = 0;
constexpr int kIndexCached = 1;
constexpr int kIndexReady = 2;
constexpr int kIndexFailed = 3;

constexpr UINT kContextCommandOpen = 1;
constexpr UINT kContextCommandOpenLocation = 2;
constexpr UINT kContextCommandCopyPath = 3;
constexpr UINT kShellCommandBase = 100;
constexpr UINT kTrayCommandOpen = 1;
constexpr UINT kTrayCommandEditExclusions = 2;
constexpr UINT kTrayCommandExit = 3;
constexpr UINT kSysCommandEditExclusions = 0x0010;  // low four bits must stay zero

constexpr int kColumnName = 0;
constexpr int kColumnPath = 1;
constexpr int kColumnType = 2;
constexpr int kColumnSize = 3;
constexpr int kColumnModified = 4;
constexpr int kColumnCount = 5;

UINT ActivateMessage() {
    static const UINT message = RegisterWindowMessageW(kActivateMessageName);
    return message;
}

void PostString(HWND hwnd, UINT message, std::wstring value) {
    auto* payload = new std::wstring(std::move(value));
    if (!PostMessageW(hwnd, message, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

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

std::wstring LongPath(const std::wstring& path) {
    return path.size() >= MAX_PATH && path.rfind(L"\\\\", 0) != 0 ? L"\\\\?\\" + path : path;
}

std::wstring FormatFileTime(uint64_t value) {
    FILETIME fileTime = {static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)};
    SYSTEMTIME utc = {};
    SYSTEMTIME local = {};
    if (!FileTimeToSystemTime(&fileTime, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return {};
    }
    wchar_t text[32] = {};
    StringCchPrintfW(text, std::size(text), L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    return text;
}

// Natural order, like Explorer: "file2" before "file10".
int CompareText(const std::wstring& left, const std::wstring& right) {
    return CompareStringEx(
               LOCALE_NAME_USER_DEFAULT,
               NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
               left.c_str(),
               static_cast<int>(left.size()),
               right.c_str(),
               static_cast<int>(right.size()),
               nullptr,
               nullptr,
               0) -
           CSTR_EQUAL;
}

}  // namespace

struct MainWindow::SearchPayload {
    uint64_t requestId = 0;
    std::wstring query;
    SearchOutcome outcome;
};

bool MainWindow::ActivateRunningInstance() {
    HWND existing = FindWindowW(kWindowClassName, nullptr);
    if (existing == nullptr) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(existing, &processId);
    AllowSetForegroundWindow(processId);
    PostMessageW(existing, ActivateMessage(), 0, 0);
    return true;
}

int MainWindow::Run(HINSTANCE instance, int commandShow) {
    instance_ = instance;
    if (!RegisterWindowClass()) {
        return 0;
    }

    const UINT systemDpi = GetDpiForSystem();
    hwnd_ = CreateWindowExW(
        0,
        kWindowClassName,
        Text().indexingTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        MulDiv(1080, static_cast<int>(systemDpi), 96),
        MulDiv(720, static_cast<int>(systemDpi), 96),
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
    StartUpdateThread();

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (PreTranslate(message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    StopWorkers();
    ReleaseResources();
    return static_cast<int>(message.wParam);
}

bool MainWindow::PreTranslate(const MSG& message) {
    if (message.message != WM_KEYDOWN || hwnd_ == nullptr || (message.hwnd != hwnd_ && GetAncestor(message.hwnd, GA_ROOT) != hwnd_)) {
        return false;
    }
    if (GetKeyState(VK_MENU) < 0) {
        return false;
    }

    const bool control = GetKeyState(VK_CONTROL) < 0;
    const WPARAM key = message.wParam;
    if ((control && key == 'L') || key == VK_F3) {
        FocusSearch();
        return true;
    }

    if (message.hwnd == searchEdit_) {
        switch (key) {
        case VK_RETURN: {
            const int row = SelectedRow();
            OpenRow(row >= 0 ? row : 0, control);
            return true;
        }
        case VK_DOWN:
            if (!results_.empty()) {
                const int row = SelectedRow();
                SetFocus(listView_);
                SelectOnlyRow(row >= 0 ? row : 0);
            }
            return true;
        case VK_ESCAPE:
            if (GetWindowTextLengthW(searchEdit_) > 0) {
                SetWindowTextW(searchEdit_, L"");
            } else {
                HideToTray();
            }
            return true;
        case 'A':
            if (control) {
                SendMessageW(searchEdit_, EM_SETSEL, 0, -1);
                return true;
            }
            break;
        }
        return false;
    }

    if (message.hwnd == listView_) {
        switch (key) {
        case VK_RETURN:
            OpenRow(SelectedRow(), control);
            return true;
        case VK_ESCAPE:
            FocusSearch();
            return true;
        case VK_UP:
            if (ListView_GetNextItem(listView_, -1, LVNI_FOCUSED) <= 0) {
                FocusSearch();
                return true;
            }
            break;
        case 'C':
            if (control) {
                CopySelectedPaths();
                return true;
            }
            break;
        case 'A':
            if (control) {
                ListView_SetItemState(listView_, -1, LVIS_SELECTED, LVIS_SELECTED);
                return true;
            }
            break;
        }
    }
    return false;
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

int MainWindow::Scale(int value) const {
    return MulDiv(value, static_cast<int>(dpi_), 96);
}

void MainWindow::CreateControls() {
    dpi_ = GetDpiForWindow(hwnd_);

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
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchEditId)),
        instance_,
        nullptr);

    SetWindowSubclass(searchEdit_, SearchEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(searchEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(Text().searchCue));

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
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListViewId)),
        instance_,
        nullptr);

    statusBar_ = CreateWindowExW(
        0,
        STATUSCLASSNAMEW,
        nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0,
        0,
        0,
        0,
        hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusBarId)),
        instance_,
        nullptr);

    ListView_SetUnicodeFormat(listView_, TRUE);
    ListView_SetExtendedListViewStyle(listView_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    InitializeFonts();
    InitializeSystemImageList();
    InsertColumns();
    ResizeChildren();
    UpdateStatus();
    SetFocus(searchEdit_);

    // Edit Exclusions also lives in the window menu (Alt+Space), for when the tray icon is hidden.
    HMENU systemMenu = GetSystemMenu(hwnd_, FALSE);
    if (systemMenu != nullptr) {
        AppendMenuW(systemMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(systemMenu, MF_STRING, kSysCommandEditExclusions, Text().menuEditExclusions);
    }

    // Chatda runs as admin: let Explorer's (non-admin) tray messages through.
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd_, kTrayMessage, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreatedMessage_, MSGFLT_ALLOW, nullptr);

    hotkeyRegistered_ = RegisterHotKey(hwnd_, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F') != FALSE;
    AddTrayIcon();
}

void MainWindow::InsertColumns() {
    struct Column {
        const wchar_t* title;
        int format;
    };
    const Column columns[kColumnCount] = {
        {Text().columnName, LVCFMT_LEFT},
        {Text().columnPath, LVCFMT_LEFT},
        {Text().columnType, LVCFMT_LEFT},
        {Text().columnSize, LVCFMT_RIGHT},
        {Text().columnModified, LVCFMT_LEFT},
    };

    for (int index = 0; index < kColumnCount; ++index) {
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        column.fmt = columns[index].format;
        column.pszText = const_cast<LPWSTR>(columns[index].title);
        column.cx = Scale(100);
        column.iSubItem = index;
        ListView_InsertColumn(listView_, index, &column);
    }
}

void MainWindow::ResizeChildren() {
    if (hwnd_ == nullptr || searchEdit_ == nullptr || listView_ == nullptr || statusBar_ == nullptr) {
        return;
    }

    SendMessageW(statusBar_, WM_SIZE, 0, 0);
    RECT statusRect = {};
    GetWindowRect(statusBar_, &statusRect);
    const int statusHeight = statusRect.bottom - statusRect.top;

    RECT rect = {};
    GetClientRect(hwnd_, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int margin = Scale(12);
    const int listTop = margin + searchControlHeight_ + Scale(10);
    const int listWidth = std::max(0, width - (margin * 2));

    MoveWindow(searchEdit_, margin, margin, listWidth, searchControlHeight_, TRUE);
    MoveWindow(listView_, margin, listTop, listWidth, std::max(0, height - listTop - statusHeight - Scale(6)), TRUE);

    int parts[2] = {std::max(Scale(200), width * 55 / 100), -1};
    SendMessageW(statusBar_, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));

    const int usable = listWidth - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_) - Scale(4);
    const int typeWidth = Scale(70);
    const int sizeWidth = Scale(90);
    const int dateWidth = Scale(130);
    const int nameWidth = std::max(Scale(200), usable / 4);
    ListView_SetColumnWidth(listView_, kColumnName, nameWidth);
    ListView_SetColumnWidth(listView_, kColumnPath, std::max(Scale(200), usable - nameWidth - typeWidth - sizeWidth - dateWidth));
    ListView_SetColumnWidth(listView_, kColumnType, typeWidth);
    ListView_SetColumnWidth(listView_, kColumnSize, sizeWidth);
    ListView_SetColumnWidth(listView_, kColumnModified, dateWidth);
    UpdateSearchEditLayout();
}

void MainWindow::InitializeFonts() {
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
        return;
    }

    LOGFONTW listFontSpec = metrics.lfMessageFont;
    listFontSpec.lfHeight = -MulDiv(9, static_cast<int>(dpi_), 72);

    LOGFONTW searchFontSpec = metrics.lfMessageFont;
    searchFontSpec.lfHeight = -MulDiv(12, static_cast<int>(dpi_), 72);

    HFONT oldListFont = listFont_;
    HFONT oldSearchFont = searchFont_;
    listFont_ = CreateFontIndirectW(&listFontSpec);
    searchFont_ = CreateFontIndirectW(&searchFontSpec);

    if (listFont_ != nullptr) {
        SendMessageW(listView_, WM_SETFONT, reinterpret_cast<WPARAM>(listFont_), TRUE);
        SendMessageW(statusBar_, WM_SETFONT, reinterpret_cast<WPARAM>(listFont_), TRUE);
    }
    if (searchFont_ != nullptr) {
        SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(searchFont_), TRUE);
    }
    // Delete only after the controls stopped using them.
    if (oldListFont != nullptr) {
        DeleteObject(oldListFont);
    }
    if (oldSearchFont != nullptr) {
        DeleteObject(oldSearchFont);
    }

    SendMessageW(searchEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Scale(10), Scale(10)));

    searchTextHeight_ = Scale(18);
    searchControlHeight_ = Scale(34);
    HDC dc = GetDC(searchEdit_);
    if (dc != nullptr) {
        HFONT previousFont = searchFont_ != nullptr ? reinterpret_cast<HFONT>(SelectObject(dc, searchFont_)) : nullptr;
        TEXTMETRICW metricsInfo = {};
        if (GetTextMetricsW(dc, &metricsInfo)) {
            searchTextHeight_ = static_cast<int>(metricsInfo.tmHeight + metricsInfo.tmExternalLeading);
            searchControlHeight_ = std::max(Scale(34), searchTextHeight_ + Scale(18));
        }
        if (previousFont != nullptr) {
            SelectObject(dc, previousFont);
        }
        ReleaseDC(searchEdit_, dc);
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

    const int padding = Scale(10);
    const int clientHeight = clientRect.bottom - clientRect.top;
    const int verticalPadding = std::max(2, (clientHeight - searchTextHeight_) / 2);
    RECT textRect = {
        padding,
        verticalPadding - 1,
        std::max(static_cast<LONG>(padding + 1), clientRect.right - padding),
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
    if (trayIcon_ != nullptr) {
        DestroyIcon(trayIcon_);
        trayIcon_ = nullptr;
    }
}

void MainWindow::StartWorkers() {
    shutdown_.store(false, std::memory_order_relaxed);
    searchThread_ = std::thread([this]() { SearchLoop(); });
    indexThread_ = std::thread([this]() { IndexLoop(); });
}

// Asks once per launch when GitHub has a newer release; the answer arrives as kMessageUpdateAvailable.
void MainWindow::StartUpdateThread() {
    updateThread_ = std::thread([hwnd = hwnd_, cancel = updateCancel_]() {
        std::wstring version = FindNewerVersion(*cancel);
        if (!version.empty() && !cancel->load(std::memory_order_relaxed)) {
            PostString(hwnd, kMessageUpdateAvailable, std::move(version));
        }
    });
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

    // The update thread can sit in a network call for many seconds. It only owns copies and the
    // shared cancel flag, and starts nothing once cancelled, so let it finish on its own.
    updateCancel_->store(true, std::memory_order_relaxed);
    if (updateThread_.joinable()) {
        updateThread_.detach();
    }
    if (indexThread_.joinable()) {
        indexThread_.join();
    }
    if (searchThread_.joinable()) {
        searchThread_.join();
    }
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
        PostMessageW(hwnd_, kMessageIndexReady, kIndexCached, 0);
    }
    cachedIndex.reset();

    for (bool firstBuild = true;; firstBuild = false) {
        auto index = std::make_shared<SearchIndex>();
        const bool built = index->Build(shutdown_);
        if (shutdown_.load(std::memory_order_relaxed)) {
            return;
        }

        if (built) {
            if (firstBuild) {
                index->SaveCache();  // once per run: the cache only speeds up the next start
            }
            {
                std::lock_guard lock(indexMutex_);
                searchIndex_ = index;
            }
            hasIndex = true;
            PostMessageW(hwnd_, kMessageIndexReady, kIndexReady, 0);
        } else if (!hasIndex) {
            PostMessageW(hwnd_, kMessageIndexReady, kIndexFailed, 0);
        }

        // Follow the journals until a full rebuild is due.
        const auto builtAt = std::chrono::steady_clock::now();
        bool journalFailed = false;
        while (true) {
            {
                std::unique_lock lock(indexMutex_);
                if (indexCondition_.wait_for(lock, kJournalPollInterval, [&]() { return shutdown_.load(std::memory_order_relaxed); })) {
                    return;
                }
            }

            const auto elapsed = std::chrono::steady_clock::now() - builtAt;
            if (!built) {
                if (elapsed >= kPeriodicRebuildInterval) {
                    break;
                }
                continue;
            }
            if (index->DrivesChanged() || (journalFailed && elapsed >= kJournalRetryDelay) ||
                (index->NeedsPeriodicRebuild() && elapsed >= kPeriodicRebuildInterval)) {
                break;
            }

            const SearchIndex::PollResult poll = index->PollJournals();
            if (poll == SearchIndex::PollResult::Rebuild) {
                journalFailed = true;
            }
            if (poll != SearchIndex::PollResult::None) {
                PostMessageW(hwnd_, kMessageIndexChanged, 0, 0);
            }
        }
    }
}

void MainWindow::SearchLoop() {
    EnsureExcludeFile(Text().excludeFileDefault);
    ExcludeRules rules;
    uint64_t rulesStamp = UINT64_MAX;

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

        // exclude.txt is re-read whenever it was saved since the last search.
        const uint64_t stamp = ExcludeFileStamp();
        if (stamp != rulesStamp) {
            rules = LoadExcludeRules();
            rulesStamp = stamp;
        }

        auto payload = std::make_unique<SearchPayload>();
        payload->requestId = requestId;
        payload->query = query;

        std::shared_ptr<SearchIndex> index;
        {
            std::lock_guard lock(indexMutex_);
            index = searchIndex_;
        }
        if (index != nullptr) {
            payload->outcome = index->Search(query, requestId, latestRequestedSearchId_, rules);
        }

        if (shutdown_.load(std::memory_order_relaxed)) {
            break;
        }

        SearchPayload* raw = payload.release();
        if (!PostMessageW(hwnd_, kMessageSearchComplete, 0, reinterpret_cast<LPARAM>(raw))) {
            delete raw;
        }
    }
}

void MainWindow::ApplyResults(std::unique_ptr<SearchPayload> payload) {
    // The same query again means the index changed underneath: keep the selection and scroll position.
    const bool refresh = !resultsQuery_.empty() && payload->query == resultsQuery_;
    std::wstring keepPath;
    if (refresh) {
        const int focused = ListView_GetNextItem(listView_, -1, LVNI_FOCUSED | LVNI_SELECTED);
        if (focused >= 0 && static_cast<size_t>(focused) < results_.size()) {
            keepPath = results_[static_cast<size_t>(focused)].path;
        }
    }

    results_ = std::move(payload->outcome.rows);
    totalMatches_ = payload->outcome.totalMatches;
    resultsQuery_ = std::move(payload->query);
    for (size_t index = 0; index < results_.size(); ++index) {
        results_[index].relevance = static_cast<uint32_t>(index);
    }
    if (sortColumn_ >= 0) {
        SortResults();
    }

    ListView_SetItemState(listView_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(listView_, static_cast<int>(results_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    if (!keepPath.empty()) {
        const auto found = std::find_if(results_.begin(), results_.end(), [&](const SearchResultRow& row) { return row.path == keepPath; });
        if (found != results_.end()) {
            const int row = static_cast<int>(found - results_.begin());
            ListView_SetItemState(listView_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    } else if (!refresh && !results_.empty()) {
        ListView_EnsureVisible(listView_, 0, FALSE);
    }
    InvalidateRect(listView_, nullptr, TRUE);
    UpdateStatus();
}

void MainWindow::EnsureFileInfo(SearchResultRow& row) const {
    if (row.infoLoaded) {
        return;
    }
    row.infoLoaded = true;

    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (GetFileAttributesExW(LongPath(row.path).c_str(), GetFileExInfoStandard, &data)) {
        row.hasInfo = true;
        row.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        row.modified = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    }
}

void MainWindow::SortResults() {
    if (sortColumn_ == kColumnSize || sortColumn_ == kColumnModified) {
        for (auto& row : results_) {
            EnsureFileInfo(row);  // at most 2,000 rows
        }
    }

    std::sort(results_.begin(), results_.end(), [&](const SearchResultRow& left, const SearchResultRow& right) {
        int order = 0;
        switch (sortColumn_) {
        case kColumnName:
            order = CompareText(left.name, right.name);
            break;
        case kColumnPath:
            order = CompareText(left.path, right.path);
            break;
        case kColumnType:
            order = left.isDirectory != right.isDirectory ? (left.isDirectory ? -1 : 1) : CompareText(left.name, right.name);
            break;
        case kColumnSize:
            if (left.isDirectory != right.isDirectory) {
                order = left.isDirectory ? -1 : 1;
            } else {
                order = left.size < right.size ? -1 : (left.size > right.size ? 1 : 0);
            }
            break;
        case kColumnModified:
            order = left.modified < right.modified ? -1 : (left.modified > right.modified ? 1 : 0);
            break;
        default:
            break;
        }
        if (!sortAscending_) {
            order = -order;
        }
        return order != 0 ? order < 0 : left.relevance < right.relevance;
    });
}

void MainWindow::UpdateSortArrows() const {
    HWND header = ListView_GetHeader(listView_);
    for (int column = 0; column < kColumnCount; ++column) {
        HDITEMW item = {};
        item.mask = HDI_FORMAT;
        if (!Header_GetItem(header, column, &item)) {
            continue;
        }
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (column == sortColumn_) {
            item.fmt |= sortAscending_ ? HDF_SORTUP : HDF_SORTDOWN;
        }
        Header_SetItem(header, column, &item);
    }
}

// Click cycles: ascending -> descending -> relevance. Size and date start with the largest/newest.
void MainWindow::OnColumnClick(int column) {
    if (column == sortColumn_) {
        if (sortAscending_ == (column < kColumnSize)) {
            sortAscending_ = !sortAscending_;
        } else {
            sortColumn_ = -1;
        }
    } else {
        sortColumn_ = column;
        sortAscending_ = column < kColumnSize;
    }

    const int selected = SelectedRow();
    const std::wstring keepPath = selected >= 0 && static_cast<size_t>(selected) < results_.size() ? results_[static_cast<size_t>(selected)].path : std::wstring{};
    if (sortColumn_ >= 0) {
        SortResults();
    } else {
        std::sort(results_.begin(), results_.end(), [](const SearchResultRow& left, const SearchResultRow& right) { return left.relevance < right.relevance; });
    }
    UpdateSortArrows();

    if (!keepPath.empty()) {
        const auto found = std::find_if(results_.begin(), results_.end(), [&](const SearchResultRow& row) { return row.path == keepPath; });
        if (found != results_.end()) {
            SelectOnlyRow(static_cast<int>(found - results_.begin()));
        }
    }
    InvalidateRect(listView_, nullptr, TRUE);
}

void MainWindow::UpdateStatus() {
    if (statusBar_ == nullptr) {
        return;
    }

    std::wstring left;
    if (!Trim(resultsQuery_).empty()) {
        left = totalMatches_ > results_.size()
                   ? FormatText(Text().statusResultsCapped, FormatCount(totalMatches_).c_str(), FormatCount(results_.size()).c_str())
                   : FormatText(Text().statusResults, FormatCount(results_.size()).c_str());
    }

    size_t indexed = 0;
    {
        std::lock_guard lock(indexMutex_);
        if (searchIndex_ != nullptr) {
            indexed = searchIndex_->Size();
        }
    }
    std::wstring right;
    switch (indexState_) {
    case kIndexCached:
        right = FormatText(Text().statusRefreshing, FormatCount(indexed).c_str());
        break;
    case kIndexReady:
        right = FormatText(Text().statusIndexed, FormatCount(indexed).c_str());
        break;
    case kIndexFailed:
        right = Text().statusIndexFailed;
        break;
    default:
        right = Text().statusIndexing;
        break;
    }

    SendMessageW(statusBar_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(left.c_str()));
    SendMessageW(statusBar_, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(right.c_str()));
}

void MainWindow::PopulateVirtualItem(LPARAM payload) {
    auto* info = reinterpret_cast<NMLVDISPINFOW*>(payload);
    const int row = info->item.iItem;
    if (row < 0 || static_cast<size_t>(row) >= results_.size()) {
        return;
    }

    auto& item = results_[static_cast<size_t>(row)];
    if ((info->item.mask & LVIF_TEXT) != 0 && info->item.pszText != nullptr) {
        std::wstring text;
        switch (info->item.iSubItem) {
        case kColumnName:
            text = item.name;
            break;
        case kColumnPath:
            text = item.path;
            break;
        case kColumnType:
            text = item.isDirectory ? Text().typeFolder : Text().typeFile;
            break;
        case kColumnSize:
            if (!item.isDirectory) {
                EnsureFileInfo(item);
                wchar_t size[64] = {};
                if (item.hasInfo && StrFormatKBSizeW(static_cast<LONGLONG>(item.size), size, static_cast<UINT>(std::size(size))) != nullptr) {
                    text = size;
                }
            }
            break;
        case kColumnModified:
            EnsureFileInfo(item);
            if (item.hasInfo) {
                text = FormatFileTime(item.modified);
            }
            break;
        default:
            break;
        }
        StringCchCopyW(info->item.pszText, info->item.cchTextMax, text.c_str());
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
        point.x = itemRect.left + Scale(24);
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

void MainWindow::OpenRow(int row, bool containingFolder) const {
    if (row < 0 || static_cast<size_t>(row) >= results_.size()) {
        return;
    }
    const auto& item = results_[static_cast<size_t>(row)];
    if (containingFolder) {
        OpenContainingFolder(item);
    } else {
        OpenResult(item);
    }
}

void MainWindow::CopySelectedPaths() const {
    std::wstring text;
    for (int row = ListView_GetNextItem(listView_, -1, LVNI_SELECTED); row >= 0; row = ListView_GetNextItem(listView_, row, LVNI_SELECTED)) {
        if (static_cast<size_t>(row) < results_.size()) {
            if (!text.empty()) {
                text += L"\r\n";
            }
            text += results_[static_cast<size_t>(row)].path;
        }
    }
    if (text.empty() || !OpenClipboard(hwnd_)) {
        return;
    }

    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        void* target = GlobalLock(memory);
        if (target != nullptr) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
                GlobalFree(memory);
            }
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void MainWindow::FocusSearch() const {
    SetFocus(searchEdit_);
    SendMessageW(searchEdit_, EM_SETSEL, 0, -1);
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
        result = SHGetFileInfoW(row.path.c_str(), FILE_ATTRIBUTE_DIRECTORY, &fileInfo, sizeof(fileInfo), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (result == 0) {
            result = SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &fileInfo, sizeof(fileInfo), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        }
    } else {
        const std::wstring extension = LowerExtension(row.path);
        if (HasOwnIcon(extension)) {
            result = SHGetFileInfoW(row.path.c_str(), FILE_ATTRIBUTE_NORMAL, &fileInfo, sizeof(fileInfo), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        }
        if (result == 0 && !extension.empty()) {
            result = SHGetFileInfoW(extension.c_str(), FILE_ATTRIBUTE_NORMAL, &fileInfo, sizeof(fileInfo), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        }
        if (result == 0) {
            result = SHGetFileInfoW(L"*", FILE_ATTRIBUTE_NORMAL, &fileInfo, sizeof(fileInfo), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
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

    const SearchResultRow rowData = results_[static_cast<size_t>(row)];  // a copy: results_ may be refreshed while the menu is open

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    InsertMenuW(menu, 0, MF_BYPOSITION | MF_STRING, kContextCommandOpen, Text().menuOpen);
    InsertMenuW(menu, 1, MF_BYPOSITION | MF_STRING, kContextCommandOpenLocation, rowData.isDirectory ? Text().menuOpenFolder : Text().menuOpenLocation);
    InsertMenuW(menu, 2, MF_BYPOSITION | MF_STRING, kContextCommandCopyPath, Text().menuCopyPath);
    SetMenuDefaultItem(menu, kContextCommandOpen, FALSE);

    PIDLIST_ABSOLUTE absolutePidl = nullptr;
    IContextMenu* contextMenu = nullptr;
    const HRESULT parseResult = SHParseDisplayName(rowData.path.c_str(), nullptr, &absolutePidl, 0, nullptr);
    if (SUCCEEDED(parseResult) && absolutePidl != nullptr) {
        IShellFolder* parentFolder = nullptr;
        PCUITEMID_CHILD child = nullptr;
        const HRESULT bindResult = SHBindToParent(absolutePidl, IID_PPV_ARGS(&parentFolder), &child);
        if (SUCCEEDED(bindResult) && parentFolder != nullptr && child != nullptr) {
            PCUITEMID_CHILD childArray[1] = {child};
            const HRESULT menuResult = parentFolder->GetUIObjectOf(hwnd_, 1, childArray, IID_IContextMenu, nullptr, reinterpret_cast<void**>(&contextMenu));
            if (FAILED(menuResult)) {
                contextMenu = nullptr;
            }
            parentFolder->Release();
        }
    }

    if (contextMenu != nullptr) {
        InsertMenuW(menu, 3, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        contextMenu->QueryContextMenu(menu, 4, kShellCommandBase, 0x7FFF, CMF_NORMAL);
    }

    const UINT selectedCommand = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);

    if (selectedCommand == kContextCommandOpen) {
        OpenResult(rowData);
    } else if (selectedCommand == kContextCommandOpenLocation) {
        OpenContainingFolder(rowData);
    } else if (selectedCommand == kContextCommandCopyPath) {
        CopySelectedPaths();
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

void MainWindow::AddTrayIcon() {
    if (trayIcon_ == nullptr) {
        LoadIconMetric(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), LIM_SMALL, &trayIcon_);
    }

    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = trayIcon_;
    StringCchPrintfW(data.szTip, std::size(data.szTip), hotkeyRegistered_ ? L"Chatda (%s)" : L"Chatda", kHotkeyText);
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (trayAdded_) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
}

void MainWindow::RemoveTrayIcon() {
    if (!trayAdded_) {
        return;
    }
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayAdded_ = false;
}

void MainWindow::ShowTrayMenu(POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, kTrayCommandOpen, Text().menuOpen);
    AppendMenuW(menu, MF_STRING, kTrayCommandEditExclusions, Text().menuEditExclusions);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayCommandExit, Text().menuExit);
    SetMenuDefaultItem(menu, kTrayCommandOpen, FALSE);

    // Without this the menu does not close when clicking elsewhere.
    SetForegroundWindow(hwnd_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (command) {
    case kTrayCommandOpen:
        ShowAndFocus();
        break;
    case kTrayCommandEditExclusions:
        EditExclusions();
        break;
    case kTrayCommandExit:
        ExitApp();
        break;
    default:
        break;
    }
}

void MainWindow::ShowAndFocus() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
    FocusSearch();
}

void MainWindow::HideToTray() {
    if (!trayAdded_) {
        ShowWindow(hwnd_, SW_MINIMIZE);  // nowhere to bring it back from
        return;
    }

    ShowWindow(hwnd_, SW_HIDE);
    if (!trayHintShown_ && hotkeyRegistered_) {
        trayHintShown_ = true;
        NOTIFYICONDATAW data = {};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = 1;
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        StringCchCopyW(data.szInfoTitle, std::size(data.szInfoTitle), Text().trayHiddenTitle);
        StringCchCopyW(data.szInfo, std::size(data.szInfo), FormatText(Text().trayHiddenBody, kHotkeyText).c_str());
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
}

void MainWindow::ToggleFromHotkey() {
    if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_) && GetForegroundWindow() == hwnd_) {
        HideToTray();
    } else {
        ShowAndFocus();
    }
}

void MainWindow::EditExclusions() const {
    EnsureExcludeFile(Text().excludeFileDefault);
    const std::wstring path = ExcludeFilePath();
    if (!path.empty()) {
        ShellExecuteAsUser(hwnd_, path.c_str(), nullptr);  // the user's editor, without admin rights
    }
}

void MainWindow::ExitApp() {
    exiting_ = true;
    DestroyWindow(hwnd_);
}

LRESULT MainWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == ActivateMessage()) {
        ShowAndFocus();
        return 0;
    }
    if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
        trayAdded_ = false;  // Explorer restarted: the old icon is gone
        AddTrayIcon();
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        CreateControls();
        return 0;

    case WM_SIZE:
        ResizeChildren();
        return 0;

    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        InitializeFonts();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ResizeChildren();
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            HWND focus = GetFocus();
            if (focus == searchEdit_ || focus == listView_) {
                lastFocus_ = focus;
            }
        } else {
            SetFocus(lastFocus_ != nullptr ? lastFocus_ : searchEdit_);
            if (!Trim(CurrentQuery()).empty()) {
                QueueSearch();  // picks up index changes and exclude.txt edits made meanwhile
            }
        }
        return 0;

    case WM_SETFOCUS:
        SetFocus(lastFocus_ != nullptr ? lastFocus_ : searchEdit_);
        return 0;

    case WM_TIMER:
        if (wParam == kSearchDebounceTimerId) {
            KillTimer(hwnd_, kSearchDebounceTimerId);
            QueueSearch();
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == kSearchEditId && HIWORD(wParam) == EN_CHANGE) {
            SetTimer(hwnd_, kSearchDebounceTimerId, kSearchDebounceMs, nullptr);
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == kSysCommandEditExclusions) {
            EditExclusions();
            return 0;
        }
        break;

    case WM_HOTKEY:
        if (wParam == kHotkeyId) {
            ToggleFromHotkey();
        }
        return 0;

    case kTrayMessage:
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowAndFocus();
            break;
        case WM_CONTEXTMENU:
            ShowTrayMenu({GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)});
            break;
        default:
            break;
        }
        return 0;

    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == listView_) {
            POINT menuPoint = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ShowResultContextMenu(menuPoint);
            return 0;
        }
        break;

    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->hwndFrom == listView_) {
            switch (header->code) {
            case LVN_GETDISPINFOW:
                PopulateVirtualItem(lParam);
                break;
            case NM_DBLCLK: {
                const auto* activate = reinterpret_cast<const NMITEMACTIVATE*>(lParam);
                OpenRow(activate->iItem, (activate->uKeyFlags & LVKF_CONTROL) != 0);
                break;
            }
            case LVN_COLUMNCLICK:
                OnColumnClick(reinterpret_cast<const NMLISTVIEW*>(lParam)->iSubItem);
                break;
            default:
                break;
            }
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(760);
        info->ptMinTrackSize.y = Scale(480);
        return 0;
    }

    case kMessageIndexReady:
        indexState_ = static_cast<int>(wParam);
        SetWindowTextW(hwnd_, kAppTitle);
        UpdateStatus();
        QueueSearch();
        return 0;

    case kMessageIndexChanged:
        UpdateStatus();
        // Re-run the visible search so new and deleted files show up; skip it while hidden.
        if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_) && !Trim(CurrentQuery()).empty()) {
            QueueSearch();
        }
        return 0;

    case kMessageSearchComplete: {
        std::unique_ptr<SearchPayload> payload(reinterpret_cast<SearchPayload*>(lParam));
        if (payload != nullptr && payload->requestId == latestRequestedSearchId_.load(std::memory_order_relaxed)) {
            ApplyResults(std::move(payload));
        }
        return 0;
    }

    case kMessageUpdateAvailable: {
        const std::unique_ptr<std::wstring> version(reinterpret_cast<std::wstring*>(lParam));
        if (ConfirmUpdate(hwnd_, *version)) {
            if (updateThread_.joinable()) {
                updateThread_.join();  // the check thread posted this message as its last step
            }
            updateThread_ = std::thread([hwnd = hwnd_, cancel = updateCancel_, target = *version]() {
                std::wstring error = DownloadAndStartInstaller(target, *cancel);
                if (cancel->load(std::memory_order_relaxed)) {
                    return;
                }
                if (!error.empty()) {
                    PostString(hwnd, kMessageUpdateFailed, std::move(error));
                } else {
                    PostMessageW(hwnd, kMessageExitApp, 0, 0);  // the installer replaces Chatda.exe
                }
            });
        }
        return 0;
    }

    case kMessageUpdateFailed: {
        const std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lParam));
        ShowUpdateError(hwnd_, *error);
        return 0;
    }

    case kMessageExitApp:
        ExitApp();
        return 0;

    case WM_CLOSE:
        if (!exiting_ && trayAdded_) {
            HideToTray();  // keeps running in the tray; Exit is in the tray menu
            return 0;
        }
        break;

    case WM_ENDSESSION:
        if (wParam != FALSE) {
            ExitApp();  // sign-out, shutdown, or an installer closing Chatda
        }
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        if (hotkeyRegistered_) {
            UnregisterHotKey(hwnd_, kHotkeyId);
        }
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
