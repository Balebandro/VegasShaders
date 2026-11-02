@echo off

@REM Arguments passed: Project name to build, and deploy location
SET "PROJECT=%~1"
SET "DEPLOY_LOCATION=%~2"

@REM Check arguments validity
if "%PROJECT%" NEQ "NewVegasReloaded" (
    echo "Project %PROJECT% invalid. Use 'NewVegasReloaded'."
    PAUSE
    exit /b %ERRORLEVEL%
)

@REM build project
msbuild -target:%PROJECT% /property:Configuration=Release /property:Platform=x86 -m

if %ERRORLEVEL% NEQ 0 (
    echo Build has failed. Deploy aborted.
    PAUSE
    exit /b %ERRORLEVEL%
)

@REM Cleanup in case of previous build
rmdir "build" /s /q

if "%PROJECT%" == "NewVegasReloaded" (
    SET "FOLDER=nvse"
)

@REM 2026-11-02 Vegas Shaders: the project is still called NewVegasReloaded; what it installs uses the new name
SET "PRODUCT=VegasShaders"

@REM Copy dll & pdb
xcopy "%PROJECT%\Release\%PRODUCT%.dll" "build\%PROJECT%\%FOLDER%\Plugins\" /y /d
xcopy "%PROJECT%\Release\%PRODUCT%.pdb" "build\%PROJECT%\%FOLDER%\Plugins\" /y /d

@REM Copy config (the defaults also as the starting settings file; the legacy .config keeps its old name)
xcopy "resource\%PROJECT%.dll.config" "build\%PROJECT%\%FOLDER%\Plugins\" /y /d
xcopy "resource\%PRODUCT%.defaults.toml" "build\%PROJECT%\%FOLDER%\Plugins\" /y /d
xcopy "resource\%PRODUCT%.materials.txt" "build\%PROJECT%\%FOLDER%\Plugins\" /y /d
xcopy "resource\%PRODUCT%.maxquality.toml" "build\%PROJECT%\%FOLDER%\Plugins\" /y /d
echo F|xcopy "resource\%PRODUCT%.defaults.toml" "build\%PROJECT%\%FOLDER%\Plugins\%PRODUCT%.toml" /y /d

@REM Copy Shaders
set GAME=%PROJECT:Reloaded=""%
robocopy /mir .\src\hlsl\%GAME%\Shaders .\build\%PROJECT%\Shaders\%PRODUCT%\Shaders
robocopy /mir .\src\hlsl\%GAME%\Effects .\build\%PROJECT%\Shaders\%PRODUCT%\Effects
@REM Compute: the SPIR-V the NVR DXVK paths load (NewVegas only; unused on native D3D9 and stock DXVK)
if exist .\src\hlsl\%GAME%\Compute robocopy /mir .\src\hlsl\%GAME%\Compute .\build\%PROJECT%\Shaders\%PRODUCT%\Compute

@REM Copy Textures
robocopy /mir .\resource\Textures\ .\build\%PROJECT%\Textures\

@REM Copy to deploy if 2nd arg was passed
if "%DEPLOY_LOCATION%" NEQ "" (
    echo deploying to "%DEPLOY_LOCATION%"
    robocopy /mir "build\%PROJECT%\." "%DEPLOY_LOCATION%"
) else (
    echo "No deploy stage."
)

PAUSE

@REM handle Robocopy weird error codes
if (%ERRORLEVEL% LSS 8) exit /b 0
exit /b %ERRORLEVEL%
