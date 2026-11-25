; Inno Setup script for the Windows installer. The release workflow runs:
;   iscc /DAppVersion=<version> /DSourceDir=<folder with Moonsprout.exe> /DOutputName=<file name> moonsprout.iss
; The installer is per user by default (no administrator rights needed); the setup can also install
; for all users. Settings and saves stay in %APPDATA%\mppm and are kept on uninstall.

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\Moonsprout"
#endif
#ifndef OutputName
  #define OutputName "Moonsprout-setup"
#endif

[Setup]
AppId={{C1C82A41-4090-4DD1-B1D9-F80B710382E3}
AppName=Moonsprout
AppVersion={#AppVersion}
AppVerName=Moonsprout {#AppVersion}
AppPublisher=Moonsprout contributors
AppPublisherURL=https://github.com/owallzz/moonsprout
AppSupportURL=https://github.com/owallzz/moonsprout/issues
AppUpdatesURL=https://github.com/owallzz/moonsprout/releases
DefaultDirName={autopf}\Moonsprout
DefaultGroupName=Moonsprout
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=.
OutputBaseFilename={#OutputName}
SetupIconFile=moonsprout.ico
UninstallDisplayIcon={app}\Moonsprout.exe
LicenseFile={#SourceDir}\LICENSE.txt
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "portable.txt"

[Icons]
Name: "{autoprograms}\Moonsprout"; Filename: "{app}\Moonsprout.exe"
Name: "{autodesktop}\Moonsprout"; Filename: "{app}\Moonsprout.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Moonsprout.exe"; Description: "{cm:LaunchProgram,Moonsprout}"; Flags: nowait postinstall skipifsilent
