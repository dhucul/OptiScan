; ============================================================================
;  OptiScan - Inno Setup installer script
;  Builds a self-contained setup that installs the x64 Release build and,
;  if necessary, the Microsoft Visual C++ 2015-2022 x64 runtime.
;
;  Compile with:
;     "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\OptiScan.iss
;  Output:
;     installer\Output\OptiScan-<version>-Setup.exe
; ============================================================================

#define MyAppName        "OptiScan"
#define MyAppVersion      "3.12"
#define MyAppPublisher    "dhucul"
#define MyAppURL          "https://github.com/dhucul/OptiScan"
#define MyAppExeName       "OptiScan.exe"

; Repo root, derived from this script's location (installer\..)
#define RepoDir           SourcePath + ".."
#define ReleaseDir         RepoDir + "\x64\Release"

[Setup]
; A unique AppId keeps upgrades/uninstall consistent across versions.
AppId={{8C244A24-70AC-4EE9-8596-6F8132A87254}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}/releases
VersionInfoVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} Setup

DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
UninstallDisplayName={#MyAppName} {#MyAppVersion}
UninstallDisplayIcon={app}\{#MyAppExeName}

; 64-bit only application.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

; Administrator install (needed for Program Files and the VC++ runtime).
PrivilegesRequired=admin

; Branding / appearance.
SetupIconFile={#RepoDir}\OptiScan.ico
WizardStyle=modern
LicenseFile={#RepoDir}\LICENSE.txt

; Output.
OutputDir={#SourcePath}Output
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
; Main application (single self-contained exe; UI assets/icons are embedded resources).
Source: "{#ReleaseDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoDir}\LICENSE.txt";        DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoDir}\README.md";          DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoDir}\OptiScan.ico";       DestDir: "{app}"; Flags: ignoreversion

; Visual C++ 2015-2022 x64 runtime - extracted to {tmp} and run only when missing.
Source: "{#SourcePath}redist\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: VCRedistNeedsInstall

[Icons]
Name: "{group}\{#MyAppName}";        Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\OptiScan.ico"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}";  Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\OptiScan.ico"; Tasks: desktopicon

[Run]
; Install the VC++ runtime first (only if needed), then optionally launch the app.
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; \
    StatusMsg: "Installing Microsoft Visual C++ 2015-2022 Runtime..."; \
    Check: VCRedistNeedsInstall; Flags: waituntilterminated

Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; \
    Flags: nowait postinstall skipifsilent

[Code]
{ Returns True when the VC++ 2015-2022 x64 runtime is absent or older than the
  build the application was compiled against (vcruntime140_1 requires >= 14.20). }
function VCRedistNeedsInstall: Boolean;
var
  Installed, Bld: Cardinal;
  KeyPath: String;
begin
  Result := True;
  KeyPath := 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\X64';
  if RegQueryDWordValue(HKLM64, KeyPath, 'Installed', Installed) and (Installed = 1) then
  begin
    if RegQueryDWordValue(HKLM64, KeyPath, 'Bld', Bld) then
      Result := (Bld < 30000)   { older than ~14.30; force update }
    else
      Result := False;          { installed, no build value - assume OK }
  end;
end;
