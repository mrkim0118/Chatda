<p align="center">
  <img src="docs/icon.png" width="128" alt="Chatda 아이콘">
</p>

<h1 align="center">Chatda</h1>

<p align="center">네이티브 C++로 만든 Windows용 초고속 파일·폴더 이름 검색 프로그램입니다.</p>

<p align="center">Author: <b>Manggdy</b></p>

<p align="center"><a href="README.md">English</a> | <b>한국어</b></p>

Chatda는 로컬 드라이브의 모든 파일·폴더 이름을 색인해 두고, 입력하는 즉시 결과를 보여줍니다. NTFS 드라이브에서는 USN 저널을 직접 읽어서 모든 폴더를 훑는 것보다 훨씬 빠르게 색인을 만듭니다.

## 주요 기능

- **입력 즉시 검색**: Enter 없이 입력하는 동안 결과가 바로 바뀝니다.
- **여러 단어 검색**: 입력한 단어가 순서와 대소문자에 상관없이 모두 이름에 들어 있는 항목을 찾습니다.
- **똑똑한 정렬**: 이름이 정확히 같은 항목, 첫 단어로 시작하는 항목, 나머지 순으로 보여주며 폴더가 파일보다 먼저 나옵니다.
- **빠른 색인**: NTFS USN 저널을 읽고, NTFS가 아닌 드라이브는 폴더를 직접 훑어 색인합니다.
- **모든 로컬 드라이브**: 고정 디스크, 이동식 디스크, RAM 디스크를 색인합니다.
- **색인 캐시**: 처음 만든 색인을 저장해 두어 다음 실행 때 바로 검색할 수 있습니다.
- **백그라운드 갱신**: 10분마다 색인을 다시 만들어 새 파일을 반영합니다.
- **결과에서 바로 열기**: 결과를 오른쪽 클릭해 열거나 탐색기에서 위치를 엽니다.
- **시스템 아이콘**: 결과마다 Windows 파일 형식 아이콘을 보여줍니다.

## 요구 사항

- Windows 10 또는 11 (x64)

## 사용법

1. Chatda를 실행합니다. 첫 색인이 끝날 때까지 제목에 `Chatda - Indexing...`이 표시됩니다.
2. 검색창에 파일·폴더 이름의 일부를 입력합니다.
3. 결과를 오른쪽 클릭합니다.
   - `Open`: 파일이나 폴더 열기
   - `Open File Location` / `Open Folder`: 탐색기에서 위치 열기

검색 결과는 최대 2,000개까지 표시됩니다.

## 데이터와 설정

| 데이터 | 위치 |
|---|---|
| 색인 캐시 | `%LOCALAPPDATA%\Chatda\index-cache-v1.bin` |

## 제한 사항

- 새로 만들거나 이름을 바꾸거나 지운 파일은 다음 백그라운드 갱신(최대 10분) 뒤에 반영됩니다.
- 파일 내용은 검색하지 않고 파일·폴더 이름만 검색합니다.

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

버전 번호는 [`Chatda/version.txt`](Chatda/version.txt) 한 곳에서만 관리하며, 배포 스크립트가 이 값을 사용합니다.

## 프로젝트 구조

| 경로 | 내용 |
|---|---|
| `Chatda/main.cpp` | 진입점 |
| `Chatda/MainWindow.cpp` | 메인 창, 검색창, 결과 목록, 오른쪽 클릭 메뉴, 작업 스레드 |
| `Chatda/SearchIndex.cpp` | 색인 생성(USN 저널 / 폴더 탐색), 캐시, 검색과 정렬 |
| `Chatda/app.ico` | 앱 아이콘 |
| `installer/Chatda.iss` | Inno Setup 설치 스크립트 |
| `scripts/Build-Installer.ps1` | Release 빌드와 설치 파일 생성 |
| `docs/` | README용 아이콘 |

## 사용한 라이브러리

- [Inno Setup](https://jrsoftware.org/isinfo.php): 설치 프로그램
