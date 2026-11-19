param(
    [string]$Version = "",
    [string]$OutputDir = "artifacts\installer"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-VersionFromFile {
    param([Parameter(Mandatory = $true)][string]$ProjectDir)

    $versionPath = Join-Path $ProjectDir "version.txt"
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

function Get-IsccPath {
    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
        (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    throw "Inno Setup 6 (ISCC.exe) is not installed. Run: winget install JRSoftware.InnoSetup"
}

function Invoke-NativeReleaseBuild {
    param(
        [Parameter(Mandatory = $true)][string]$VcVarsPath,
        [Parameter(Mandatory = $true)][string]$ProjectDir
    )

    # /MT links the VC++ runtime statically, so the installer needs no vcredist.
    $command = @"
@echo off
call "$VcVarsPath" >nul
if errorlevel 1 exit /b 1
if not exist "obj\Release" mkdir "obj\Release"
if not exist "build\Release" mkdir "build\Release"
rc /nologo /fo "obj\Release\app.res" "app.rc"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /MT /DUNICODE /D_UNICODE /DWIN32 /D_WINDOWS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fo"obj\Release\\" "MainWindow.cpp" "main.cpp" "SearchIndex.cpp" /link /DEBUG:NONE /OUT:"build\Release\Chatda.exe" /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" User32.lib Gdi32.lib Shell32.lib Ole32.lib Comctl32.lib "obj\Release\app.res"
if errorlevel 1 exit /b 1
exit /b 0
"@

    $batchPath = [System.IO.Path]::ChangeExtension([System.IO.Path]::GetTempFileName(), ".cmd")
    try {
        Set-Content -LiteralPath $batchPath -Value $command -Encoding ASCII
        # The batch is ASCII, so a non-ASCII project path cannot appear in it; run from that folder instead.
        Push-Location -LiteralPath $ProjectDir
        try { & cmd.exe /d /c $batchPath } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) {
            throw "Native release build failed."
        }
    }
    finally {
        Remove-Item -LiteralPath $batchPath -Force -ErrorAction SilentlyContinue
    }
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$projectDir = Join-Path $repoRoot "Chatda"
$issPath = Join-Path $repoRoot "installer\Chatda.iss"
$outputDirPath = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $OutputDir))

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = Get-VersionFromFile -ProjectDir $projectDir
}

$isccPath = Get-IsccPath

Write-Host "1/2 Native release build"
Invoke-NativeReleaseBuild -VcVarsPath (Get-VcVarsPath) -ProjectDir $projectDir
$exePath = Join-Path $projectDir "build\Release\Chatda.exe"
if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Build output was not found: $exePath"
}

Write-Host "2/2 Building installer"
New-Item -ItemType Directory -Force -Path $outputDirPath | Out-Null
& $isccPath /Q "/DAppVersion=$Version" "/DSourceExe=$exePath" "/DOutputDir=$outputDirPath" $issPath
if ($LASTEXITCODE -ne 0) {
    throw "ISCC failed."
}

$setupPath = Join-Path $outputDirPath "Chatda-Setup-$Version.exe"
if (-not (Test-Path -LiteralPath $setupPath)) {
    throw "Setup file was not found: $setupPath"
}
Write-Host ""
Write-Host "Installer: $setupPath"
