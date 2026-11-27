<p align="center">
  <img src="docs/icon.png" width="128" alt="Chatda 아이콘">
</p>

<h1 align="center">Chatda</h1>

<p align="center">네이티브 C++로 만든 Windows용 초고속 파일·폴더 이름 검색 프로그램입니다.</p>

<p align="center">Author: <b>Manggdy</b></p>

<p align="center"><a href="README.md">English</a> | <b>한국어</b></p>

Chatda는 로컬 드라이브의 모든 파일·폴더 이름을 색인해 두고, 입력하는 즉시 결과를 보여줍니다. NTFS 드라이브에서는 USN 저널을 직접 읽어서 모든 폴더를 훑는 것보다 훨씬 빠르게 색인을 만들고, 파일이 바뀌면 1초 안에 반영합니다.

## 다운로드

[Releases](https://github.com/mrkim0118/Chatda/releases/latest) 페이지에서 `Chatda-Setup-<버전>.exe`를 받아 실행하세요. 설치 폴더를 고를 수 있습니다(기본값 `C:\Program Files\Chatda`).

## 주요 기능

- **입력 즉시 검색**: Enter 없이 입력하는 동안 결과가 바로 바뀝니다.
- **여러 단어 검색**: 입력한 단어가 순서와 대소문자에 상관없이 모두 이름에 들어 있는 항목을 찾습니다.
- **검색 문법**: 와일드카드(`*.pdf`), 확장자 필터(`ext:pdf,docx`), 폴더만/파일만 보기, 경로 검색을 지원합니다.
- **초성 검색**: `ㅂㄱㅅ`으로 "보고서"를 찾습니다.
- **똑똑한 정렬**: 이름이 정확히 같은 항목, 첫 단어로 시작하는 항목, 나머지 순으로 보여주며 폴더가 파일보다 먼저 나옵니다.
- **열 정렬**: 이름, 경로, 종류, 크기, 수정한 날짜 열을 보여주고, 열 제목을 클릭하면 정렬합니다.
- **빠른 색인**: NTFS USN 저널을 읽고, NTFS가 아닌 드라이브는 폴더를 직접 훑어 색인합니다.
- **실시간 반영**: NTFS 드라이브에서는 새로 만들거나 이름을 바꾸거나 옮기거나 지운 파일이 1초 안에 반영됩니다.
- **모든 로컬 드라이브**: 고정 디스크, 이동식 디스크, RAM 디스크를 색인하고, 나중에 꽂은 드라이브도 색인합니다.
- **제외 목록**: `$Recycle.Bin`, `node_modules` 같은 폴더를 검색 결과에서 뺍니다.
- **색인 캐시**: 처음 만든 색인을 저장해 두어 다음 실행 때 바로 검색할 수 있습니다.
- **트레이 상주와 전역 단축키**: 창을 닫으면 트레이에서 계속 실행되고, 어디서든 `Ctrl+Alt+F`로 다시 띄웁니다.
- **결과에서 바로 열기**: 더블클릭하거나 Enter를 누르면 열리고, 오른쪽 클릭하면 탐색기 메뉴가 나옵니다.
- **시스템 아이콘**: 결과마다 Windows 파일 형식 아이콘을 보여줍니다.
- **한국어·영어 화면**: Windows 표시 언어를 따릅니다.
- **업데이트 확인**: 실행할 때 GitHub 최신 릴리스를 확인하고, 새 버전이 있으면 내려받아 설치할지 묻습니다.

## 요구 사항

- Windows 10 또는 11 (x64)
- 관리자 권한(NTFS 색인을 읽는 데 필요해서 실행할 때 묻습니다)

## 사용법

1. Chatda를 실행합니다. 첫 색인이 끝날 때까지 제목에 `Chatda - 색인 중...`이 표시되고, 상태 표시줄에 색인된 항목 수가 나옵니다.
2. 검색창에 파일·폴더 이름의 일부를 입력합니다.
3. 결과를 더블클릭하거나 Enter를 누르면 열립니다. 오른쪽 클릭하면 다음 메뉴가 나옵니다.
   - `열기`: 파일이나 폴더 열기
   - `파일 위치 열기` / `폴더 열기`: 탐색기에서 위치 열기
   - `경로 복사`: 전체 경로 복사
   - 해당 항목의 탐색기 오른쪽 클릭 메뉴

검색 결과는 가장 잘 맞는 2,000개까지 표시되고, 전체 일치 개수는 상태 표시줄에 나옵니다. Chatda를 다시 실행하면 새로 뜨지 않고 실행 중인 창이 앞으로 나옵니다.

### 검색 문법

여러 조건을 함께 쓸 수 있고, 모든 조건에 맞는 항목만 나옵니다.

| 검색어 | 찾는 항목 |
|---|---|
| `report 2024` | `report`와 `2024`가 모두 이름에 들어 있는 항목 |
| `*.pdf`, `report_20??.txt` | 이름 전체가 패턴과 맞는 항목(`*`는 아무 글자, `?`는 한 글자) |
| `ext:pdf`, `ext:pdf,docx` | 해당 확장자로 끝나는 항목 |
| `폴더:` / `파일:` | 폴더만 / 파일만(`folder:`, `file:`도 됩니다) |
| `docs\report` | 전체 경로에 `docs\report`가 들어 있는 항목(`/`도 됩니다) |
| `ㅂㄱㅅ` | 초성이 맞는 한글 이름, 예: `보고서.hwp` |

### 단축키

| 키 | 동작 |
|---|---|
| `Ctrl+Alt+F` | 어디서든 Chatda 띄우기 / 숨기기 |
| `Enter` | 선택한 결과 열기(검색창에서는 첫 번째 결과) |
| `Ctrl+Enter` | 선택한 결과의 위치 열기 |
| `↓` / `↑` | 검색창과 결과 목록 사이 이동 |
| `Ctrl+C` | 선택한 결과의 경로 복사 |
| `Ctrl+A` | 검색창의 글자 전체 선택, 결과 목록에서는 결과 전체 선택 |
| `Ctrl+L`, `F3` | 검색창으로 이동 |
| `Esc` | 검색어 지우기. 이미 비어 있으면 트레이로 숨기기 |

### 트레이

창을 닫으면 Chatda가 알림 영역(트레이)으로 숨어서 계속 색인을 최신으로 유지합니다. 트레이 아이콘을 클릭하면 다시 열리고, 오른쪽 클릭하면 `제외 목록 편집...`과 `종료`가 나옵니다.

### 제외 목록

트레이 메뉴나 창 메뉴(`Alt+Space`)에서 `제외 목록 편집...`을 엽니다. 한 줄에 규칙 하나씩 적습니다.

- `C:\Windows`처럼 전체 경로를 적으면 그 폴더와 안의 모든 항목이 빠집니다.
- `node_modules`처럼 백슬래시 없이 이름만 적으면 그 이름의 항목과 그 안의 모든 항목이 어디에 있든 빠집니다.

저장하면 다음 검색부터 적용됩니다. `$Recycle.Bin`과 `System Volume Information`은 기본으로 빠져 있습니다.

## 데이터와 설정

| 데이터 | 위치 |
|---|---|
| 색인 캐시 | `%LOCALAPPDATA%\Chatda\index-cache-v1.bin` |
| 제외 목록 | `%LOCALAPPDATA%\Chatda\exclude.txt` |

프로그램을 제거하면 이 폴더도 함께 지워집니다.

## 제한 사항

- NTFS가 아닌 드라이브(FAT32, exFAT)에서는 새로 만들거나 이름을 바꾸거나 지운 파일이 다음 백그라운드 갱신(최대 10분) 뒤에 반영됩니다.
- 파일 내용은 검색하지 않고 파일·폴더 이름만 검색합니다.
- 다른 프로그램이 이미 `Ctrl+Alt+F`를 쓰고 있으면 전역 단축키를 쓸 수 없습니다.

## 소스에서 빌드

Visual Studio 2022 (C++ 데스크톱 개발 워크로드, v143 도구 집합, C++20)와 Windows 10/11 SDK가 필요합니다.

### 빌드

Visual Studio에서 [Chatda.sln](Chatda.sln)을 열고 `Release|x64`로 빌드하거나, 다음 명령을 실행합니다.

```powershell
msbuild Chatda.sln /p:Configuration=Release /p:Platform=x64
```

결과물은 `Chatda\build\Release\Chatda.exe`에 생성됩니다.

### 배포

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\Build-Installer.ps1
```

이 스크립트는 VC++ 런타임을 exe에 포함해 앱을 빌드하고, `artifacts\installer`에 설치 파일(`Chatda-Setup-<버전>.exe`)을 만듭니다. 설치할 때 설치 폴더를 직접 고를 수 있습니다(기본값 `C:\Program Files\Chatda`). [Inno Setup 6](https://jrsoftware.org/isinfo.php)이 필요합니다(`winget install JRSoftware.InnoSetup`).

### 버전 관리

버전 번호는 [`Chatda/version.txt`](Chatda/version.txt) 한 곳에서만 관리하며, 빌드와 배포 스크립트가 이 값을 사용합니다.

## 프로젝트 구조

| 경로 | 내용 |
|---|---|
| `Chatda/main.cpp` | 진입점, 중복 실행 확인 |
| `Chatda/MainWindow.cpp` | 메인 창, 검색창, 결과 목록, 상태 표시줄, 트레이, 단축키, 작업 스레드 |
| `Chatda/SearchIndex.cpp` | 색인 생성(USN 저널 / 폴더 탐색), 저널 실시간 반영, 캐시, 검색 문법, 제외 목록, 정렬 |
| `Chatda/Strings.cpp` | 한국어·영어 화면 문구 |
| `Chatda/UpdateCheck.cpp` | GitHub 릴리스 업데이트 확인 |
| `Chatda/app.ico` | 앱 아이콘 |
| `installer/Chatda.iss` | Inno Setup 설치 스크립트 |
| `scripts/Build-Installer.ps1` | Release 빌드와 설치 파일 생성 |
| `docs/` | README용 아이콘 |

## 사용한 라이브러리

- [Inno Setup](https://jrsoftware.org/isinfo.php): 설치 프로그램
