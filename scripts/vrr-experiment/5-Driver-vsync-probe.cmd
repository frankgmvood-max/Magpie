@echo off
cd /d "%~dp0"
set "MAGPIE_VRR_EXPERIMENT=5"
echo Close every running Magpie instance before using this launcher.
echo Mode 5: Driver-vsync-probe. Use your usual Scale shortcut to start and stop.
start "" "%~dp0Magpie.exe"
