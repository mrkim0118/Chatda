#pragma once

#include <algorithm>
#include <cwctype>
#include <string>

namespace chatda {

inline std::wstring Trim(const std::wstring& value) {
    auto is_space = [](wchar_t ch) { return iswspace(ch) != 0; };
    const auto start = std::find_if_not(value.begin(), value.end(), is_space);
    const auto end = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    if (start >= end) {
        return {};
    }
    return std::wstring(start, end);
}

inline std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == L'\\') {
        return left + right;
    }
    return left + L'\\' + right;
}

}  // namespace chatda
