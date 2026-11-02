# PBR

Physically based lighting for objects and the ground: Lambert diffuse plus GGX highlights for the sun and lamps, metal, and
optional per-texture material maps. Settings: `[Shaders.PBR.*]` (`defaults.toml` here). Shader code: `Shaders/Includes/PBR.hlsl`
(the lighting maths), `Shaders/Includes/Object.hlsl` (material inputs), `Shaders/ObjectTemplate.hlsl` / `ParallaxTemplate.hlsl`
(the object shaders that use them).

This doc is still partial: it covers the material maps. The rest of the effect is described in the settings' own descriptions.

## Material maps (`_rmaos`)

`[Shaders.PBR.Rmaos] Enabled` (restart). Where a texture has `<name>_rmaos.dds` beside it or under `textures\pbr\` at the same
path, the object shaders read it on sampler s2 (`Render.cpp` RmaosDraw binds it per draw, sets `NVR_Rmaos` c220). Layout is the
Community Shaders "True PBR" one:

| Channel | Meaning | Used as |
|---|---|---|
| R | roughness | replaces the guess from the normal map's alpha (`getRoughness`) |
| G | metal | only with `MetalFromMap` (default off: NVR's generated maps carry a guessed metal) |
| B | ambient occlusion, white = none | darkens the ambient light (`RmaosAmbient`, times `Occlusion` and the weather's `AOScale`) |
| A | specular level | non-metal reflectance F0 = 0.04 x A (`SpecularFromMap`, 2026-11-02) |

Specular level: white (1) is the usual 0.04, so maps without a painted level (and NVR's generated maps, which write A = 1)
look exactly as before; darker values reflect less. It is set through `PBR_DIELECTRIC_F0`, which `Object.hlsl` defines before it
includes `PBR.hlsl`; every other includer of `PBR.hlsl` (skin, terrain, water) gets the plain 0.04. Not applied in the 6-lamp
interior shader (SLS2035, `NVR_SLOT_TIGHT`), which is at the instruction count the driver refuses. `SpecularFromMap = false`
compiles it out (`RMAOS_SPEC_OFF`, `ShaderRecord.cpp`).

Edge cases: a map that has just loaded fades in over `FadeIn` seconds (the specular level too, from 0.04); `DebugView` 1-4 shows
roughness, metal, occlusion, or which objects have a map. MaterialAware reflections read the map's roughness and metal from the
scene's alpha (`core/ReflMaterial.h`).

How to check it: an object with a hand-made map whose alpha is dark should show weaker sun and lamp highlights on its non-metal
parts than before; with NVR's generated maps nothing changes.
