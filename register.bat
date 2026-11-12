@echo off
rem 注册到当前用户，无需管理员权限。DLL 路径会被记录，移动 DLL 后需重新注册。
regsvr32 "%~dp0build\HashPage.dll"