# BloodFlow simulation without the game (tests/blood_preview.cpp, terrain mode): builds the preview, runs the tree's
# BloodFlow.fx.hlsl on gentle desert ground, prints the per-splat check (PASS / FAIL) and opens the time-lapse viewer.
#   powershell -ExecutionPolicy Bypass -File tools\blood-sim.ps1 [-Out .\blood-sim-out] [-NoOpen]
param([string]$Out = '.\blood-sim-out', [switch]$NoOpen)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -version '[16.0,17.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio 2019 C++ tools (v142) are required.' }
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$repo\tests\blood_preview.vcxproj" /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'Blood preview build failed.' }
New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item "$repo\tests\blood_sim_viewer.html" "$Out\index.html" -Force
& "$repo\build\blood-test\blood_preview.exe" "$repo\src\hlsl\NewVegas\Effects\BloodFlow.fx.hlsl" $Out terrain
$result = $LASTEXITCODE
if (-not $NoOpen) { Start-Process "$Out\index.html" }
exit $result
