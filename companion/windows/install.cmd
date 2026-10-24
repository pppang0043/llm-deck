@echo off
chcp 65001 >nul
setlocal
rem Installs the LLM Deck helper with the Python installed on this PC (no PowerShell, no new exe).
rem In the zip this file is at the top and install.py is in app\windows. "install.cmd --uninstall" removes it.
set "SCRIPT=%~dp0install.py"
set "APP=%~dp0.."
if not exist "%SCRIPT%" set "SCRIPT=%~dp0app\windows\install.py"
if exist "%~dp0app\main.py" set "APP=%~dp0app"
set "PY="
rem Python prints paths (Korean user names too) in UTF-8 to match chcp 65001
set "PYTHONIOENCODING=utf-8"

rem 1) The Python chosen by the last install
if exist "%APP%\python.txt" set /p PY=<"%APP%\python.txt"
if defined PY if exist "%PY%" goto found
set "PY="
rem 2) The Python launcher (py.exe) from the python.org installer
for /f "delims=" %%P in ('py -3 -c "import sys; print(sys.executable)" 2^>nul') do set "PY=%%P"
if defined PY if exist "%PY%" goto found
set "PY="
rem 3) python on PATH (the Microsoft Store shortcut prints nothing when Python is not installed)
for /f "delims=" %%P in ('python -c "import sys; print(sys.executable)" 2^>nul') do set "PY=%%P"
if defined PY if exist "%PY%" goto found
set "PY="
rem 4) Where the python.org installer records itself
for %%R in (HKCU HKLM) do (
  for /f "tokens=2,*" %%A in ('reg query "%%R\Software\Python\PythonCore" /s /v ExecutablePath 2^>nul ^| findstr /i "ExecutablePath"') do (
    if not defined PY if exist "%%B" set "PY=%%B"
  )
)
if defined PY goto found
rem 5) The usual install folders
for /d %%D in ("%LOCALAPPDATA%\Programs\Python\Python3*" "%ProgramFiles%\Python3*") do (
  if not defined PY if exist "%%~D\python.exe" set "PY=%%~D\python.exe"
)
if defined PY goto found

echo Python 을 찾지 못했습니다. 64비트 Python 3.11 ~ 3.14 를 설치한 뒤 다시 실행하세요.
echo   - 회사 PC: 회사 소프트웨어 센터(포털)에서 Python 3.12 또는 3.13
echo   - 개인 PC: https://www.python.org/downloads/windows/ 의 "Windows installer (64-bit)"
echo 자세한 것은 INSTALL.txt 의 "1. 준비" 를 보세요.
pause
exit /b 1

:found
rem This Python only runs the installer. The installer picks the Python that runs the helper.
"%PY%" "%SCRIPT%" %*
pause
