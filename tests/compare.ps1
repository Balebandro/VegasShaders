# Side-by-side sheet of two cloud_preview output folders (tests\out\<left>, tests\out\<right>), the top 600 rows of each view.
#   powershell -File tests\compare.ps1 <left> <right> <sheet.png> view1,view2,...
param([string]$Left, [string]$Right, [string]$Out, [string[]]$Views)
Add-Type -AssemblyName System.Drawing
$root = Join-Path $PSScriptRoot "out"
$w = 640; $h = 300
$sheet = New-Object System.Drawing.Bitmap ($w * 2), ($h * $Views.Count)
$g = [System.Drawing.Graphics]::FromImage($sheet); $i = 0
$font = New-Object System.Drawing.Font 'Arial', 12
foreach ($n in $Views) {
	$imgL = [System.Drawing.Image]::FromFile("$root\$Left\$n.png"); $imgR = [System.Drawing.Image]::FromFile("$root\$Right\$n.png")
	$src = New-Object System.Drawing.Rectangle 0, 0, 1280, 600
	$g.DrawImage($imgL, (New-Object System.Drawing.Rectangle 0, ($i * $h), $w, $h), $src, 'Pixel')
	$g.DrawImage($imgR, (New-Object System.Drawing.Rectangle $w, ($i * $h), $w, $h), $src, 'Pixel')
	$g.DrawString("$n   left: $Left   right: $Right", $font, [System.Drawing.Brushes]::Yellow, 4, ($i * $h + 4))
	$imgL.Dispose(); $imgR.Dispose(); $i++
}
$sheet.Save((Join-Path $root $Out))
