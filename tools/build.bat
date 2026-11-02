@echo off
REM NVR build: double-click. All the work is in build.ps1 next to this file (batch cannot cope with "(x86)" in paths).
REM Optional argument: the game's Data folder, to install the DLL and shaders after the build.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
echo.
pause
