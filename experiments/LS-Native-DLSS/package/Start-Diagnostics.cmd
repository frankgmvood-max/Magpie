@echo off
powershell.exe -STA -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-Diagnostics.ps1"
pause
