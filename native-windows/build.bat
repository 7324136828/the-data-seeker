@echo off
setlocal
powershell.exe -NoLogo -NoProfile -File "%~dp0scripts\build.ps1" %*
exit /b %errorlevel%
