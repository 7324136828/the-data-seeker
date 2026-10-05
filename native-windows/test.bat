@echo off
setlocal
powershell.exe -NoLogo -NoProfile -File "%~dp0scripts\test.ps1" %*
exit /b %errorlevel%
