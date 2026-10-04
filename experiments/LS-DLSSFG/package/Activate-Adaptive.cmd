@echo off
echo Native frame generation in Lossless Scaling must be OFF.
echo Adaptive x2 output follows the incoming real-frame cadence. Restart LS.
copy /y "%~dp0profiles\adaptive.ini" "%~dp0LS_DLSSFG.ini"
pause
