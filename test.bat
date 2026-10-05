@echo off
call "%~dp0native-windows\test.bat" %*
exit /b %errorlevel%
