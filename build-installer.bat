@echo off
REM Double-click wrapper: builds OptiScan Release x64 and packages the Inno
REM Setup installer by running build-installer.ps1. See that file for details.
REM Remember to bump the version in installer\OptiScan.iss and OptiScan.rc first.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-installer.ps1"
echo.
pause
