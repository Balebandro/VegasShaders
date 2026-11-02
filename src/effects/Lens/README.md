# Effect spec: Lens

Status 2026-11-02 (branch claude/rw3-lens): `src/hlsl/NewVegas/Effects/Lens.fx.hlsl` and `Compute/Lens.frag` were written fresh from this spec. Same settings, same push layout, same samplers in the same order (s0 to s3). Bloom shaping as written: the bloom's own hue at unit luma, scaled by its original luma raised to the bloom exponent (black stays black; the 1e-6 floor). Not changed: the Vulkan dirt is still read at the frame's own uv (no render-rect mapping): the C++ does not pass RenderScale to this pass, so with RenderScale below 1 the Vulkan dirt still covers only the drawn part (section 6, not fixed here; the C++ would need to change).

Source of truth: src/hlsl/NewVegas/Effects/Lens.fx.hlsl (D3D9), src/effects/Lens.h and Lens.cpp (C++ side), src/hlsl/NewVegas/Compute/Lens.frag (Vulkan pass; compiled Lens.spv sits beside it). Settings from resource/VegasShaders.defaults.toml. Frame order from src/core/ShaderManager.cpp.

## 1. Purpose

A dirt-on-the-lens look. Where the picture is bright (around lights, the sun, bright reflections), the bloom glow is spread across the screen through a fixed dirt-smudge texture, so bright spots bleed soft light over the smudges, the way light scatters on a dirty camera lens. Dark areas and dirt-free parts of the screen are unchanged. The effect only adds light; it never darkens the picture.

## 2. When it runs

- Enabled by the Status key of the Lens section (default off). Skipped when the effect is not loaded.
- Skipped when the player is underwater: the strength is forced to zero and the should-render check returns false.
- Skipped when its strength for the current place and time is zero or less.
- Strength is chosen by place and time. Exterior uses the Night and Main values, blended by the game's day/night transition curve (Night value at curve 0, Main value at curve 1). Interior (any non-exterior cell) uses the Interiors values. There is no separate menu rule; the effect simply follows the frame it is given.
- Frame order: drawn in the NVR post-effect stage, after the game's tonemapper has produced the picture. The copy taken at the start of that stage is tonemapped and display-encoded sRGB. Within the stage it runs after the exposure step and after this frame's bloom buffer has been rendered, and before the optional pre-tonemap LUT step. Its timer label is "Pre-tonemap lens + LUT". Its output becomes the current image for the LUT step and everything after.
- Because bloom is produced earlier in the same frame, the bloom input is this frame's bloom, not last frame's.

## 3. Inputs

Textures (D3D9 technique). NVR binds samplers by declaration order, so the slot order below is the order the shader declares them:

| Slot | Name | Holds | Addressing / filter |
|---|---|---|---|
| s0 | TESR_RenderedBuffer | The current image. Declared, but this technique never samples it. | Clamp, linear |
| s1 | TESR_SourceBuffer | The frame copy taken at the start of the post-effect stage. This is the image the effect reads. | Clamp, linear, mip linear |
| s2 | TESR_BloomBuffer | This frame's bloom (the first bloom level the bloom effect publishes). | Clamp, linear, mip linear |
| s3 | TESR_LensSampler | The dirt-smudge image, file Effects\dirtlens.png. Only its red channel is used; its colour space was not checked. | Wrap, linear, mip linear |

Vulkan pass (Lens.frag): the same three images at descriptor set 1, bindings in order: binding 0 = source image, binding 1 = bloom image, binding 2 = dirt image. Samplers are chosen by index from the push block (section 7).

Constants (D3D9):
- TESR_LensData (float4) is the only constant this effect registers. Its register is not fixed in the source; the constant table assigns it. Components:
  - x = effective strength (multiplier on the added light; 0 means nothing is added). Unitless.
  - y = bloom exponent (the bloom's luma is raised to this power; larger = the smudges light up only right around the brightest spots). Unitless.
  - z = smudginess (0 up to about 3). The dirt image is raised to the power (3 minus z), floored at 0: 0 gives the sharpest, sparsest dirt; larger values cover more of the screen; at 3 the exponent is 0 and the dirt is effectively full coverage. Unitless.
- Declared in the shader but not used by this effect (do not rely on them): TESR_ReciprocalResolution, TESR_CinemaSettings, TESR_SunColor, TESR_SunAmbient, TESR_SunAmount, TESR_DebugVar.
- TESR_RenderScale comes in through the shared render-rect include. The manager sets it for every pass (section 6); this effect's C++ does not register it.

## 4. Settings

All under the Shaders.Lens sections. The defaults file writes the headers with a leading underscore (for example [_Shaders.Lens.Main]); the code reads the same sections without it. All values are floats except Status.Enabled, a boolean.

| Section | Key | Type | Range | Default | Visual effect |
|---|---|---|---|---|---|
| Main | Strength | float | not stated in defaults | 0.4 | Overall amount of added dirt light by day outdoors. |
| Main | BloomExponent | float | not stated | 0.7 | Higher = smudges light only right around the brightest spots; lower = more of the bright areas light the dirt. |
| Main | Smudginess | float | not stated (about 0 to 3 meaningful) | 0.0 | Higher = more of the screen covered by dirt. |
| Night | Strength | float | not stated | 0.4 | Same as Main, used outdoors at night (blended with Main by the day/night curve). |
| Night | BloomExponent | float | not stated | 0.7 | Same as Main. |
| Night | Smudginess | float | not stated | 0.4 | Same as Main. More dirt at night by default. |
| Interiors | Strength | float | not stated | 0.05 | Interior amount (very low by default). |
| Interiors | BloomExponent | float | not stated | 0.6 | Same as Main, interiors only. |
| Interiors | Smudginess | float | not stated | 0.2 | Same as Main, interiors only. |
| Status | Enabled | bool | true / false | false | Turns the whole effect on or off. Off = no pass runs, nothing is added. |

Notes:
- The in-game overlay's lens sliders read and write Shaders.Lens.Main Strength and Smudginess only. Night and Interiors values are edited in the file, not by the overlay.
- The overlay sets each of those two values as a base value plus a scaled offset from the overlay's zoom control, and clamps the written value at zero.
- BloomExponent is not exposed in the overlay.

## 5. Passes and outputs

- One pass, one technique. The D3D9 technique is unnamed in the source; technique index 0 is the only one the C++ asks for. Vertex shader: the shared full-screen frame vertex stage (passes position and texture coordinate through). Pixel shader: the lens pass. Shader model 3.0 (vs_3_0 / ps_3_0).
- Render target: the frame's current render target (the chain target the earlier effects wrote to). No new target is created.
- Output: opaque colour (alpha written as 1). Full-screen; every pixel is written.
- Vulkan: one pass named "Lens" through the NVR extension, one push block, the same output target, one colour output at location 0.

## 6. Behaviour details

- Colour space: the source is display-encoded sRGB. It is converted to linear before light is added and converted back to sRGB at the end. The bloom and dirt values are used as stored, without conversion.
- Dirt lookup (D3D9): the dirt is looked up through the render-rect helper, so with RenderScale below 1 the dirt image spans the whole screen rather than only the drawn top-left part. With RenderScale 1 (or a native frame) this is the identity.
- Source and bloom lookups use the rect coordinate (the texture coordinate itself), so they follow the drawn part of a RenderScale frame.
- Dirt: only the red channel of the dirt, after the smudginess power, is used. The dirt is a monochrome mask.
- Bloom shaping: the bloom colour is first normalised to unit luma (divided by its own luma, with the denominator floored at 1e-6, so a black bloom gives zero, not NaN). Its luma is then raised to the bloom exponent and the colour is scaled by that. Hue is kept; brightness of the dimmer glow is reduced.
- Luma: Rec. 709 weights (0.2126, 0.7152, 0.0722) on the bloom as read.
- Added light = dirt (red channel, after the power) times shaped bloom times strength, added to the linear source, then converted back to sRGB. There is no clamp in the shader; values above 1 are clipped by the target format. A bright spot with strong dirt saturates to white rather than rolling off.
- Sky and depth: no depth read and no sky test. The effect is applied to the whole frame, sky included, equally.
- First-person weapon and HUD: no special case; the effect sees the already-composited frame.
- Jitter (TAA): none. No history sampling and no sub-pixel offset.
- Reversed depth: not applicable (no depth).
- HDR vs LDR: LDR. The input is the tonemapped, sRGB-encoded picture; feeding HDR linear data would be wrong.
- NaN/inf guards: only the bloom normalisation has a guard (floor 1e-6). The dirt, the source and the final conversion have none; a non-finite bloom sample would propagate.
- Underwater: strength is zeroed and the pass is skipped (no added light).
- Off: the pass is not run at all (no cost, no copy).
- Vulkan difference (important): the Vulkan pass samples the dirt at the frame's own texture coordinate, not through the render-rect helper the D3D9 pass uses. With RenderScale below 1, the Vulkan dirt covers only the drawn part, while D3D9 covers the whole screen. The arithmetic is otherwise the same.

## 7. Vulkan path

- Shader: src/hlsl/NewVegas/Compute/Lens.frag (GLSL 450, includes Common.glsl). Compiled output Lens.spv in the same folder.
- Selection: the effect's C++ extension entry is used when the NVR DXVK extension is present and the pass option is on. The option is NvrDxvkPasses in resource/VegasShaders.defaults.toml, section [_Main.Main.Performance] (top-level key, default on). D3D9 is used instead when the effect is named in the keep-on-D3D9 list, when the extension render failed earlier this session (logged; the session then falls back to D3D9 for this effect), or when a shader-model-6 build covers the technique and that option is on.
- Pass name: "Lens". It selects Compute\Lens.spv through the shared extension pass helper.
- Textures, in the order handed to the pass: index 0 = frame copy (source), index 1 = first bloom level (null if the bloom effect does not exist), index 2 = dirt file texture. Bound at set 1, bindings 0, 1, 2.
- Samplers: the extension's sampler heap is set 0 binding 0. Two sampler indices are passed: linear clamp (source and bloom) and linear wrap (dirt).
- Push block (C++ sends 32 bytes; the shader reads the first 24):
  - offset 0: four floats, the same as TESR_LensData (x strength, y bloom exponent, z smudginess).
  - offset 16: unsigned integer, sampler index for linear clamp.
  - offset 20: unsigned integer, sampler index for linear wrap.
  - offsets 24 to 31: two unused unsigned integers (padding).
  The shader masks each sampler index to its low 16 bits. The offsets 16 and 20 are the values the C++ passes as sampler-index offsets; they must stay in step with the shader's push block.
- Technique index other than 0 is refused (no pass).

## 8. Look and cost

- Correct: a faint, soft glow of dirt-shaped light around bright lights and sun glints. In daylight outdoors it is subtle; at night more visible (default Night smudginess 0.4); indoors very faint (Interiors strength 0.05). Dark scenes and dirt-free parts of the screen show no change. The dirt shape is fixed on screen and does not move with the camera.
- Typical wrong results:
  - Dirt visible on dark areas with no bright source: bloom is not reaching there, so the dirt should stay dark. Check the bloom input and the bloom exponent.
  - Haze that lifts blacks across the screen: strength or smudginess too high, or the bloom buffer is not black where the picture is dark.
  - Dirt shifted or mis-sized on Vulkan with RenderScale below 1: the coordinate difference in section 6.
  - Hard white clipping around bright lights: expected to some degree (no clamp), but default settings should not blow bright spots to flat white.
  - Black speckles or NaN-looking pixels: check the bloom normalisation.
  - Colour tint from the dirt: none expected, since only the red channel is used as a neutral mask and the light takes the bloom's own colour.
- Cost class: cheap. One full-screen pass, three texture reads (source, bloom, dirt), a few power functions per pixel. No loops, no extra targets, no copies beyond the frame copy the stage already makes. Skipped entirely when off or underwater.

## 9. Open questions

- Strength, BloomExponent and Smudginess have no stated ranges in the defaults file and no clamps in the code. Usable ranges are not documented here; the overlay slider ranges were not read.
- The dirt image is not in the repository. Its location on a real install (Effects\dirtlens.png relative to the shader folder) and whether it is colour or grey-scale were not confirmed.
- Which bloom level the D3D9 bloom sampler receives is set outside Lens.cpp (by the bloom/chain code). Assumed to be the first bloom level, as on the Vulkan path; not verified.
- What the Vulkan pass does when the bloom effect does not exist (null bloom entry): not traced.
- The timer label says "Pre-tonemap", but the frame-copy comment says the picture is already tonemapped at that point. The spec follows the code (after the tonemapper); the label is probably historical. Not resolved.
- Whether the effect is in the keep-on-D3D9 list, or has a shader-model-6 build, was not checked.
- The colour space of the dirt texture as loaded (linear or sRGB) was not checked; the shader uses the stored value.
- The render-rect and frame-chain behaviour is taken from the shared include's comments and the manager's comments; not re-verified in the manager code.
