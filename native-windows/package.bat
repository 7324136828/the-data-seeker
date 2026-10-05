@echo off
setlocal
powershell.exe -NoLogo -NoProfile -File "%~dp0scripts\package.ps1" %*
exit /b %errorlevel%
