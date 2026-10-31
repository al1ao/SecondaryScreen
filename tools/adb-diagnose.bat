@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0adb-diagnose.ps1" %*
echo.
echo Report saved next to this script (adb-diagnosis.txt). Review it before sharing.
pause
