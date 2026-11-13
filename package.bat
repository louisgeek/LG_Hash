@echo off
rem Build, sign and package a release.
rem
rem   package.bat                 x64 only
rem   package.bat x64 arm64       both
rem
rem Steps: build.bat -> sign.bat -> dist\LG_Hash-<version>-<arch>.exe + SHA256SUMS.txt
rem Refuses to package an unsigned binary.
setlocal enabledelayedexpansion
cd /d "%~dp0"

set ARCHES=%*
if "%ARCHES%"=="" set ARCHES=x64

rem --- read the version from its single source (src\version.h) ---
rem build\version_gen.h is produced by tools\gen-version-header.ps1 (called by
rem build.bat). Its STRING macro is the full x.y.z.0 form; the file name wants
rem the short x.y.z form, so take the first three components.
set "VER="
set "GEN=build\version_gen.h"
if not exist "%GEN%" (
    echo [package] missing %GEN% - run build.bat first
    exit /b 1
)
for /f "tokens=3 delims= " %%v in ('findstr /c:"LG_HASH_VERSION_STRING" "%GEN%"') do set "VERFULL=%%v"
set "VERFULL=%VERFULL:"=%"
for /f "tokens=1,2,3 delims=." %%a in ("%VERFULL%") do set "VER=%%a.%%b.%%c"
if not defined VER (
    echo [package] cannot read version from %GEN%
    exit /b 1
)
echo [package] version %VER%

rem --- locate signtool (ships with the Windows SDK) ---
set SIGNTOOL=
for %%v in (10.0.26100.0 10.0.22621.0 10.0.20348.0 10.0.19041.0) do (
    if not defined SIGNTOOL if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\%%v\x64\signtool.exe" (
        set "SIGNTOOL=%ProgramFiles(x86)%\Windows Kits\10\bin\%%v\x64\signtool.exe"
    )
)

set DIST=dist
if exist "%DIST%" rd /s /q "%DIST%"
mkdir "%DIST%"

for %%a in (%ARCHES%) do (
    if /i "%%a"=="x64" (
        set "SRC=build"
        set "NAME=LG_Hash-%VER%-x64"
    ) else (
        set "SRC=build\%%a"
        set "NAME=LG_Hash-%VER%-%%a"
    )
    if not exist "!SRC!\LG_Hash.exe" (
        echo [package] missing !SRC!\LG_Hash.exe - run build.bat %%a first
        exit /b 1
    )

    rem unsigned binaries must never reach a release package
    if defined SIGNTOOL (
        "!SIGNTOOL!" verify /pa "!SRC!\LG_Hash.exe" >nul 2>&1
        if errorlevel 1 (
            echo [package] !SRC!\LG_Hash.exe is not signed - run sign.bat first
            exit /b 1
        )
    ) else (
        echo [package] warning: signtool not found, cannot verify signature
    )

    copy /y "!SRC!\LG_Hash.exe" "%DIST%\!NAME!.exe" >nul
    if errorlevel 1 exit /b 1
    echo [package] %DIST%\!NAME!.exe
)

copy /y README.md "%DIST%\README.md" >nul
rem README 引用了 screenshot\ 下的图，一并带上，否则分发包里是破图
if exist screenshot (
    if not exist "%DIST%\screenshot" mkdir "%DIST%\screenshot"
    copy /y screenshot\*.png "%DIST%\screenshot\" >nul
)

rem include checksums for post-distribution verification
powershell -NoProfile -Command ^
  "Get-ChildItem '%DIST%\*.exe' | ForEach-Object { $h=(Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(); '{0}  {1}' -f $h, $_.Name } | Set-Content -Encoding ASCII '%DIST%\SHA256SUMS.txt'"

echo.
echo PACKAGE OK
dir /b "%DIST%"
exit /b 0
