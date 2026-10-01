@echo off
cd /d "%~dp0"
echo 3>"%~dp0vrr-experiment-mode.txt"
set "MAGPIE_VRR_EXPERIMENT=3"
echo Close every running Magpie instance before using this launcher.
echo Mode 3: Maximum-opaque-foreground. Use your usual Scale shortcut to start and stop.
start "" "%~dp0Magpie.exe"
