; Installer di Kurva3D (Inno Setup 6). Compilato dalla CI Windows:
;   ISCC.exe /DAppVersion=0.8.0 /DSourceDir=..\dist /DOutputDir=..\out installer\kurva3d.iss
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist"
#endif
#ifndef OutputDir
  #define OutputDir "..\out"
#endif
#ifndef Publisher
  #define Publisher "Kurva3D"
#endif

[Setup]
AppId={{F403F35B-3D5A-48AE-8B46-544EF3877912}
AppName=Kurva3D
AppVersion={#AppVersion}
AppVerName=Kurva3D {#AppVersion}
AppPublisher={#Publisher}
DefaultDirName={autopf}\Kurva3D
DefaultGroupName=Kurva3D
DisableProgramGroupPage=yes
LicenseFile={#SourceDir}\EULA.txt
OutputDir={#OutputDir}
OutputBaseFilename=Kurva3D-Setup-{#AppVersion}
SetupIconFile=..\res\kurva3d.ico
UninstallDisplayIcon={app}\kurva3d.exe
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern
ChangesAssociations=yes

[Languages]
Name: "it"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
; disattivata di default: i file .3dm restano associati al programma che li apre ora
Name: "assoc3dm"; Description: "Apri i file .3dm con Kurva3D"; GroupDescription: "Associazioni:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "screenshot-avvio.png,commands-test.exe"

[Icons]
Name: "{autoprograms}\Kurva3D"; Filename: "{app}\kurva3d.exe"
Name: "{autoprograms}\Licenza d'uso di Kurva3D"; Filename: "{app}\EULA.txt"
Name: "{autodesktop}\Kurva3D"; Filename: "{app}\kurva3d.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.3dm\OpenWithProgids"; ValueType: string; ValueName: "Kurva3D.3dm"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc3dm
Root: HKA; Subkey: "Software\Classes\Kurva3D.3dm"; ValueType: string; ValueName: ""; ValueData: "Modello 3D (.3dm)"; Flags: uninsdeletekey; Tasks: assoc3dm
Root: HKA; Subkey: "Software\Classes\Kurva3D.3dm\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\kurva3d.exe,0"; Tasks: assoc3dm
Root: HKA; Subkey: "Software\Classes\Kurva3D.3dm\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\kurva3d.exe"" ""%1"""; Tasks: assoc3dm

[Run]
Filename: "{app}\kurva3d.exe"; Description: "{cm:LaunchProgram,Kurva3D}"; Flags: nowait postinstall skipifsilent
