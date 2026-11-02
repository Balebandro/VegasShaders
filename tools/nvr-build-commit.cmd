@echo off
rem UNOFFICIAL 2026-11-01: NewVegasReloaded.vcxproj's pre-build step. Writes <IntDir>\NvrBuildCommit.h with the git commit and branch
rem of the tree being built, for the log's "SESSION build" line (src/core/SessionLog.cpp). Without git (or outside a checkout) the
rem commit is "unknown". The header is only rewritten when it changes, so an unchanged tree does not recompile SessionLog.cpp.
rem Usage: nvr-build-commit.cmd <repo folder> <IntDir>
setlocal
set "REPO=%~1"
set "OUT=%~2"
if not exist "%OUT%" mkdir "%OUT%" 2>nul
set "COMMIT=unknown"
set "BRANCH="
set "DIRTY="
for /f "delims=" %%i in ('git -C "%REPO%" rev-parse --short^=8 HEAD 2^>nul') do set "COMMIT=%%i"
for /f "delims=" %%i in ('git -C "%REPO%" rev-parse --abbrev-ref HEAD 2^>nul') do set "BRANCH= %%i"
for /f "delims=" %%i in ('git -C "%REPO%" status --porcelain --untracked-files^=no --ignore-submodules 2^>nul') do set "DIRTY=-dirty"
> "%OUT%NvrBuildCommit.h.new" echo #define NVR_BUILD_COMMIT "%COMMIT%%DIRTY%%BRANCH%"
fc /b "%OUT%NvrBuildCommit.h.new" "%OUT%NvrBuildCommit.h" >nul 2>&1 || copy /y "%OUT%NvrBuildCommit.h.new" "%OUT%NvrBuildCommit.h" >nul
del "%OUT%NvrBuildCommit.h.new" 2>nul
exit /b 0
