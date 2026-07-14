@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere not found; is Visual Studio installed?
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do (
    set "VSPATH=%%i"
)

set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo vcvars64.bat not found under %VSPATH%
    exit /b 1
)

call "%VCVARS%" >nul

set "SRC=%~dp0"
set "OUT=%SRC%build"
if not exist "%OUT%" mkdir "%OUT%"

cd /d "%OUT%"

cl /nologo /O2 /W3 /LD /DWV_BUILD_SHARED /I "%SRC%include" "%SRC%src\wavelet.c" /Fe:wavelet.dll
if %errorlevel% neq 0 exit /b %errorlevel%

rc /nologo /fo app.res "%SRC%gui\app.rc"
if %errorlevel% neq 0 exit /b %errorlevel%

cl /nologo /O2 /W3 /I "%SRC%include" "%SRC%src\cli.c" app.res wavelet.lib /Fe:wavelet.exe
if %errorlevel% neq 0 exit /b %errorlevel%

cl /nologo /O2 /W3 /DWV_STATIC /I "%SRC%include" "%SRC%test\test.c" "%SRC%src\wavelet.c" /Fe:wavelet_test.exe
if %errorlevel% neq 0 exit /b %errorlevel%

cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /I "%SRC%include" /I "%SRC%gui" "%SRC%gui\main.c" "%SRC%gui\image_io.c" "%SRC%gui\png.c" app.res wavelet.lib comctl32.lib comdlg32.lib gdi32.lib user32.lib /Fe:wavelet_gui.exe /link /SUBSYSTEM:WINDOWS
if %errorlevel% neq 0 exit /b %errorlevel%

echo built: wavelet.dll, wavelet.lib, wavelet.exe, wavelet_test.exe, wavelet_gui.exe (in %OUT%)
