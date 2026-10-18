@echo off
rem Opens the helper settings (app\config.toml) in Notepad. Run install.cmd again after saving.
set "CFG=%~dp0app\config.toml"
if not exist "%CFG%" set "CFG=%~dp0..\config.toml"
start "" notepad "%CFG%"
