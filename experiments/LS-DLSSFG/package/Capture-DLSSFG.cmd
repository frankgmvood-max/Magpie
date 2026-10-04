@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Capture-Pacing.ps1" -Mode DLSSFG
pause
