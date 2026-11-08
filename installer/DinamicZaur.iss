; Instalador de Dinamic Zaur (Inno Setup 6). Compilar despues de build.bat:
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\DinamicZaur.iss
; Instala la version de 64 o 32 bits segun el Windows; no necesita permisos de administrador.

#define AppName "Dinamic Zaur"
#define AppVersion "1.0.0"
#define AppPublisher "Zargch"
#define AppURL "https://github.com/Zargch/dinamic-zaur"

[Setup]
AppId={{BD397F9B-A3C3-4B44-AE13-D0262C865D25}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
DefaultDirName={localappdata}\Programs\DinamicZaur
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=DinamicZaur-{#AppVersion}-setup
SetupIconFile=..\res\app.ico
UninstallDisplayIcon={app}\DinamicZaur.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
MinVersion=10.0.17763
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startup"; Description: "{cm:AutoStartProgram,{#AppName}}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\dist\DinamicZaur-x64.exe"; DestDir: "{app}"; DestName: "DinamicZaur.exe"; Check: Is64BitInstallMode; Flags: ignoreversion
Source: "..\dist\DinamicZaur-x86.exe"; DestDir: "{app}"; DestName: "DinamicZaur.exe"; Check: not Is64BitInstallMode; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"
Source: "..\README.md"; DestDir: "{app}"

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\DinamicZaur.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\DinamicZaur.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "DinamicZaur"; ValueData: """{app}\DinamicZaur.exe"" --autostart"; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\DinamicZaur.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{cmd}"; Parameters: "/c taskkill /im DinamicZaur.exe /f"; Flags: runhidden; RunOnceId: "KillApp"

[UninstallDelete]
Type: filesandordirs; Name: "{userappdata}\DinamicZaur"
