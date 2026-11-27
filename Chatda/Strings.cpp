#include "Strings.h"

#include <windows.h>

#include <cstdarg>
#include <cwchar>

namespace chatda {
namespace {

constexpr UiText kEnglish = {
    L"Chatda - Indexing...",
    L"Search file or folder name",
    L"Name",
    L"Path",
    L"Type",
    L"Size",
    L"Date modified",
    L"Folder",
    L"File",
    L"Open",
    L"Open Folder",
    L"Open File Location",
    L"Copy Path",
    L"Edit Exclusions...",
    L"Exit",
    L"Chatda is still running",
    L"Press %s or click the tray icon to open it again.",
    L"Indexing...",
    L"%s items · updating index...",
    L"%s items indexed",
    L"Could not index the drives",
    L"%s results",
    L"%s results (showing the first %s)",
    L"Update Available",
    L"New version: %s\nCurrent version: %s\n\nInstall now?",
    L"Update Failed",
    L"Could not download the installer.",
    L"The installer could not be started.",
    L"# Chatda exclusions\r\n"
    L"# One rule per line. Lines starting with # are ignored.\r\n"
    L"# A full path (C:\\Windows) hides that folder and everything inside it.\r\n"
    L"# A name without a backslash (node_modules) hides every item with that name and everything inside it.\r\n"
    L"# Changes apply to the next search.\r\n"
    L"$Recycle.Bin\r\n"
    L"System Volume Information\r\n",
};

constexpr UiText kKorean = {
    L"Chatda - 색인 중...",
    L"파일 또는 폴더 이름 검색",
    L"이름",
    L"경로",
    L"종류",
    L"크기",
    L"수정한 날짜",
    L"폴더",
    L"파일",
    L"열기",
    L"폴더 열기",
    L"파일 위치 열기",
    L"경로 복사",
    L"제외 목록 편집...",
    L"종료",
    L"Chatda가 계속 실행 중입니다",
    L"%s 또는 트레이 아이콘으로 다시 열 수 있습니다.",
    L"색인 중...",
    L"항목 %s개 · 색인 갱신 중...",
    L"항목 %s개 색인됨",
    L"드라이브를 색인하지 못했습니다",
    L"결과 %s개",
    L"결과 %s개 중 %s개 표시",
    L"업데이트",
    L"새 버전: %s\n현재 버전: %s\n\n지금 설치할까요?",
    L"업데이트 실패",
    L"설치 파일을 내려받지 못했습니다.",
    L"설치 프로그램을 실행하지 못했습니다.",
    L"# Chatda 제외 목록\r\n"
    L"# 한 줄에 하나씩 적습니다. #으로 시작하는 줄은 무시합니다.\r\n"
    L"# 전체 경로(C:\\Windows)를 적으면 그 폴더와 안의 모든 항목이 검색 결과에서 빠집니다.\r\n"
    L"# 백슬래시 없이 이름(node_modules)만 적으면 그 이름의 항목과 그 안의 모든 항목이 빠집니다.\r\n"
    L"# 저장하면 다음 검색부터 적용됩니다.\r\n"
    L"$Recycle.Bin\r\n"
    L"System Volume Information\r\n",
};

}  // namespace

const UiText& Text() {
    static const UiText& text = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN ? kKorean : kEnglish;
    return text;
}

std::wstring FormatText(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int length = _vscwprintf(format, copy);
    va_end(copy);
    std::wstring text(static_cast<size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) {
        _vsnwprintf_s(text.data(), text.size() + 1, _TRUNCATE, format, args);
    }
    va_end(args);
    return text;
}

std::wstring FormatCount(uint64_t value) {
    std::wstring digits = std::to_wstring(value);
    for (ptrdiff_t position = static_cast<ptrdiff_t>(digits.size()) - 3; position > 0; position -= 3) {
        digits.insert(static_cast<size_t>(position), 1, L',');
    }
    return digits;
}

}  // namespace chatda
