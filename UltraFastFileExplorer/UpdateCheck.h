#pragma once

#include <windows.h>

#include <atomic>

namespace ufe {

void CheckForUpdatesOnce(HWND owner, const std::atomic_bool& shutdownFlag);
constexpr wchar_t kDonateUrl[] = L"https://www.paypal.com/ncp/payment/T2KAST2XQC6BS";
constexpr wchar_t kHomepageUrl[] = L"https://tech.trendarchive.io/";

}  // namespace ufe
