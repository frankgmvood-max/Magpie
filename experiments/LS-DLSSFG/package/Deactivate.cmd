@echo off
copy /y "%~dp0profiles\off.ini" "%~dp0LS_DLSSFG.ini"
echo DLSS FG deactivated. Restart LS. You can enable native LSFG again.
pause
