@echo off
setlocal
if not exist "%~dp0Settings.ps1" (
    echo ERROR: Settings.ps1 is missing. Extract the complete package first.
    echo Keep Settings.cmd beside Settings.ps1.
    pause
    exit /b 2
)
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Settings.ps1"
set "LS_SETTINGS_EXIT=%ERRORLEVEL%"
if "%LS_SETTINGS_EXIT%"=="0" exit /b 0
echo.
echo Native DLSS settings failed to start or exited with an error.
echo Exit code: %LS_SETTINGS_EXIT%
echo Copy the error shown above before closing this window.
pause
exit /b %LS_SETTINGS_EXIT%
