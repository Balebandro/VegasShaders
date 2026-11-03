$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -version '[16.0,17.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio 2019 C++ tools (v142) are required.' }
# UNOFFICIAL, test only: the frame replayer (tests\frame_replay.cpp) -> build\frame-replay\frame_replay.exe
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$repo\tests\frame_replay.vcxproj" /p:Configuration=Release /p:Platform=Win32 /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'frame_replay build failed.' }
