<p align="center">
  <img src="docs/icon.png" width="128" alt="Chatda icon">
</p>

<h1 align="center">Chatda</h1>

<p align="center">An instant file and folder name search for Windows, written in native C++.</p>

<p align="center">Author: <b>Manggdy</b></p>

<p align="center"><b>English</b> | <a href="README.ko.md">한국어</a></p>

Chatda indexes every file and folder name on your local drives, then shows matches as you type. On NTFS drives it reads the USN journal directly, so building the index is much faster than scanning every folder.

## Download

Get `Chatda-Setup-<version>.exe` from the [Releases](https://github.com/mrkim0118/Chatda/releases/latest) page and run it. You can choose the install folder (default `C:\Program Files\Chatda`).

## Features

- **Search as you type**: Results update while you type, with no Enter needed.
- **Multi-word search**: Every word must appear in the name, in any order (case-insensitive).
- **Smart ranking**: Exact name first, then names starting with the first word, then the rest; folders before files.
- **Fast indexing**: Reads the NTFS USN journal; non-NTFS drives fall back to a recursive scan.
- **All local drives**: Fixed, removable and RAM drives are indexed.
- **Index cache**: The index is saved on first build, so the next start shows results immediately.
- **Background refresh**: The index is rebuilt every 10 minutes, so new files show up.
- **Open from results**: Right-click a result to open it or open its location in Explorer.
- **System icons**: Each result shows its Windows file type icon.

## Requirements

- Windows 10 or 11 (x64)

## Usage

1. Start Chatda. The title shows `Chatda - Indexing...` until the first index is ready.
2. Type part of a file or folder name in the search box.
3. Right-click a result:
   - `Open`: open the file or folder
   - `Open File Location` / `Open Folder`: show it in Explorer

Search shows up to 2,000 results.

## Data & Settings

| Data | Location |
|---|---|
| Index cache | `%LOCALAPPDATA%\Chatda\index-cache-v1.bin` |

## Limitations

- New, renamed or deleted files appear after the next background refresh (up to 10 minutes).
- Only file and folder names are searched, not file contents.

## Build from Source

Requires Visual Studio 2022 with the C++ desktop workload (v143 toolset, C++20) and a Windows 10/11 SDK.

### Build

Open [Chatda.sln](Chatda.sln) in Visual Studio and build `Release|x64`, or run:

```powershell
msbuild Chatda.sln /p:Configuration=Release /p:Platform=x64
```

The output is `Chatda\build\Release\Chatda.exe`.

### Release

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\Build-Installer.ps1
```

The script builds the app with the VC++ runtime linked statically and creates an installer (`Chatda-Setup-<version>.exe`) in `artifacts\installer`. The installer lets the user choose the install folder (default `C:\Program Files\Chatda`). It requires [Inno Setup 6](https://jrsoftware.org/isinfo.php) (`winget install JRSoftware.InnoSetup`).

### Versioning

[`Chatda/version.txt`](Chatda/version.txt) is the single source of the version number used by the installer script.

## Project Layout

| Path | Contents |
|---|---|
| `Chatda/main.cpp` | Entry point |
| `Chatda/MainWindow.cpp` | Main window, search box, result list, context menu, worker threads |
| `Chatda/SearchIndex.cpp` | Index build (USN journal / recursive scan), cache, search and ranking |
| `Chatda/app.ico` | App icon |
| `installer/Chatda.iss` | Inno Setup installer script |
| `scripts/Build-Installer.ps1` | Release build and installer packaging |
| `docs/` | README icon |

## Libraries

- [Inno Setup](https://jrsoftware.org/isinfo.php): installer
