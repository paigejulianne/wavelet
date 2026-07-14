[Setup]
AppId={{6F2B4C3A-9E71-4B8D-8A1C-1D0E5A2B7C31}
AppName=Wavelet Image Studio
AppVersion=1.0
AppPublisher=Wavelet
DefaultDirName={autopf}\Wavelet
DefaultGroupName=Wavelet Image Studio
OutputDir=build
OutputBaseFilename=WaveletSetup
Compression=lzma
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64
WizardStyle=modern
; HKCR / HKLM writes below need elevation.
PrivilegesRequired=admin
; Tell Windows the setup changes file associations so Inno fires
; SHChangeNotify(SHCNE_ASSOCCHANGED). Without this the shell keeps the *cached*
; association + icon until logoff/reboot -- i.e. double-click appears "broken".
ChangesAssociations=yes
SetupIconFile=assets\logo.ico
UninstallDisplayIcon={app}\wavelet_gui.exe

[Files]
Source: "build\wavelet_gui.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\wavelet.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\wavelet.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Wavelet Image Studio"; Filename: "{app}\wavelet_gui.exe"
Name: "{group}\Uninstall Wavelet"; Filename: "{uninstallexe}"

[Registry]
; --- .wvlc file type -------------------------------------------------------
; A unique ProgID (not a generic name that could collide with another app),
; pointed at by both the extension's default value and its OpenWithProgids list
; (the latter makes the app show up under "Open with" on Windows 10/11).
Root: HKCR; Subkey: ".wvlc"; ValueType: string; ValueName: ""; ValueData: "WaveletImageStudio.wvlc"; Flags: uninsdeletevalue
Root: HKCR; Subkey: ".wvlc"; ValueType: string; ValueName: "PerceivedType"; ValueData: "image"; Flags: uninsdeletevalue
Root: HKCR; Subkey: ".wvlc"; ValueType: string; ValueName: "Content Type"; ValueData: "application/x-wvlc"; Flags: uninsdeletevalue
Root: HKCR; Subkey: ".wvlc\OpenWithProgids"; ValueType: string; ValueName: "WaveletImageStudio.wvlc"; ValueData: ""; Flags: uninsdeletevalue

Root: HKCR; Subkey: "WaveletImageStudio.wvlc"; ValueType: string; ValueName: ""; ValueData: "Wavelet Compressed Image"; Flags: uninsdeletekey
Root: HKCR; Subkey: "WaveletImageStudio.wvlc"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "Wavelet Compressed Image"
Root: HKCR; Subkey: "WaveletImageStudio.wvlc\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\wavelet_gui.exe,0"
Root: HKCR; Subkey: "WaveletImageStudio.wvlc\shell\open"; ValueType: string; ValueName: ""; ValueData: "&Open with Wavelet Image Studio"
Root: HKCR; Subkey: "WaveletImageStudio.wvlc\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\wavelet_gui.exe"" ""%1"""

; --- "Compress with Wavelet" on supported source images --------------------
Root: HKCR; Subkey: "SystemFileAssociations\.bmp\shell\CompressWithWavelet"; ValueType: string; ValueName: ""; ValueData: "Compress with Wavelet"; Flags: uninsdeletekey
Root: HKCR; Subkey: "SystemFileAssociations\.bmp\shell\CompressWithWavelet"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\wavelet_gui.exe,0"
Root: HKCR; Subkey: "SystemFileAssociations\.bmp\shell\CompressWithWavelet\command"; ValueType: string; ValueName: ""; ValueData: """{app}\wavelet_gui.exe"" ""%1"""
Root: HKCR; Subkey: "SystemFileAssociations\.png\shell\CompressWithWavelet"; ValueType: string; ValueName: ""; ValueData: "Compress with Wavelet"; Flags: uninsdeletekey
Root: HKCR; Subkey: "SystemFileAssociations\.png\shell\CompressWithWavelet"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\wavelet_gui.exe,0"
Root: HKCR; Subkey: "SystemFileAssociations\.png\shell\CompressWithWavelet\command"; ValueType: string; ValueName: ""; ValueData: """{app}\wavelet_gui.exe"" ""%1"""
Root: HKCR; Subkey: "SystemFileAssociations\.pgm\shell\CompressWithWavelet"; ValueType: string; ValueName: ""; ValueData: "Compress with Wavelet"; Flags: uninsdeletekey
Root: HKCR; Subkey: "SystemFileAssociations\.pgm\shell\CompressWithWavelet"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\wavelet_gui.exe,0"
Root: HKCR; Subkey: "SystemFileAssociations\.pgm\shell\CompressWithWavelet\command"; ValueType: string; ValueName: ""; ValueData: """{app}\wavelet_gui.exe"" ""%1"""
Root: HKCR; Subkey: "SystemFileAssociations\.ppm\shell\CompressWithWavelet"; ValueType: string; ValueName: ""; ValueData: "Compress with Wavelet"; Flags: uninsdeletekey
Root: HKCR; Subkey: "SystemFileAssociations\.ppm\shell\CompressWithWavelet"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\wavelet_gui.exe,0"
Root: HKCR; Subkey: "SystemFileAssociations\.ppm\shell\CompressWithWavelet\command"; ValueType: string; ValueName: ""; ValueData: """{app}\wavelet_gui.exe"" ""%1"""
