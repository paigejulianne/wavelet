[Setup]
AppName=Wavelet Image Studio
AppVersion=1.0
DefaultDirName={autopf}\Wavelet
DefaultGroupName=Wavelet Image Studio
OutputDir=build
OutputBaseFilename=WaveletSetup
Compression=lzma
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64

[Files]
Source: "build\wavelet_gui.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\wavelet.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\wavelet.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Wavelet Image Studio"; Filename: "{app}\wavelet_gui.exe"
Name: "{group}\Uninstall Wavelet"; Filename: "{uninstallexe}"

[Registry]
; Register .wvlc extension
Root: HKCR; Subkey: ".wvlc"; ValueType: string; ValueName: ""; ValueData: "WaveletContainer"; Flags: uninsdeletevalue
Root: HKCR; Subkey: "WaveletContainer"; ValueType: string; ValueName: ""; ValueData: "Wavelet Compressed Image"; Flags: uninsdeletekey
Root: HKCR; Subkey: "WaveletContainer\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\wavelet_gui.exe,0"; Flags: uninsdeletekey
Root: HKCR; Subkey: "WaveletContainer\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\wavelet_gui.exe"" ""%1"""; Flags: uninsdeletekey

; Add "Compress with Wavelet" to right-click menu for supported images
Root: HKCR; Subkey: "SystemFileAssociations\.bmp\shell\CompressWithWavelet"; ValueType: string; ValueName: ""; ValueData: "Compress with Wavelet"; Flags: uninsdeletekey
Root: HKCR; Subkey: "SystemFileAssociations\.bmp\shell\CompressWithWavelet\command"; ValueType: string; ValueName: ""; ValueData: """{app}\wavelet_gui.exe"" ""%1"""; Flags: uninsdeletekey
Root: HKCR; Subkey: "SystemFileAssociations\.png\shell\CompressWithWavelet"; ValueType: string; ValueName: ""; ValueData: "Compress with Wavelet"; Flags: uninsdeletekey
Root: HKCR; Subkey: "SystemFileAssociations\.png\shell\CompressWithWavelet\command"; ValueType: string; ValueName: ""; ValueData: """{app}\wavelet_gui.exe"" ""%1"""; Flags: uninsdeletekey
Root: HKCR; Subkey: "SystemFileAssociations\.pgm\shell\CompressWithWavelet"; ValueType: string; ValueName: ""; ValueData: "Compress with Wavelet"; Flags: uninsdeletekey
Root: HKCR; Subkey: "SystemFileAssociations\.pgm\shell\CompressWithWavelet\command"; ValueType: string; ValueName: ""; ValueData: """{app}\wavelet_gui.exe"" ""%1"""; Flags: uninsdeletekey
Root: HKCR; Subkey: "SystemFileAssociations\.ppm\shell\CompressWithWavelet"; ValueType: string; ValueName: ""; ValueData: "Compress with Wavelet"; Flags: uninsdeletekey
Root: HKCR; Subkey: "SystemFileAssociations\.ppm\shell\CompressWithWavelet\command"; ValueType: string; ValueName: ""; ValueData: """{app}\wavelet_gui.exe"" ""%1"""; Flags: uninsdeletekey
