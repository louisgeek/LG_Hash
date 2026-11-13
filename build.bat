@echo off
rem Build LG_Hash for one or more architectures.
rem
rem   build.bat                 x64 only (default)
rem   build.bat x64 arm64       both
rem   build.bat arm64           ARM64 only
rem
rem Outputs:
rem   build\LG_Hash.dll/.exe           x64
rem   build\arm64\LG_Hash.dll/.exe     ARM64
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist build mkdir build

rem Version numbers live in exactly one place: src\version.h.
rem Generate build\version_gen.h for rc.exe (which cannot do # stringification),
rem then verify nothing reintroduced a hard-coded version elsewhere.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\gen-version-header.ps1" -OutputPath "%~dp0build\version_gen.h"
if errorlevel 1 (
    echo.
    echo BUILD FAILED - cannot generate version header
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\check-version.ps1" -Quiet
if errorlevel 1 (
    echo.
    echo BUILD FAILED - version numbers are not single-sourced
    exit /b 1
)

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio not found
    exit /b 1
)

set ARCHES=%*
if "%ARCHES%"=="" set ARCHES=x64

set FAILED=0
for %%a in (%ARCHES%) do (
    call :build_one %%a
    if errorlevel 1 set FAILED=1
)

if "%FAILED%"=="1" (
    echo.
    echo BUILD FAILED
    exit /b 1
)

echo.
echo BUILD OK
for %%a in (%ARCHES%) do (
    if /i "%%a"=="x64" (
        echo   build\LG_Hash.dll / .exe          x64
    ) else (
        echo   build\%%a\LG_Hash.dll / .exe     %%a
    )
)
exit /b 0

:build_one
set ARCH=%~1
set OUTDIR=build
if /i not "%ARCH%"=="x64" set OUTDIR=build\%ARCH%

rem vcvarsall picks the right host/target toolset for each architecture
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul 2>&1

rem vcvarsall exits 0 even when the target compiler is missing, so probe cl directly.
rem Checking for cl anywhere is not enough when several architectures are built in one
rem session: the previous arch already put its own cl.exe on PATH, and building ARM64
rem with the x64 compiler only surfaces as a pile of LNK4272 "machine type conflict".
rem So match the target directory in the compiler path (Hostx64\x64, Hostx64\arm64).
set "CLFOUND="
for /f "delims=" %%p in ('where cl 2^>nul') do (
    echo %%p | findstr /i /c:"\%ARCH%\cl.exe" >nul && set "CLFOUND=%%p"
)
if not defined CLFOUND (
    echo [%ARCH%] compiler not found for this target.
    if /i not "%ARCH%"=="x64" (
        echo          Install the MSVC ARM64 build tools:
        echo          Visual Studio Installer -^> Modify -^> Individual components
        echo          -^> MSVC C++ ARM64 build tools ^(latest^)
    )
    exit /b 1
)

rem /MT static CRT: a shell extension must not require the VC++ redistributable
set COMMON=/nologo /std:c++17 /O2 /W3 /EHsc /utf-8 /DUNICODE /D_UNICODE /MT
set LIBS=ole32.lib oleaut32.lib uuid.lib bcrypt.lib comctl32.lib user32.lib gdi32.lib shell32.lib shlwapi.lib advapi32.lib

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

rem vcvarsall points rc.exe at the target architecture, but rc runs on the host:
rem an ARM64 rc.exe cannot execute on an x64 build machine. Use the host rc instead.
set "RC=rc.exe"
if /i not "%ARCH%"=="x64" (
    set "RC="
    for %%v in (10.0.26100.0 10.0.22621.0 10.0.20348.0 10.0.19041.0) do (
        if not defined RC if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\%%v\x64\rc.exe" (
            set "RC=%ProgramFiles(x86)%\Windows Kits\10\bin\%%v\x64\rc.exe"
        )
    )
    if not defined RC (
        echo [%ARCH%] host rc.exe not found - cannot build ARM64 without the Windows SDK
        exit /b 1
    )
)

echo [%ARCH% 1/3] resource
"%RC%" /nologo /I src /I build /fo %OUTDIR%\LG_Hash.res src\LG_Hash.rc
if errorlevel 1 exit /b 1

echo [%ARCH% 2/3] shell extension dll
cl %COMMON% /LD src\dllmain.cpp src\HashExt.cpp src\LG_Hash.cpp src\LG_HashDlg.cpp src\LG_HashDraw.cpp src\HashJob.cpp src\HashEngine.cpp src\Lang.cpp src\Theme.cpp src\PathUtil.cpp /Fo:%OUTDIR%\ /Fe:%OUTDIR%\LG_Hash.dll /link /DLL %OUTDIR%\LG_Hash.res %LIBS%
if errorlevel 1 exit /b 1

echo [%ARCH% 3/3] installer exe
rem installer.rc embeds LG_Hash.dll from OUTDIR, so it must run after the DLL link
"%RC%" /nologo /I src /I build /I %OUTDIR% /d LG_HASH_DLL_PATH=%OUTDIR%\LG_Hash.dll /fo %OUTDIR%\installer.res src\installer.rc
if errorlevel 1 exit /b 1
cl %COMMON% /D_WINDOWS src\installer.cpp /Fo:%OUTDIR%\ /Fe:%OUTDIR%\LG_Hash.exe /link /SUBSYSTEM:WINDOWS %OUTDIR%\installer.res %LIBS%
if errorlevel 1 exit /b 1

exit /b 0
