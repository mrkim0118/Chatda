param(
    [string]$Version = "",
    [string]$PackId = "TechArchive.UltraFastFileExplorer",
    [string]$PackTitle = "Ultra Fast File Explorer",
    [string]$Channel = "win",
    [string]$Runtime = "win-x64",
    [string]$MainExe = "UltraFastFileExplorer.exe",
    [string]$Authors = "TechArchive",
    [string]$DefaultUpdateSource = "https://tech.trendarchive.io/Utility/UltraFastFileExplorer/",
    [string]$PublishDir = "",
    [string]$OutputDir = "",
    [string]$ReleaseNotes = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-FullPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "Path value cannot be empty."
    }

    if ([System.IO.Path]::IsPathRooted($Value)) {
        return [System.IO.Path]::GetFullPath($Value)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Value))
}

function Convert-ToFileSafeName {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    $safe = $Value -replace '[^A-Za-z0-9]+', '-'
    return $safe.Trim('-')
}

function Get-VersionFromFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProjectDir
    )

    $versionPath = Join-Path $ProjectDir "latest-version.txt"
    if (-not (Test-Path -LiteralPath $versionPath)) {
        throw "Version file was not found: $versionPath"
    }

    $version = (Get-Content -LiteralPath $versionPath -Raw).Trim()
    if ([string]::IsNullOrWhiteSpace($version)) {
        throw "Version file is empty: $versionPath"
    }

    return $version
}

function Get-VcVarsPath {
    $vsWherePath = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vsWherePath)) {
        throw "vswhere.exe was not found."
    }

    $installationPath = (& $vsWherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installationPath)) {
        throw "A Visual C++ build tools installation could not be found."
    }

    $vcVarsPath = Join-Path $installationPath "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path -LiteralPath $vcVarsPath)) {
        throw "vcvars64.bat was not found at $vcVarsPath"
    }

    return $vcVarsPath
}

function Invoke-NativeReleaseBuild {
    param(
        [Parameter(Mandatory = $true)]
        [string]$VcVarsPath,
        [Parameter(Mandatory = $true)]
        [string]$ProjectDir
    )

    $command = @"
@echo off
call "$VcVarsPath" >nul
if errorlevel 1 exit /b 1
cd /d "$ProjectDir"
if not exist "obj\Release" mkdir "obj\Release"
if not exist "build\Release" mkdir "build\Release"
rc /nologo /fo "obj\Release\app.res" "app.rc"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /MD /DUNICODE /D_UNICODE /DWIN32 /D_WINDOWS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fo"obj\Release\\" "MainWindow.cpp" "main.cpp" "SearchIndex.cpp" "UpdateCheck.cpp" /link /DEBUG:NONE /OUT:"build\Release\UltraFastFileExplorer.exe" /SUBSYSTEM:WINDOWS User32.lib Gdi32.lib Shell32.lib Ole32.lib Comctl32.lib Winhttp.lib "obj\Release\app.res"
if errorlevel 1 exit /b 1
copy /Y "latest-version.txt" "build\Release\" >nul
if exist "update-url.txt" copy /Y "update-url.txt" "build\Release\" >nul
if exist "installer-url.txt" copy /Y "installer-url.txt" "build\Release\" >nul
exit /b 0
"@

    $batchPath = [System.IO.Path]::ChangeExtension([System.IO.Path]::GetTempFileName(), ".cmd")
    try {
        Set-Content -LiteralPath $batchPath -Value $command -Encoding ASCII
        & cmd.exe /d /c $batchPath
        if ($LASTEXITCODE -ne 0) {
            throw "Native release build failed."
        }
    }
    finally {
        Remove-Item -LiteralPath $batchPath -Force -ErrorAction SilentlyContinue
    }
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-FullPath -BasePath $scriptRoot -Value ".."
$projectDir = Resolve-FullPath -BasePath $repoRoot -Value "UltraFastFileExplorer"

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = Get-VersionFromFile -ProjectDir $projectDir
}

if ([string]::IsNullOrWhiteSpace($PublishDir)) {
    $PublishDir = "artifacts\publish\$Version\$Runtime"
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = "artifacts\velopack\Releases"
}

if ([string]::IsNullOrWhiteSpace($ReleaseNotes)) {
    $ReleaseNotes = "artifacts\velopack\release-notes-$Version.md"
}

$publishDirPath = Resolve-FullPath -BasePath $repoRoot -Value $PublishDir
$outputDirPath = Resolve-FullPath -BasePath $repoRoot -Value $OutputDir
$releaseNotesPath = Resolve-FullPath -BasePath $repoRoot -Value $ReleaseNotes
$iconPath = Join-Path $projectDir "app.ico"
$defaultsFilePath = Join-Path $publishDirPath "app-defaults.json"
$latestVersionOutputPath = Join-Path $outputDirPath "latest-version.txt"
$installerUrlOutputPath = Join-Path $outputDirPath "installer-url.txt"

New-Item -ItemType Directory -Force -Path $publishDirPath | Out-Null
New-Item -ItemType Directory -Force -Path $outputDirPath | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $releaseNotesPath) | Out-Null

if (Test-Path -LiteralPath $publishDirPath) {
    Get-ChildItem -LiteralPath $publishDirPath -Force | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path -LiteralPath $releaseNotesPath)) {
    @(
        "# Ultra Fast File Explorer $Version"
        ""
        "- Packaged release."
    ) | Set-Content -LiteralPath $releaseNotesPath -Encoding UTF8
}

$vcVarsPath = Get-VcVarsPath
Write-Host "1/3 Native release build"
Invoke-NativeReleaseBuild -VcVarsPath $vcVarsPath -ProjectDir $projectDir

$buildOutputDir = Join-Path $projectDir "build\Release"
if (-not (Test-Path -LiteralPath $buildOutputDir)) {
    throw "Build output directory was not found: $buildOutputDir"
}

Write-Host "2/3 Preparing publish directory"
Get-ChildItem -LiteralPath $buildOutputDir -Force | Copy-Item -Destination $publishDirPath -Recurse -Force
Copy-Item -LiteralPath (Join-Path $projectDir "latest-version.txt") -Destination $publishDirPath -Force
if (Test-Path -LiteralPath (Join-Path $projectDir "update-url.txt")) {
    Copy-Item -LiteralPath (Join-Path $projectDir "update-url.txt") -Destination $publishDirPath -Force
}
if (Test-Path -LiteralPath (Join-Path $projectDir "installer-url.txt")) {
    Copy-Item -LiteralPath (Join-Path $projectDir "installer-url.txt") -Destination $publishDirPath -Force
}

Get-ChildItem -LiteralPath $publishDirPath -Filter "*.pdb" -File -Recurse -ErrorAction SilentlyContinue |
    Remove-Item -Force

@{
    DefaultUpdateSource = $DefaultUpdateSource
} | ConvertTo-Json | Set-Content -LiteralPath $defaultsFilePath -Encoding UTF8

Set-Content -LiteralPath $latestVersionOutputPath -Value $Version -Encoding UTF8
$setupUrl = "$DefaultUpdateSource$PackId-$Channel-Setup.exe"
Set-Content -LiteralPath $installerUrlOutputPath -Value $setupUrl -Encoding UTF8

$vpkCommand = Get-Command vpk.exe -ErrorAction SilentlyContinue
if (-not $vpkCommand) {
    $fallbackVpkPath = Join-Path $env:USERPROFILE ".dotnet\tools\vpk.exe"
    if (Test-Path -LiteralPath $fallbackVpkPath) {
        $vpkCommand = Get-Command $fallbackVpkPath -ErrorAction SilentlyContinue
    }
}

if (-not $vpkCommand) {
    throw "The 'vpk' CLI tool is not installed."
}

Write-Host "3/3 Packing Velopack release"
& $vpkCommand.Source pack `
    --packId $PackId `
    --packVersion $Version `
    --packDir $publishDirPath `
    --mainExe $MainExe `
    --packAuthors $Authors `
    --packTitle $PackTitle `
    --channel $Channel `
    --runtime $Runtime `
    --releaseNotes $releaseNotesPath `
    --outputDir $outputDirPath `
    --icon $iconPath `
    --exclude ".*\.pdb$" `
    --framework "vcredist143-x64" `
    --shortcuts "Desktop,StartMenuRoot"

if ($LASTEXITCODE -ne 0) {
    throw "vpk pack failed."
}

$setupFileName = "$PackId-$Channel-Setup.exe"
$setupFilePath = Join-Path $outputDirPath $setupFileName
if (Test-Path -LiteralPath $setupFilePath) {
    $versionedSetupName = "{0}-Setup-{1}.exe" -f (Convert-ToFileSafeName -Value $PackTitle), $Version
    $versionedSetupPath = Join-Path $outputDirPath $versionedSetupName
    Copy-Item -LiteralPath $setupFilePath -Destination $versionedSetupPath -Force
    Write-Host "Versioned setup : $versionedSetupPath"
}

Write-Host ""
Write-Host "Velopack release complete."
Write-Host "Publish files : $publishDirPath"
Write-Host "Release feed  : $outputDirPath"
Write-Host "Update source : $DefaultUpdateSource"
