; Inno Setup 6 script for the Windows installer of Claudius Maximus (per-user, no admin rights).
;
;   ISCC.exe /DAppVersion=0.3.0 /DArch=x64 /DDistDir=..\..\dist\ClaudiusMaximus /DOutputDir=..\..\dist\installer ^
;            packaging\windows\installer.iss
;
; Relative paths are relative to this file. Output: ClaudiusMaximus-<version>-windows-<arch>.exe
; The release workflow passes the version from host/companion/version.py (packaging/check_version.py).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Arch
  #define Arch "x64"
#endif
#ifndef DistDir
  #define DistDir "..\..\dist\ClaudiusMaximus"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist\installer"
#endif
#define AppName "Claudius Maximus"
#define AppExe "ClaudiusMaximus.exe"

[Setup]
; Never change the AppId: it identifies the installation for upgrades and the uninstaller.
AppId={{6F1B7E2A-4C1D-4E7B-9C55-2D3A8B0C9E41}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Claudius Maximus
AppPublisherURL=https://github.com/d-weber/claudius-maximus
AppSupportURL=https://github.com/d-weber/claudius-maximus/issues
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\ClaudiusMaximus
DisableDirPage=auto
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=ClaudiusMaximus-{#AppVersion}-windows-{#Arch}
SetupIconFile=..\icons\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Close the running tray app (it holds the exe and its _internal\*.pyd/.dll open) before replacing or removing
; files, via the Restart Manager; the [Run] entry starts the new version afterwards.
CloseApplications=force
CloseApplicationsFilter=*.exe,*.dll,*.pyd
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "Start {#AppName} when I log in"; GroupDescription: "Startup:"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Parameters: "app"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Parameters: "app"; Tasks: desktopicon

[Registry]
; Same value as companion/autostart.py writes, so the tray's "Start at login" toggle and the installer agree.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "ClaudiusMaximus"; \
  ValueData: """{app}\{#AppExe}"" app"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#AppExe}"; Parameters: "app"; Description: "{cm:LaunchProgram,{#AppName}}"; \
  Flags: nowait postinstall skipifsilent

[UninstallRun]
; Remove our hooks and statusLine wrapper from ~/.claude/settings.json while the exe is still there,
; then make sure no copy of the app keeps files open.
Filename: "{app}\{#AppExe}"; Parameters: "hooks uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "HooksUninstall"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /T /IM {#AppExe}"; Flags: runhidden waituntilterminated; RunOnceId: "KillApp"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
