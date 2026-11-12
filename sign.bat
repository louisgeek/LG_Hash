@echo off
rem Sign build outputs with a code-signing certificate.
rem
rem   sign.bat                      sign with the default self-signed dev cert
rem   sign.bat /thumb <SHA1>        sign with a specific cert thumbprint
rem   sign.bat /pfx <file> [/pw <p>] sign with a PFX file
rem   sign.bat /timestamp <url>     add an RFC3161 timestamp
rem
rem SHA-256 for both the file digest and the certificate hash.
setlocal
cd /d "%~dp0"

set SIGNTOOL=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
rem signtool ships with the Windows SDK, not with MSVC
for %%v in (10.0.26100.0 10.0.22621.0 10.0.20348.0 10.0.19041.0) do (
    if not defined SIGNTOOL if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\%%v\x64\signtool.exe" (
        set "SIGNTOOL=%ProgramFiles(x86)%\Windows Kits\10\bin\%%v\x64\signtool.exe"
    )
)
if not defined SIGNTOOL (
    echo signtool.exe not found. Install the Windows SDK signing tools.
    exit /b 1
)

set THUMB=
set PFX=
set PFXPW=
set TIMESTAMP=http://timestamp.digicert.com

:parse
if "%~1"=="" goto ready
if /i "%~1"=="/thumb" (
    set "THUMB=%~2"
    shift & shift & goto parse
)
if /i "%~1"=="/pfx" (
    set "PFX=%~2"
    shift & shift & goto parse
)
if /i "%~1"=="/pw" (
    set "PFXPW=%~2"
    shift & shift & goto parse
)
if /i "%~1"=="/timestamp" (
    set "TIMESTAMP=%~2"
    shift & shift & goto parse
)
echo unknown option: %~1
exit /b 1

:ready
rem Default to the development certificate created by make-dev-cert.ps1
if not defined THUMB if not defined PFX (
    if exist "build\devcert-thumbprint.txt" (
        set /p THUMB=<"build\devcert-thumbprint.txt"
    )
)
if defined THUMB set "THUMB=%THUMB: =%"

set FILES=build\LG_Hash.dll build\LG_Hash.exe
if exist build\arm64\LG_Hash.dll set FILES=%FILES% build\arm64\LG_Hash.dll build\arm64\LG_Hash.exe

set FAILED=0
for %%f in (%FILES%) do (
    if exist "%%f" (
        echo [sign] %%f
        if defined PFX (
            "%SIGNTOOL%" sign /f "%PFX%" /p "%PFXPW%" /fd SHA256 /tr "%TIMESTAMP%" /td SHA256 "%%f"
        ) else (
            "%SIGNTOOL%" sign /sha1 "%THUMB%" /fd SHA256 /tr "%TIMESTAMP%" /td SHA256 "%%f"
        )
        if errorlevel 1 (
            echo [sign] FAILED: %%f
            set FAILED=1
        )
    )
)

if "%FAILED%"=="1" (
    echo.
    echo SIGNING FAILED
    exit /b 1
)
echo.
echo SIGN OK
exit /b 0
