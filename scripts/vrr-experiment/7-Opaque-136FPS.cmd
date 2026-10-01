@echo off
cd /d "%~dp0"
>"%~dp0vrr-experiment-mode.txt" echo 7
set "MAGPIE_VRR_EXPERIMENT=7"
echo Close every running Magpie instance before using this launcher.
echo Mode 7: Opaque native input, fixed 136 FPS DLSSFG output.
echo With FG x2, limit your game to 68 FPS. Scale shortcut starts and stops.
start "" "%~dp0Magpie.exe"
