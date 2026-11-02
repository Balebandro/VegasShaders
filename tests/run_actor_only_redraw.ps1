# Builds tests\actor_only_redraw.cpp (32-bit) and runs it on this machine's D3D9; with -Dxvk <folder>, a second time with
# that folder's d3d9.dll next to the exe (DXVK, as the game may run).
param([string]$Dxvk = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -version '[16.0,17.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio 2019 C++ tools (v142) are required.' }
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;" + $env:PATH   # vcvarsall calls vswhere
$out = "$repo\build\actor-test"
New-Item -ItemType Directory -Force $out | Out-Null
cmd /c "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" x86 >nul && cl /nologo /O2 /EHsc `"$repo\tests\actor_only_redraw.cpp`" /Fe:`"$out\actor_only_redraw.exe`" /Fo:`"$out\\`" user32.lib"
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Remove-Item -Force -ErrorAction SilentlyContinue "$out\d3d9.dll"
Write-Output '--- system D3D9'
& "$out\actor_only_redraw.exe"
$failed = $LASTEXITCODE
if ($Dxvk) {
	Copy-Item -Force "$Dxvk\d3d9.dll" "$out\d3d9.dll"
	Write-Output "--- DXVK ($Dxvk\d3d9.dll)"
	& "$out\actor_only_redraw.exe"
	if ($LASTEXITCODE -ne 0) { $failed = $LASTEXITCODE }
	Remove-Item -Force "$out\d3d9.dll"
}
exit $failed
