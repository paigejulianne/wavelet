@echo off
setlocal

set "ISCC="

:: 1. Try registry (usually the most reliable if it was installed properly)
for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1" /v "InstallLocation" 2^>nul') do (
    if exist "%%B\ISCC.exe" set "ISCC=%%B\ISCC.exe"
)
if not defined ISCC (
    for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1" /v "InstallLocation" 2^>nul') do (
        if exist "%%B\ISCC.exe" set "ISCC=%%B\ISCC.exe"
    )
)
if not defined ISCC (
    for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 5_is1" /v "InstallLocation" 2^>nul') do (
        if exist "%%B\ISCC.exe" set "ISCC=%%B\ISCC.exe"
    )
)
if not defined ISCC (
    for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 5_is1" /v "InstallLocation" 2^>nul') do (
        if exist "%%B\ISCC.exe" set "ISCC=%%B\ISCC.exe"
    )
)

:: 2. Fallback to common paths
if not defined ISCC (
    if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" (
        set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
    ) else if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" (
        set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
    ) else if exist "%ProgramFiles(x86)%\Inno Setup 5\ISCC.exe" (
        set "ISCC=%ProgramFiles(x86)%\Inno Setup 5\ISCC.exe"
    ) else if exist "%ProgramFiles%\Inno Setup 5\ISCC.exe" (
        set "ISCC=%ProgramFiles%\Inno Setup 5\ISCC.exe"
    ) else if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" (
        set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
    ) else if exist "%LOCALAPPDATA%\Programs\Inno Setup 5\ISCC.exe" (
        set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 5\ISCC.exe"
    ) else if exist "C:\Inno Setup 6\ISCC.exe" (
        set "ISCC=C:\Inno Setup 6\ISCC.exe"
    ) else if exist "C:\Inno Setup 5\ISCC.exe" (
        set "ISCC=C:\Inno Setup 5\ISCC.exe"
    ) else (
        for %%X in (ISCC.exe) do (set "ISCC=%%~$PATH:X")
    )
)

if not defined ISCC (
    echo Inno Setup Compiler ^(ISCC.exe^) not found. Please install Inno Setup ^(https://jrsoftware.org/isinfo.php^) and ensure it's in your PATH or standard program files directory.
    exit /b 1
)

echo Found Inno Setup Compiler at: %ISCC%
echo Compiling installer.iss...

"%ISCC%" "installer.iss"

if %errorlevel% equ 0 (
    echo Success! The installer has been generated in the build\ directory.
) else (
    echo Failed to compile the installer.
    exit /b %errorlevel%
)
