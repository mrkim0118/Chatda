<p align="center">
  <img src="docs/icon.png" width="128" alt="Chatda icon">
</p>

<h1 align="center">Chatda</h1>

<p align="center">An instant file and folder name search for Windows, written in native C++.</p>

<p align="center">Author: <b>Manggdy</b></p>

<p align="center"><b>English</b> | <a href="README.ko.md">한국어</a></p>

Chatda indexes every file and folder name on your local drives, then shows matches as you type. On NTFS drives it reads the USN journal directly, so building the index is much faster than scanning every folder, and changes show up within a second.

## Download

Get `Chatda-Setup-<version>.exe` from the [Releases](https://github.com/mrkim0118/Chatda/releases/latest) page and run it. You can choose the install folder (default `C:\Program Files\Chatda`).

## Features

- **Search as you type**: Results update while you type, with no Enter needed.
- **Multi-word search**: Every word must appear in the name, in any order (case-insensitive).
- **Search syntax**: Wildcards (`*.pdf`), extension filters (`ext:pdf,docx`), folders or files only, and path search.
- **Korean initial consonants**: `ㅂㄱㅅ` finds "보고서".
- **Smart ranking**: Exact name first, then names starting with the first word, then the rest; folders before files.
- **Sortable columns**: Name, path, type, size and date modified; click a column header to sort.
- **Fast indexing**: Reads the NTFS USN journal; non-NTFS drives fall back to a recursive scan.
- **Live updates**: On NTFS drives, new, renamed, moved and deleted files show up within a second.
- **All local drives**: Fixed, removable and RAM drives are indexed, including drives plugged in later.
- **Exclusions**: Hide folders such as `$Recycle.Bin` or `node_modules` from the results.
- **Index cache**: The index is saved on first build, so the next start shows results immediately.
- **Tray and global hotkey**: Closing the window keeps Chatda in the tray; `Ctrl+Alt+F` brings it back from anywhere.
- **Open from results**: Double-click or press Enter to open; right-click for the Explorer context menu.
- **System icons**: Each result shows its Windows file type icon.
- **Korean and English UI**: Follows the Windows display language.
- **Update check**: On launch, checks the latest GitHub release and asks whether to download and install the new version.

## Requirements

- Windows 10 or 11 (x64)
- Administrator rights (Chatda asks on start; reading the NTFS index needs them)

## Usage

1. Start Chatda. The title shows `Chatda - Indexing...` until the first index is ready, and the status bar shows how many items are indexed.
2. Type part of a file or folder name in the search box.
3. Double-click a result or press Enter to open it. Right-click for more:
   - `Open`: open the file or folder
   - `Open File Location` / `Open Folder`: show it in Explorer
   - `Copy Path`: copy the full path
   - the Explorer context menu of the item

Search shows the best 2,000 results; the status bar tells how many matched in total. Launching Chatda again brings up the running window instead of starting a second copy.

### Search Syntax

Words can be combined; every part must match.

| Query | Finds |
|---|---|
| `report 2024` | Names containing both `report` and `2024` |
| `*.pdf`, `report_20??.txt` | Names matching the whole pattern (`*` any text, `?` one character) |
| `ext:pdf`, `ext:pdf,docx` | Names ending in one of the extensions |
| `folder:` / `file:` | Folders only / files only (`폴더:` and `파일:` also work) |
| `docs\report` | Items whose full path contains `docs\report` (`/` works too) |
| `ㅂㄱㅅ` | Korean names by their initial consonants, e.g. `보고서.hwp` |

### Keyboard Shortcuts

| Key | Action |
|---|---|
| `Ctrl+Alt+F` | Show or hide Chatda from anywhere |
| `Enter` | Open the selected result (from the search box: the first result) |
| `Ctrl+Enter` | Open the location of the selected result |
| `↓` / `↑` | Move between the search box and the results |
| `Ctrl+C` | Copy the paths of the selected results |
| `Ctrl+A` | Select all text in the search box, or all results in the list |
| `Ctrl+L`, `F3` | Go to the search box |
| `Esc` | Clear the search; when it is already empty, hide to the tray |

### Tray

Closing the window hides Chatda to the notification area, where it keeps the index up to date. Click the tray icon to open it; right-click for `Edit Exclusions...` and `Exit`.

### Exclusions

Open `Edit Exclusions...` from the tray menu or the window menu (`Alt+Space`). Write one rule per line:

- A full path such as `C:\Windows` hides that folder and everything inside it.
- A name without a backslash such as `node_modules` hides every item with that name and everything inside it.

Changes apply to the next search. `$Recycle.Bin` and `System Volume Information` are excluded by default.

## Data & Settings

| Data | Location |
|---|---|
| Index cache | `%LOCALAPPDATA%\Chatda\index-cache-v1.bin` |
| Exclusions | `%LOCALAPPDATA%\Chatda\exclude.txt` |

Uninstalling removes this folder.

## Limitations

- On non-NTFS drives (FAT32, exFAT), new, renamed or deleted files appear after the next background refresh (up to 10 minutes).
- Only file and folder names are searched, not file contents.
- The global hotkey is not available when another program already uses `Ctrl+Alt+F`.

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

[`Chatda/version.txt`](Chatda/version.txt) is the single source of the version number used by the build and the installer script.

## Project Layout

| Path | Contents |
|---|---|
| `Chatda/main.cpp` | Entry point, single-instance check |
| `Chatda/MainWindow.cpp` | Main window, search box, result list, status bar, tray, shortcuts, worker threads |
| `Chatda/SearchIndex.cpp` | Index build (USN journal / recursive scan), live journal updates, cache, search syntax, exclusions, ranking |
| `Chatda/Strings.cpp` | Korean and English UI text |
| `Chatda/UpdateCheck.cpp` | GitHub release update check |
| `Chatda/app.ico` | App icon |
| `installer/Chatda.iss` | Inno Setup installer script |
| `scripts/Build-Installer.ps1` | Release build and installer packaging |
| `docs/` | README icon |

## Libraries

- [Inno Setup](https://jrsoftware.org/isinfo.php): installer
