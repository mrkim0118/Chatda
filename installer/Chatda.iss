; Inno Setup script for Chatda. Built by scripts\Build-Installer.ps1, which passes
; AppVersion, SourceExe and OutputDir with /D.

#ifndef AppVersion
  #error AppVersion is not defined. Run scripts\Build-Installer.ps1.
#endif

[Setup]
AppId={{6E0C2F4B-6C1D-4E57-9D8A-2B7C5E3F9A41}
AppName=Chatda
AppVersion={#AppVersion}
AppVerName=Chatda {#AppVersion}
AppPublisher=Manggdy
DefaultDirName={autopf}\Chatda
DefaultGroupName=Chatda
; Show the "Select Destination Location" page, even on reinstall.
DisableDirPage=no
UsePreviousAppDir=no
DisableProgramGroupPage=yes
; Chatda runs elevated (it reads the NTFS USN journal), so install machine-wide.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupIconFile=..\Chatda\app.ico
UninstallDisplayIcon={app}\Chatda.exe
OutputDir={#OutputDir}
OutputBaseFilename=Chatda-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Chatda"; Filename: "{app}\Chatda.exe"
Name: "{autodesktop}\Chatda"; Filename: "{app}\Chatda.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Chatda.exe"; Description: "{cm:LaunchProgram,Chatda}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Index cache written by the app.
Type: filesandordirs; Name: "{localappdata}\Chatda"
