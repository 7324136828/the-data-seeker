@echo off
call "%~dp0native-windows\build.bat" %*
exit /b %errorlevel%
