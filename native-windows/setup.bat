@echo off
setlocal
powershell.exe -NoLogo -NoProfile -File "%~dp0setup.ps1" %*
exit /b %errorlevel%
