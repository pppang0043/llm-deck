@echo off
chcp 65001 >nul
rem In the zip this file is at the top and install.ps1 is in app\windows
set "PS1=%~dp0install.ps1"
if not exist "%PS1%" set "PS1=%~dp0app\windows\install.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
pause
