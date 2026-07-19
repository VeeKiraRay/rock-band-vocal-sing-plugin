@echo off

rem scripts\deploy_plugin.bat - build the plugin and (re)install it into the system
rem VST3 folder via scripts\deploy_plugin.ps1 (closes REAPER around the copy and
rem restarts it after). Plain batch wrapper so it runs from a double-click without
rem an -ExecutionPolicy prompt. Arguments pass through: -Config Debug, -NoRestart.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy_plugin.ps1" %*
exit /b %ERRORLEVEL%
