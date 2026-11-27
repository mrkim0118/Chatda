#pragma once

#include <cstdint>
#include <string>

namespace chatda {

// UI text in the Windows display language (Korean, otherwise English).
struct UiText {
    const wchar_t* indexingTitle;
    const wchar_t* searchCue;
    const wchar_t* columnName;
    const wchar_t* columnPath;
    const wchar_t* columnType;
    const wchar_t* columnSize;
    const wchar_t* columnModified;
    const wchar_t* typeFolder;
    const wchar_t* typeFile;
    const wchar_t* menuOpen;
    const wchar_t* menuOpenFolder;
    const wchar_t* menuOpenLocation;
    const wchar_t* menuCopyPath;
    const wchar_t* menuEditExclusions;
    const wchar_t* menuExit;
    const wchar_t* trayHiddenTitle;
    const wchar_t* trayHiddenBody;     // %s: hotkey
    const wchar_t* statusIndexing;
    const wchar_t* statusRefreshing;   // %s: file count
    const wchar_t* statusIndexed;      // %s: file count
    const wchar_t* statusIndexFailed;
    const wchar_t* statusResults;      // %s: count
    const wchar_t* statusResultsCapped;  // %s: total, %s: shown
    const wchar_t* updateTitle;
    const wchar_t* updateMessage;      // %s: new version, %s: current version
    const wchar_t* updateErrorTitle;
    const wchar_t* updateDownloadFailed;
    const wchar_t* updateStartFailed;
    const wchar_t* excludeFileDefault;
};

const UiText& Text();

// printf-style formatting with %s arguments.
std::wstring FormatText(const wchar_t* format, ...);
// 1234567 -> "1,234,567"
std::wstring FormatCount(uint64_t value);

}  // namespace chatda
