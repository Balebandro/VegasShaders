# WaterLens effect spec

Status 2026-11-02 (branch claude/rw3-lens): the D3D9 effect `src/hlsl/NewVegas/Effects/WaterLens.fx.hlsl` and the Vulkan pass `Compute/WaterLens.frag` were written fresh from this spec. The text below describes the new shader. Fixed from the old one: the sixth power is now an even power (squares, no NaN for a negative base); the normalisation of a zero vector is guarded (open question 9 fixed). The D3D9 pass computes the noise coordinates per pixel (no vertex-stage half-texel shift), so the D3D9 and Vulkan maths now match (open question 6 resolved that way). Settings and the fade are unchanged.

Source of truth for this spec: the NVR tree at `the source tree` (read only). Settings are from `resource/VegasShaders.defaults.toml`. The D3D9 shader is `src/hlsl/NewVegas/Effects/WaterLens.fx.hlsl`. The Vulkan pass is `src/hlsl/NewVegas/Compute/WaterLens.frag` (compiled to `WaterLens.spv`, same folder). The frame order is in `src/core/ShaderManager.cpp`.

## 1. Purpose

When the player leaves water, the screen briefly looks as if seen through the surface of a lens: the whole picture wobbles and is refracted by a drifting ripple pattern, and thin bright bluish glints appear where the ripples bend light most. The ripple is strongest at the moment the player surfaces and fades away over a short span of game time. The effect is off at all other times, including while the player is underwater.

## 2. When it runs

- **Enabled:** the effect's on/off switch is `[Shaders.WaterLens.Status]` `Enabled` (default on). It is also loaded only when its shader source is present and compiled. The effect is drawn only when this switch is on.
- **Fade state (the real gate):** the effect draws only while its strength value is above zero. That value is the setting `Amount` multiplied by a fade value from 0 to 1 (see Behaviour details, fade). Outside a fade the value is zero and the pass is skipped entirely.
- **Underwater:** while the player is underwater the fade value is forced to zero at once, so nothing draws. Leaving water starts a fade from full strength.
- **Location, weather, time of day and menus:** there are no extra rules. The effect is not gated by interior or exterior, night, weather or the menu. Any fade in progress plays in all of these.
- **Renderer:** the D3D9 path always draws. The Vulkan path draws instead when the key `NvrDxvkPasses` in `[_Main.Main.Performance]` (default on) is on, the Vegas Shaders DXVK extension is present, and no render rect is active (render-scale frames use the D3D9 path). If the Vulkan pass fails, the effect falls back to its D3D9 technique for the rest of the session.
- **Frame order:** the effect is in the lens group of the post-process chain. Order of the relevant steps, earlier first: TAA (if on), rain, snow, legacy bloom, heat haze, volumetric smoke, barrel haze, coloring, LUT (if after tonemapping), depth of field, motion blur, then **WaterLens**, then the low-high-frequency effect (LowHF), dither buster, SMAA, FXAA, sharpening, cinema, image adjust, body camera, sky dither, debug, cover map. The effect reads the image as it stands at its position in the chain. It can be the last effect in the chain, in which case it writes straight to the game's target.

## 3. Inputs

### Textures and samplers

Slot numbers are the D3D9 sampler registers. Vulkan binding numbers are given in section 7.

| Order | D3D9 name | Slot | Content | Filtering and addressing |
|---|---|---|---|---|
| 0 | `TESR_RenderedBuffer` | s0 | The frame as it stands at this point in the chain (the current image). Full frame, not only the render rect. | Linear min, mag and mip. Clamp on U and V. |
| 1 | `TESR_waterlensSampler` | s1 | The file texture `Effects\water_NRM_lens.dds` (on disk: `resource/Textures/Effects/water_NRM_lens.dds`). A tileable normal-style noise map. Each texel has four channels, each stored 0..1 and read back as -1..1. The alpha channel is used as a second height value. | Linear min, mag and mip. Wrap on U and V. |

The C++ binds samplers by declaration order: index 0 is the rendered frame, index 1 is the noise map. The noise map is loaded as a file texture; the effect does not create it.

The effect does not declare a source-buffer sampler, so the game's source frame is not copied for it.

### Constants

| Name | Register | Components and meaning | Units |
|---|---|---|---|
| `TESR_GameTime` | not fixed (the D3D9 compiler assigns it; the C++ binds by name) | x: game clock in milliseconds. y: game hour of day. z: real time in seconds since the frame-rate timer started (used by this effect). w: last frame time in seconds. | ms, hours, s, s |
| `TESR_ReciprocalResolution` | not used by the new shader (not declared) | was: 1 / width and height, for the old D3D9 vertex-stage half-texel shift. | 1/pixel |
| `TESR_WaterLensData` | not fixed, bound by name | x: time multiplier A (`TimeMultA`). y: time multiplier B (`TimeMultB`). z: viscosity (`Viscosity`). w: effective strength = `Amount` times the current fade value (0 to 1). | multipliers, multiplier, multiplier, strength |

For the D3D9 path, the constant `TESR_WaterLensData` is updated each frame: its x, y and z come from settings and its w from `Amount` times the fade. Nothing else in the effect changes between frames apart from `TESR_GameTime`.

## 4. Settings

All keys are read from the section `Shaders.WaterLens.Main` (written with a leading underscore in the defaults file, as the settings loader expects for all sections). Keys are read by the effect every frame when it is enabled.

| Section | Key | Type | Range | Default | Effect on the picture |
|---|---|---|---|---|---|
| `Shaders.WaterLens.Main` | `Amount` | float | not declared in the defaults file or menu (see section 9) | 0.1 | Overall strength. Scales the whole refraction and the highlight. Zero turns the effect off. Also the top value the fade starts from. |
| `Shaders.WaterLens.Main` | `TimeMultA` | float | not declared | 0.1 | Speed of the first ripple pattern's drift and wobble. Higher means faster ripple motion. |
| `Shaders.WaterLens.Main` | `TimeMultB` | float | not declared | 0.2 | Vertical scroll speed of the second ripple pattern. Higher means the lower-frequency detail travels faster down the screen. |
| `Shaders.WaterLens.Main` | `Viscosity` | float | not declared | 0.05 | How strongly the ripple normal tilts in the screen plane before it is normalised. Higher means stronger bending of the picture and stronger glints (the name suggests the opposite; see section 9). |
| `Shaders.WaterLens.Main` | `Time` | float | not declared | 180.0 | Present in the defaults file but **not read by the effect**. Has no visible effect. |
| `Shaders.WaterLens.Status` | `Enabled` | bool | on / off | true | Turns the whole effect on or off from the menu. Read as the effect's enabled state. |

No hand-written menu entry for WaterLens was found in the ImGui code; its settings are listed from the defaults file, without ranges.

## 5. Passes and outputs

**D3D9 path.** One technique, index 0, with one pass. The C++ asks for technique index 0 only; any other index is refused. The pass draws a full-frame quad over the target rectangle (the render rect's top-left part when render-scale frames are on). The pixel output is written to the frame chain's current output target, which is the same format and size as the chain's frame (HDR or LDR as the chain is). The output alpha is 1. The shader does not set blending; the target receives the opaque colour.

**Vulkan path.** One graphics pass named `WaterLens`, writing one colour target: the chain's output target, or the game's final target when the effect is the last in the chain. Same format and size as the chain. No depth, no stencil, no blending.

Neither path needs extra render targets or a second frame copy.

## 6. Behaviour details

**The effect reads only the frame.** No depth, no sky test, no first-person weapon test, no HUD test. Everything in the frame at this point is refracted, including the sky. A new version should not try to mask the weapon or sky unless asked.

**Two noise samples, one picture sample, one highlight term.** For each pixel:
1. Sample the noise map twice. The first sample uses the frame coordinate halved and offset by a slow sine wobble in x and a drift in y, both driven by real time times `TimeMultA` (the D3D9 path computes these coordinates in the vertex stage and interpolates them; the Vulkan path computes them per pixel). The second sample is the frame coordinate offset straight down by real time times `TimeMultB`.
2. Map each sample from 0..1 to -1..1 on all four channels. Add the alpha channel of the second sample to the blue channel of the first sample.
3. Form a normal from the first sample's x, y and z, scale its x and y by `Viscosity` (z is left as is), and normalise to unit length. This gives a refraction direction.
4. Sample the frame at the pixel's own coordinate, moved by the refraction direction's x and y multiplied by 0.2 and by the effective strength. Clamp addressing on the frame.
5. Compute the highlight: take the refraction direction's x and y, multiply each by the effective strength, raise each to the sixth power, clamp each to 0..1, add them, and multiply by a bluish tint (0.85, 0.85, 1.0). This is added to the picture with no clamp.
6. Output the displaced frame plus the highlight, alpha 1.

**Even power for the highlight.** Step 5 raises a possibly negative value to the sixth power. The sixth power of a negative number is positive, and the intended result is the even power. A GPU `pow` of a negative base is undefined and can give NaN. A new implementation must produce the even power of the signed value (for example by squaring three times), not a generic power of a possibly negative base.

**Normalisation guard.** Step 3 normalises a vector that can, in principle, be zero. A zero vector must not produce NaN or inf. Use the un-normalised vector or a small epsilon in that case, and make sure the pixel is still a valid colour.

**Edge handling.** The noise map wraps, so tiling is seamless. The frame clamps, so refraction at the screen edge repeats the edge pixels rather than sampling outside.

**Half-texel convention.** The new D3D9 pass computes everything per pixel in the pixel shader, with no half-texel shift, the same as the Vulkan pass (sub-pixel; open question 6 accepted as this).

**Real time vs. game time.** The ripple motion uses `TESR_GameTime` z, which is real seconds, so it keeps moving during fades and while the game is paused. The fade itself uses game time (below).

**Fade.** The fade value is a linear ramp in game hours (game days times 24):
- When the player goes underwater (the game's underwater flag, read from the sky): the fade is set to zero at once. No fade-out.
- When the player leaves the water: the fade starts at 1 and falls linearly to 0 over 0.01 game hours (36 game seconds). The ramp is the strength `Amount` times the fade.
- If the player goes back underwater during the fade, the fade drops to zero at once.
- The fade only updates while the effect is enabled. If the effect is switched off while underwater, its fade state is not updated until it is switched on again.
- Game start is not underwater and the fade is zero, so nothing draws at load.

**HDR vs. LDR.** The effect does no tonemapping and no clamping of its output. On an HDR chain the highlight can push values above 1, which is intended. Do not clamp the output to 1.

**Render rect and render scale.** When a render rect is active (render-scale frames), the D3D9 draw is restricted to the render rect's viewport, and the Vulkan path is not used at all. The shader does not use `TESR_RenderScale` itself; its coordinates are over the whole target as drawn (see section 9).

**Reversed depth, jitter, TAA.** None used. The effect does not read depth and does not apply jitter.

**What looks wrong if missed:**
- Refraction that never fades, or ripples while swimming.
- A hard cut-off at the end of the fade instead of a ramp.
- Sample jumps or seams at the noise map's edges (the wrap addressing must be on).
- Black or speckled pixels, which come from NaN in the sixth power or the normalisation.
- Glints across the whole screen; they should appear only where the ripple bends light strongly.
- Highlight clipped to white; the tint and additive sum must stay.

## 7. Vulkan path

**Files.** `src/hlsl/NewVegas/Compute/WaterLens.frag` (the source) and `src/hlsl/NewVegas/Compute/WaterLens.spv` (the compiled pass, loaded from the Vegas Shaders DXVK shader folder at run time as `Shaders\VegasShaders\Compute\WaterLens.spv`). The source includes `Common.glsl` for the shared sampler table. There is no compile-time variant (it is not in `variants.txt`) and no SM6 build; the generic `WaterLens.spv` is the only pass.

**Selection.** The C++ uses the Vulkan path when the Vegas Shaders DXVK extension is present, `NvrDxvkPasses` is on, no render rect is active, the effect is not named in `NvrDxvkKeepD3D9`, and the technique index is 0. Otherwise (any of those false) the D3D9 technique draws. After a failure the effect stays on D3D9 for the session and the log says so. The pass is made from the `WaterLens` shader name, with two texture bindings and two sampler bindings, and a push block of 48 bytes.

**Bindings.**
- Set 1, binding 0: the rendered frame (`TESR_RenderedBuffer` equivalent), sampled.
- Set 1, binding 1: the noise file texture (`TESR_waterlensSampler` equivalent), sampled, fetched through the extension's file-texture path under the name `TESR_waterlensSampler`.
- Set 0, binding 0: the shared sampler table (from `Common.glsl`).
- Sampler selection is by heap index in the push block. The linear/clamp sampler index is at push offset 32; the linear/wrap sampler index is at push offset 36. The C++ sets both fields to zero; the extension writes the real indices at those offsets from the binding descriptors.

**Push block layout (48 bytes, all 4-byte aligned).**
- Offset 0: `gameTime`, a vec4 equal to `TESR_GameTime` (z is the real-time value used).
- Offset 16: `waterLensData`, a vec4 equal to `TESR_WaterLensData` (x, y, z, w as in section 3).
- Offset 32: `samplerIndex`, a uint, the linear/clamp sampler heap index (written by the extension).
- Offset 36: `wrapIndex`, a uint, the linear/wrap sampler heap index (written by the extension).
- Offsets 40 to 47: padding, unused.

**Coordinates.** The pass's `uv` input is the full-screen coordinate at pixel centres, as the extension supplies it. The Vulkan shader derives the two noise coordinates from `uv` itself, so the noise moves as the D3D9 version does.

**Matching a new Vulkan version.** A new version must implement the behaviour in section 6 exactly, keep the same push offsets and binding order above, sample the frame through the clamp sampler and the noise map through the wrap sampler, and compute the sixth power with an even-power formulation (not a generic power of a possibly negative base).

## 8. Look and cost

**Correct result.** While the player is leaving water, the picture is visibly wobbled by a slowly drifting, tileable ripple, with soft refraction and small bright bluish glints where the ripples bend light most. The effect is strongest at the moment of surfacing and fades out over roughly half a minute of game time. With the effect off, or at any other time, the picture is untouched.

**Typical wrong results:**
- Constant distortion or ripples while the player is in normal play.
- Ripples that stay after the fade, or a sudden cut-off instead of a fade.
- Black or speckled pixels (NaN from the sixth power or from normalising zero).
- The whole picture washed out by glints; the glints must be limited to strong bends.
- A visible seam or stretching at the noise map's edges.
- Ripples that do not move, which happens when real time is not used.
- Highlight clipped to white instead of adding a tint.

**Cost class:** cheap. A single full-screen pass with three texture fetches and a handful of arithmetic per pixel. It runs only during the fade, so it costs nothing outside it. Not a per-pixel loop and not multi-pass.

## 9. Open questions

1. **Setting ranges.** The defaults file and the menu give no range for `Amount`, `TimeMultA`, `TimeMultB` or `Viscosity`. Ask the maintainer what a sensible range is before writing a clamp or a menu slider.
2. **Unused key.** `Time` = 180.0 in `[Shaders.WaterLens.Main]` is in the defaults file but the effect does not read it. Confirm whether it can be removed or was meant for the fade length.
3. **Viscosity direction.** `Viscosity` raises the strength of the bend (larger value, larger refraction), which is the opposite of what the name suggests. Confirm this is intended before a new version renames or inverts it.
4. **Fade duration.** 0.01 game hours is about 36 game seconds. The real-time length depends on the game's time scale. Confirm the intended feel.
5. **Fade while paused.** The fade follows game time, so it may stop while the game clock is stopped. Confirm for menus and dialogue.
6. **Half-texel and pixel-centre convention.** The D3D9 path shifts the coordinates by half a texel and the Vulkan path does not. Confirm which one is the reference, or accept the sub-pixel difference.
7. **Render rect.** Whether the frame sample should be limited to the render rect when render-scale frames are on (the D3D9 draw is restricted to its viewport, but the shader samples the whole target). Confirm the intended behaviour.
8. **HUD and weapon.** The effect is drawn in the chain before the final interface and does not mask the first-person weapon. Confirm whether the HUD is in the frame at this point.
9. **NaN handling.** FIXED 2026-11-02 in the new shader: the sixth power is formed as squares (an even power), and the normalisation guards a zero vector. Not yet confirmed on a GPU.
10. **Vulkan uv convention.** The extension's full-screen `uv` is assumed to be at pixel centres, as the Vulkan source comments say; confirm with a capture.
