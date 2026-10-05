@echo off
call "%~dp0native-windows\package.bat" %*
exit /b %errorlevel%
