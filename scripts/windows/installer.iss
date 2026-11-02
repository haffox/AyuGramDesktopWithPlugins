; Unsigned plugin-enabled fork. Distinct AppId: never uninstall the upstream app/profile.
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef Payload
  #error Payload directory is required
#endif
#ifndef Artifacts
  #error Artifacts directory is required
#endif
[Setup]
AppId={{C1F87729-84D8-4B9F-85C1-A42DDF5B6F28}
AppName=Veyra Desktop
AppVersion={#AppVersion}
AppPublisher=Veyra contributors
DefaultDirName={localappdata}\Veyra Desktop
DefaultGroupName=Veyra Desktop
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#Artifacts}
OutputBaseFilename=Veyra-{#AppVersion}-windows-x64-setup
UninstallDisplayIcon={app}\Veyra.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
[Files]
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Icons]
Name: "{group}\Veyra"; Filename: "{app}\Veyra.exe"
Name: "{userdesktop}\Veyra"; Filename: "{app}\Veyra.exe"; Tasks: desktopicon
[Run]
Filename: "{app}\Veyra.exe"; Description: "Launch Veyra Desktop"; Flags: nowait postinstall skipifsilent
; Do not delete tdata, plugin sources, account grants, or user profiles on uninstall.
