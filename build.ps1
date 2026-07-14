# build.ps1 -- build the Windows DLL + CLI with MSVC (no CMake required).
# Produces build\wavelet.dll, build\wavelet.lib, build\wavelet.exe
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found; is Visual Studio installed?" }
$vsPath = & $vswhere -latest -products * -property installationPath
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vsPath" }

$src = $PSScriptRoot
$out = Join-Path $src 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$bat = @"
@echo off
call "$vcvars" >nul
cd /d "$out"
cl /nologo /O2 /W3 /LD /DWV_BUILD_SHARED /I "$src\include" "$src\src\wavelet.c" /Fe:wavelet.dll
if errorlevel 1 exit /b 1
rc /nologo /fo app.res "$src\gui\app.rc"
if errorlevel 1 exit /b 1
cl /nologo /O2 /W3 /I "$src\include" "$src\src\cli.c" app.res wavelet.lib /Fe:wavelet.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /W3 /DWV_STATIC /I "$src\include" "$src\test\test.c" "$src\src\wavelet.c" /Fe:wavelet_test.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /I "$src\include" /I "$src\gui" "$src\gui\main.c" "$src\gui\image_io.c" "$src\gui\png.c" app.res wavelet.lib comctl32.lib comdlg32.lib gdi32.lib user32.lib /Fe:wavelet_gui.exe /link /SUBSYSTEM:WINDOWS
if errorlevel 1 exit /b 1
"@
$batFile = Join-Path $out '_build.bat'
Set-Content -Path $batFile -Value $bat -Encoding Ascii
cmd /c "`"$batFile`""
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "built: wavelet.dll, wavelet.lib, wavelet.exe, wavelet_test.exe, wavelet_gui.exe (in $out)"
