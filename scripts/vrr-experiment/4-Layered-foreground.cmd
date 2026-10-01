@echo off
cd /d "%~dp0"
echo 4>"%~dp0vrr-experiment-mode.txt"
set "MAGPIE_VRR_EXPERIMENT=4"
echo Close every running Magpie instance before using this launcher.
echo Mode 4: Layered-foreground. Use your usual Scale shortcut to start and stop.
start "" "%~dp0Magpie.exe"
