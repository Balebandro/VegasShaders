# GunFX plugin

Source of `GunFX.dll`, the xNVSE plugin behind this build's experimental gun effects (muzzle puff, heat smoke,
ejection smoke; it also drives NVR's barrel glow, heat haze and muzzle blast). The player guide and the notes for
developers of other builds are in [`docs/GUNFX.md`](../docs/GUNFX.md).

- `src/GunFX.cpp`: the whole plugin. Game addresses for FalloutNV.exe 1.4.0.525 are listed at the top.
- `src/IniDefaults.h`: creates `GunFX.ini` from `GunFX.ini.defaults`, and later adds only missing keys.
- `config/GunFX.ini.defaults`: shipped as `Data\NVSE\Plugins\GunFX.ini.defaults`. Never ship an active `GunFX.ini`:
  it would overwrite players' settings.
- `assets/Meshes/GunFX/`: the baked smoke effects, shipped as `Data\Meshes\GunFX\`. They are vanilla
  `effects\fxlightsmoke.nif` rebuilt with different numbers. The plugin only sets their emitter rate at run time
  (the puff and port bursts start when spawned and stop after `fBurstSeconds`); it does not rewrite particle modifiers.

Build: Visual Studio 2019 C++ tools (v142), Win32 Release:

    MSBuild GunFX\GunFX.vcxproj /p:Configuration=Release /p:Platform=Win32

The output is `GunFX\build\GunFX.dll`.
