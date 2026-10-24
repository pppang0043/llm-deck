@echo off
chcp 65001 >nul
setlocal
rem Opens the mirror window of the running helper (the tray app opens it, or the browser if there is no app)
set "APP=%~dp0.."
if exist "%~dp0app\main.py" set "APP=%~dp0app"
set "PY="
if exist "%APP%\python.txt" set /p PY=<"%APP%\python.txt"
if not defined PY goto missing
if not exist "%PY%" goto missing
cd /d "%APP%"
"%PY%" main.py --mirror
if errorlevel 1 pause
exit /b

:missing
echo 먼저 install.cmd 로 설치하세요.
pause
exit /b 1
