#pragma once

#include <windows.h>

#include <atomic>
#include <string>

namespace chatda {

// Worker thread: returns the newer published version, or empty when up to date, offline or cancelled.
std::wstring FindNewerVersion(const std::atomic_bool& cancel);
// UI thread: asks the user whether to install the given version.
bool ConfirmUpdate(HWND owner, const std::wstring& version);
// Worker thread: downloads and launches the installer. Returns an error message, or empty on success
// or when cancelled (then nothing is started).
std::wstring DownloadAndStartInstaller(const std::wstring& version, const std::atomic_bool& cancel);
// UI thread.
void ShowUpdateError(HWND owner, const std::wstring& error);

}  // namespace chatda
