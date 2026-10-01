@echo off
cd /d "%~dp0"
>"%~dp0vrr-experiment-mode.txt" echo 2
set "MAGPIE_VRR_EXPERIMENT=2"
echo Close every running Magpie instance before using this launcher.
echo Mode 2: Opaque-source-focus. Use your usual Scale shortcut to start and stop.
start "" "%~dp0Magpie.exe"
