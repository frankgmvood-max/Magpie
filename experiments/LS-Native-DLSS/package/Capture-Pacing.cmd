@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Capture-Pacing.ps1" %*
pause
