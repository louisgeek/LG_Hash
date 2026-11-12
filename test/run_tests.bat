@echo off
rem Build test helpers and run algorithm / language / edge-case checks.
rem For the UI-level checks run test\verify_tab.ps1 and test\verify_export.ps1.
setlocal
cd /d "%~dp0.."

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio not found
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

set OUT=build\tests
if not exist %OUT% mkdir %OUT%
set FLAGS=/nologo /std:c++17 /O2 /W3 /EHsc /utf-8 /DUNICODE /D_UNICODE /MT
set LIBS=bcrypt.lib advapi32.lib user32.lib

echo [1/6] build hashtest
cl %FLAGS% test\hashtest.cpp src\HashEngine.cpp src\Lang.cpp /Fo:%OUT%\ /Fe:%OUT%\hashtest.exe /link %LIBS%
if errorlevel 1 goto fail

echo [2/6] build edge-case test
cl %FLAGS% test\hashtest2.cpp src\HashEngine.cpp src\Lang.cpp /Fo:%OUT%\ /Fe:%OUT%\hashtest2.exe /link %LIBS%
if errorlevel 1 goto fail

echo [3/6] build export-guard test
cl %FLAGS% test\guardtest.cpp src\PathUtil.cpp /Fo:%OUT%\ /Fe:%OUT%\guardtest.exe /link %LIBS% shlwapi.lib
if errorlevel 1 goto fail

echo [4/6] build language test
cl %FLAGS% test\langtest.cpp src\Lang.cpp /Fo:%OUT%\ /Fe:%OUT%\langtest.exe /link %LIBS%
if errorlevel 1 goto fail

echo [4b/6] build multi-select job test
cl %FLAGS% test\jobtest.cpp src\HashJob.cpp src\HashEngine.cpp src\Lang.cpp /Fo:%OUT%\ /Fe:%OUT%\jobtest.exe /link %LIBS%
if errorlevel 1 goto fail

echo [5/6] compare digests with reference
python test\make_reference.py
if errorlevel 1 goto fail
python test\compare_hashes.py
if errorlevel 1 goto fail

echo [6/6] edge cases, guard, language and multi-select job
%OUT%\hashtest2.exe
if errorlevel 1 goto fail
%OUT%\guardtest.exe
if errorlevel 1 goto fail
%OUT%\langtest.exe
if errorlevel 1 goto fail
%OUT%\jobtest.exe
if errorlevel 1 goto fail

echo.
echo ALL CONSOLE TESTS PASSED
exit /b 0

:fail
echo.
echo TESTS FAILED
exit /b 1
