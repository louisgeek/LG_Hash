@echo off
rem Build, sign and package a release.
rem
rem   package.bat                 x64 only
rem   package.bat x64 arm64       both
rem
rem Steps: build.bat -> sign.bat -> dist\HashPage-<arch>.exe + SHA256SUMS.txt
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
        set "NAME=HashPage-x64"
    ) else (
        set "SRC=build\%%a"
        set "NAME=HashPage-%%a"
    )
    if not exist "!SRC!\HashPage.exe" (
        echo [package] missing !SRC!\HashPage.exe - run build.bat %%a first
        exit /b 1
    )

    rem 未签名的二进制不应进入发布包
    if defined SIGNTOOL (
        "!SIGNTOOL!" verify /pa "!SRC!\HashPage.exe" >nul 2>&1
        if errorlevel 1 (
            echo [package] !SRC!\HashPage.exe is not signed - run sign.bat first
            exit /b 1
        )
    ) else (
        echo [package] warning: signtool not found, cannot verify signature
    )

    copy /y "!SRC!\HashPage.exe" "%DIST%\!NAME!.exe" >nul
    if errorlevel 1 exit /b 1
    echo [package] %DIST%\!NAME!.exe
)

copy /y README.md "%DIST%\README.md" >nul

rem 附带校验和，便于分发后核对
powershell -NoProfile -Command ^
  "Get-ChildItem '%DIST%\*.exe' | ForEach-Object { $h=(Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(); '{0}  {1}' -f $h, $_.Name } | Set-Content -Encoding ASCII '%DIST%\SHA256SUMS.txt'"

echo.
echo PACKAGE OK
dir /b "%DIST%"
exit /b 0
