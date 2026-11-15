; Inno Setup script for Keyvok. Build with build-installers.cmd, which signs
; keyvok.exe first and defines the "keyvok" sign tool used below.
;
; Version, company and copyright are read from the exe's version resource
; (src/version.h), so the installer always matches the build.
;
; Paths in [Setup] and [Files] are relative to the repo root (SourceDir=..).

#define MyAppName "Keyvok"
#define MyAppExeName "keyvok.exe"
#define MyAppExe "build\release\" + MyAppExeName
#define MyAppExePath AddBackslash(SourcePath) + "..\" + MyAppExe

#if !FileExists(MyAppExePath)
  #error build\release\keyvok.exe not found; run make first
#endif

#define MyAppVersion GetStringFileInfo(MyAppExePath, "ProductVersion")
#define MyAppPublisher GetStringFileInfo(MyAppExePath, "CompanyName")
#define MyAppCopyright GetStringFileInfo(MyAppExePath, "LegalCopyright")

[Setup]
; Same AppId as v1, so this installer upgrades v1 (Full or Lite) in place.
AppId={{7F30AAC3-D834-4421-94B9-4CA64E37F631}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppCopyright={#MyAppCopyright}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2
SolidCompression=yes
SourceDir=..
DefaultDirName={autopf}\{#MyAppName}
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputBaseFilename=Keyvok-Setup-x64
OutputDir=build\installer
PrivilegesRequired=lowest
SetupIconFile=resources\icon.ico
SignTool=keyvok
UninstallDisplayIcon={app}\{#MyAppExeName}
VersionInfoCompany={#MyAppPublisher}
VersionInfoCopyright={#MyAppCopyright}
VersionInfoDescription={#MyAppName} Setup
VersionInfoProductName={#MyAppName}
VersionInfoVersion={#MyAppVersion}
WizardStyle=modern

[Files]
Source: "{#MyAppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[Code]
// Removes a v1 Lite exe before installing. Setup's check for running apps
// covers only files it replaces, so a running Lite is caught here: Windows
// will not delete a running exe.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  LiteExe: String;
begin
  Result := '';
  LiteExe := ExpandConstant('{app}\keyvok-lite.exe');
  if FileExists(LiteExe) and not DeleteFile(LiteExe) then
    Result := 'Keyvok Lite is running. Close it, then run this installer again.';
end;
