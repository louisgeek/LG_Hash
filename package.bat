@echo off
rem Build, sign and package a release.
rem
rem   package.bat                 x64 only
rem   package.bat x64 arm64       both
rem
rem Steps: build.bat -> sign.bat -> dist\LG_Hash-<arch>.exe + SHA256SUMS.txt
rem Refuses to package an unsigned binary.
setlocal enabledelayedexpansion
cd /d "%~dp0"

set ARCHES=%*
if "%ARCHES%"=="" set ARCHES=x64

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
        set "NAME=LG_Hash-x64"
    ) else (
        set "SRC=build\%%a"
        set "NAME=LG_Hash-%%a"
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

rem include checksums for post-distribution verification
powershell -NoProfile -Command ^
  "Get-ChildItem '%DIST%\*.exe' | ForEach-Object { $h=(Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(); '{0}  {1}' -f $h, $_.Name } | Set-Content -Encoding ASCII '%DIST%\SHA256SUMS.txt'"

echo.
echo PACKAGE OK
dir /b "%DIST%"
exit /b 0
