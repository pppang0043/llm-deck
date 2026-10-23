@echo off
rem LLM Deck orca shim
rem The orca command for terminals. install.ps1 puts it at %USERPROFILE%\.local\bin\orca.cmd and adds that folder to PATH.
rem It looks for ORCA on every call, so reinstalling or moving ORCA does not break it.
setlocal
set "ORCA_BIN="
for %%D in ("%ORCA_CLI_BIN_DIR%" "%LOCALAPPDATA%\Programs\orca\resources\bin" "%ProgramFiles%\Orca\resources\bin") do (
  if not defined ORCA_BIN if exist "%%~D\orca.exe" set "ORCA_BIN=%%~D\orca.exe"
)
if not defined ORCA_BIN (
  echo orca: ORCA is not installed or could not be found. 1>&2
  exit /b 127
)
"%ORCA_BIN%" %*
