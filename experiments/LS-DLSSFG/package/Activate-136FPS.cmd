@echo off
echo Native frame generation in Lossless Scaling must be OFF.
echo Set the game's real-frame limit to 68 FPS. Restart LS after activation.
copy /y "%~dp0profiles\136.ini" "%~dp0LS_DLSSFG.ini"
pause
