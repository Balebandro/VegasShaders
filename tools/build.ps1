# NVR build, no Visual Studio command prompt needed. Double-click tools\build.bat (it runs this), or:
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 "D:\Games\Fallout New Vegas\Data"
# Builds VegasShaders.dll (Release, Win32) into build\optimized\. With a Data folder (argument, or the first line of
# tools\deploy-path.txt) it then copies the DLL (+ .pdb) to <Data>\nvse\Plugins\ and the shader sources to
# <Data>\Shaders\VegasShaders\ -- changed files only, nothing is ever deleted. An install that still has only the old
# <Data>\Shaders\NewVegasReloaded\ folder gets the shaders there (the DLL uses that folder until the new one exists), and an
# old NewVegasReloaded.dll in nvse\Plugins is renamed to NewVegasReloaded.dll.old (two copies of the plugin must not load).
# Needs Visual Studio 2019, or Visual Studio 2022 with "MSVC v142 - VS 2019 C++ x64/x86 build tools" installed.
param([string]$Deploy = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
if (-not $Deploy) {
    $pathFile = Join-Path $PSScriptRoot 'deploy-path.txt'
    if (Test-Path $pathFile) { $Deploy = (Get-Content $pathFile -First 1).Trim() }
}
Write-Host ''
Write-Host '=== NVR build ==='
Write-Host "Repo: $repo"

# --- Visual Studio with the v142 toolset ------------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found at `"$vswhere`". Is Visual Studio installed?" }
$vs = & $vswhere -latest -version '[16.0,17.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) {   # no VS 2019: a newer VS with the v142 toolset component also works
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.v142.x86.x64 -property installationPath | Select-Object -First 1
}
if (-not $vs) {
    Write-Host 'Visual Studio installs vswhere can see:'
    & $vswhere -all -property installationPath | ForEach-Object { Write-Host "  $_" }
    throw 'None of them has the C++ v142 toolset. Install Visual Studio 2019 with "Desktop development with C++", or in the VS 2022 installer (Individual components) tick "MSVC v142 - VS 2019 C++ x64/x86 build tools".'
}
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "MSBuild not found at `"$msbuild`"." }
Write-Host "Visual Studio: $vs"

# --- submodules (imgui, toml11) --------------------------------------------------------------------
if (-not (Test-Path "$repo\lib\imgui\imgui.h")) {
    Write-Host 'lib\imgui is empty: fetching the git submodules...'
    & git -C $repo submodule update --init --recursive
    if (-not (Test-Path "$repo\lib\imgui\imgui.h")) { throw "Could not fetch the submodules. Run 'git submodule update --init --recursive' in $repo by hand." }
}

# --- build -------------------------------------------------------------------------------------------
Write-Host ''
Write-Host 'Building VegasShaders.dll (Release, Win32)...'
& $msbuild "$repo\NewVegasReloaded\NewVegasReloaded.vcxproj" /p:Configuration=Release /p:Platform=Win32 "/p:SolutionDir=$repo\" "/p:OutDir=$repo\build\optimized\" "/p:IntDir=$repo\build\optimized-obj\" /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'BUILD FAILED. Scroll up to the first line that says "error" and send it along.' }
$dll = "$repo\build\optimized\VegasShaders.dll"
if (-not (Test-Path $dll)) { throw "The build reported success but `"$dll`" is missing." }
Write-Host ''
Write-Host "BUILD OK: $dll" -ForegroundColor Green

# --- optional deploy ----------------------------------------------------------------------------------
if (-not $Deploy) {
    Write-Host ''
    Write-Host 'To install: copy build\optimized\VegasShaders.dll to <Data>\nvse\Plugins\ and any changed .hlsl files from'
    Write-Host 'src\hlsl\NewVegas\Shaders\ to <Data>\Shaders\VegasShaders\Shaders\ (Effects likewise).'
    Write-Host 'Or run:  tools\build.bat "path\to\Fallout New Vegas\Data"   (or put that path in tools\deploy-path.txt)'
    exit 0
}
if (-not (Test-Path "$Deploy\nvse\Plugins")) { throw "`"$Deploy\nvse\Plugins`" does not exist. Give the game's Data folder (the one with nvse\Plugins in it)." }
Write-Host ''
Write-Host "Copying to $Deploy ..."
Copy-Item $dll "$Deploy\nvse\Plugins\" -Force
$pdb = "$repo\build\optimized\VegasShaders.pdb"
if (Test-Path $pdb) { Copy-Item $pdb "$Deploy\nvse\Plugins\" -Force }
$oldDll = "$Deploy\nvse\Plugins\NewVegasReloaded.dll"
if (Test-Path $oldDll) {
    Move-Item $oldDll "$oldDll.old" -Force
    Write-Host 'Renamed the old nvse\Plugins\NewVegasReloaded.dll to NewVegasReloaded.dll.old (VegasShaders.dll replaces it).'
}
# shaders: the folder the DLL will use (the new name, unless only the old one exists); changed files only, nothing deleted
# (no /MIR); robocopy exit codes under 8 mean success
$shaderRoot = 'Shaders\VegasShaders'
if (-not (Test-Path "$Deploy\Shaders\VegasShaders") -and (Test-Path "$Deploy\Shaders\NewVegasReloaded")) { $shaderRoot = 'Shaders\NewVegasReloaded' }
& robocopy "$repo\src\hlsl\NewVegas\Shaders" "$Deploy\$shaderRoot\Shaders" /E /NJH /NJS /NDL /NP /NFL | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Copying the shaders failed (game running, or folder read-only?).' }
& robocopy "$repo\src\hlsl\NewVegas\Effects" "$Deploy\$shaderRoot\Effects" /E /NJH /NJS /NDL /NP /NFL | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Copying the effects failed (game running, or folder read-only?).' }
Write-Host "DEPLOYED: DLL to nvse\Plugins\, shaders to $shaderRoot\." -ForegroundColor Green
Write-Host "If a shader change does not show in game, delete the folder $shaderRoot\Shaders\Cache\ once."
exit 0
