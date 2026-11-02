# Checks a package folder against this checkout before it is zipped.
#
# Since P40 a package carries the COMPLETE shader folders of this repo (not only the files changed since
# the fork we build from), so a tester's older shader files can never mix with ours. Found 2026-09-29
# from a tester's log: Effects\Includes\Normals.hlsl had been missing from every overlay package since
# P8, so on an install without it SunShadows and VolumetricFog failed to compile. Since P45 it also
# carries all of NVR's textures (user decision), so the zip is a complete install on its own; a user
# who installed an older zip without NVR underneath crashed at startup.
#
# Checked: every shader and texture file tracked in the repo is in the package and current (text
# files line endings aside), the package holds no other shader or texture files, and the DLL and
# defaults file are the current ones.
#
#   powershell -ExecutionPolicy Bypass -File tools\check-package.ps1 ..\..\outputs\p45-package
param([Parameter(Mandatory = $true)][string]$Package)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$pkg = (Resolve-Path $Package).Path
$folders = [ordered]@{
	'src/hlsl/NewVegas/Effects/' = 'Shaders\VegasShaders\Effects\'
	'src/hlsl/NewVegas/Shaders/' = 'Shaders\VegasShaders\Shaders\'
	'src/hlsl/NewVegas/Compute/' = 'Shaders\VegasShaders\Compute\'	# the NVR DXVK SPIR-V (2026-10-25)
	'resource/Textures/' = 'Textures\'
}
function Text([string]$path) { [IO.File]::ReadAllText($path).Replace("`r`n", "`n") }
function Same([string]$a, [string]$b) {
	if ($a -like '*.hlsl' -or $a -like '*.disabled') { return (Text $a) -eq (Text $b) }
	return (Get-FileHash $a).Hash -eq (Get-FileHash $b).Hash
}
$problems = 0
$checked = 0

foreach ($entry in $folders.GetEnumerator()) {
	# 1. Every tracked file of the folder is shipped and current.
	$tracked = & git -C $repo -c safe.directory=* ls-files -- $entry.Key
	if ($LASTEXITCODE -ne 0) { throw "git ls-files failed" }
	foreach ($file in $tracked) {
		$checked++
		$target = Join-Path $pkg ($entry.Value + $file.Substring($entry.Key.Length).Replace('/', '\'))
		if (!(Test-Path $target)) { Write-Host "MISSING  $file"; $problems++ }
		elseif (!(Same $target (Join-Path $repo $file))) { Write-Host "STALE    $file"; $problems++ }
	}
	# 2. Nothing else is in it (a leftover file would be installed as if it were ours).
	$dir = Join-Path $pkg $entry.Value
	if (!(Test-Path $dir)) { continue }
	$known = @{}
	foreach ($file in $tracked) { $known[$file.Substring($entry.Key.Length).Replace('/', '\').ToLowerInvariant()] = $true }
	foreach ($item in Get-ChildItem $dir -Recurse -File) {
		$relative = $item.FullName.Substring($dir.Length)
		if ($relative.ToLowerInvariant().StartsWith('cache\')) { continue }	# the shipped shader cache, checked below
		if (!$known.ContainsKey($relative.ToLowerInvariant())) { Write-Host "EXTRA    $($item.FullName)"; $problems++ }
	}
	$cache = Join-Path $dir 'Cache'
	if (Test-Path $cache) {
		foreach ($item in Get-ChildItem $cache -Recurse -File -Filter '*.hlsl') {
			$compiled = $item.FullName.Substring(0, $item.FullName.Length - 5)
			if (!(Test-Path $compiled)) { Write-Host "CACHE    no compiled file for $($item.FullName)"; $problems++ }
			if (Select-String -Path $item.FullName -Pattern '#line \d+ "[A-Za-z]:' -Quiet) { Write-Host "CACHE    install path in $($item.FullName)"; $problems++ }
		}
	}
}

# 3. Only what players need at the top level (no working notes).
$allowed = 'COPYING.txt', 'License.md', 'README.md', 'NVSE', 'Shaders', 'Textures', 'Meshes', 'GunFX-README.txt', "Optional - Author's settings", "Optional - macintroll's settings (D3D9)"
foreach ($item in Get-ChildItem $pkg) {
	if ($allowed -notcontains $item.Name) { Write-Host "EXTRA    $($item.FullName)"; $problems++ }
}
foreach ($item in Get-ChildItem (Join-Path $pkg 'NVSE\Plugins')) {
	if ('VegasShaders.dll', 'VegasShaders.pdb', 'VegasShaders.defaults.toml', 'VegasShaders.materials.txt', 'VegasShaders.maxquality.toml', 'GunFX.dll', 'GunFX.pdb', 'GunFX.ini.defaults', 'NvrRt' -notcontains $item.Name) { Write-Host "EXTRA    $($item.FullName)"; $problems++ }
}
foreach ($file in 'COPYING.txt', 'License.md', 'README.md') {
	if (!(Test-Path (Join-Path $pkg $file))) { Write-Host "MISSING  $file"; $problems++ }
	elseif ((Text (Join-Path $pkg $file)) -ne (Text (Join-Path $repo $file))) { Write-Host "STALE    $file"; $problems++ }
}

# 4. The DLL is the latest local build and the defaults file is the repo's.
$plugins = Join-Path $pkg 'NVSE\Plugins'
if ((Get-FileHash (Join-Path $plugins 'VegasShaders.dll')).Hash -ne (Get-FileHash (Join-Path $repo 'build\optimized\VegasShaders.dll')).Hash) {
	Write-Host "STALE    NVSE\Plugins\VegasShaders.dll (not build\optimized's)"; $problems++
}
if ((Text (Join-Path $plugins 'VegasShaders.defaults.toml')) -ne (Text (Join-Path $repo 'resource\VegasShaders.defaults.toml'))) {
	Write-Host "STALE    NVSE\Plugins\VegasShaders.defaults.toml"; $problems++
}
if ((Text (Join-Path $plugins 'VegasShaders.materials.txt')) -ne (Text (Join-Path $repo 'resource\VegasShaders.materials.txt'))) {
	Write-Host "STALE    NVSE\Plugins\VegasShaders.materials.txt"; $problems++
}
if ((Text (Join-Path $plugins 'VegasShaders.maxquality.toml')) -ne (Text (Join-Path $repo 'resource\VegasShaders.maxquality.toml'))) {
	Write-Host "STALE    NVSE\Plugins\VegasShaders.maxquality.toml"; $problems++
}
# P80 update 2: the optional author preset (a settings file to copy by hand) holds nothing else.
$optional = Join-Path $pkg "Optional - Author's settings"
if (Test-Path $optional) {
	foreach ($item in Get-ChildItem $optional -Recurse -File) {
		$relative = $item.FullName.Substring($optional.Length + 1)
		if ('NVSE\Plugins\VegasShaders.toml', "README - author's settings.txt" -notcontains $relative) { Write-Host "EXTRA    $($item.FullName)"; $problems++ }
	}
}

# 5. GunFX (since P67): the plugin is the latest local build; its defaults, effect meshes and guide are the repo's.
# Never an active GunFX.ini (it would overwrite players' settings).
if ((Get-FileHash (Join-Path $plugins 'GunFX.dll')).Hash -ne (Get-FileHash (Join-Path $repo 'GunFX\build\GunFX.dll')).Hash) {
	Write-Host "STALE    NVSE\Plugins\GunFX.dll (not GunFX\build's)"; $problems++
}
if ((Text (Join-Path $plugins 'GunFX.ini.defaults')) -ne (Text (Join-Path $repo 'GunFX\config\GunFX.ini.defaults'))) { Write-Host "STALE    GunFX.ini.defaults"; $problems++ }
if (Test-Path (Join-Path $plugins 'GunFX.ini')) { Write-Host "EXTRA    NVSE\Plugins\GunFX.ini (players' settings must not be shipped)"; $problems++ }
if ((Text (Join-Path $pkg 'GunFX-README.txt')) -ne (Text (Join-Path $repo 'docs\GUNFX.md'))) { Write-Host "STALE    GunFX-README.txt"; $problems++ }
$meshes = & git -C $repo -c safe.directory=* ls-files -- 'GunFX/assets/Meshes/'
foreach ($file in $meshes) {
	$target = Join-Path $pkg ('Meshes\' + $file.Substring('GunFX/assets/Meshes/'.Length).Replace('/', '\'))
	if (!(Test-Path $target)) { Write-Host "MISSING  $file"; $problems++ }
	elseif ((Get-FileHash $target).Hash -ne (Get-FileHash (Join-Path $repo $file)).Hash) { Write-Host "STALE    $file"; $problems++ }
}
if ((Get-ChildItem (Join-Path $pkg 'Meshes') -Recurse -File).Count -ne @($meshes).Count) { Write-Host "EXTRA    files in Meshes"; $problems++ }

if ($problems) { throw "$problems package problem(s) in $pkg" }
Write-Host "Package OK: $pkg ($checked shader/texture files, DLL and defaults current)"
