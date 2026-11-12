@echo off
rem Register for the current user; no admin required.
rem The DLL path is recorded, so re-register after moving the DLL.
regsvr32 "%~dp0build\LG_Hash.dll"