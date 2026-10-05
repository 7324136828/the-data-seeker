@echo off
setlocal
powershell.exe -NoLogo -NoProfile -File "%~dp0run.ps1" %*
exit /b %errorlevel%
