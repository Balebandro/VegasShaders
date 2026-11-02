# Sharpening

Rewritten 2026-11-02 (wave 3, branch claude/rw3-screen, commit a2231d88). New shaders: src/hlsl/NewVegas/Effects/Sharpening.fx.hlsl (D3D9) and
src/hlsl/NewVegas/Compute/Sharpening.frag (Vulkan). The C++ is unchanged.
FIXED: the D3D9 neighbourhood weights are now the intended Manhattan weights (centre 1, edges 2/3, corners 1/3) in both paths (section 6
and item 2 below). FIXED: Offset 0 (or less) gives no sharpening, not a divide by zero. FIXED: the output is clamped before the write.


## 1. Purpose

Sharpening adds local edge contrast to the whole picture so that near surfaces look crisper. It compares each pixel with the average of a small neighbourhood and pushes it away from that average, so detail and edges stand out. The effect fades out with distance from the camera: nearby surfaces get the full boost, and surfaces past a set distance get none, so the sky and far land are left alone. The player sees a slightly harder, more detailed close-up picture. The boost is not clamped (section 9), so strong settings can produce halos along strong edges.

## 2. When it runs

- **Switch:** `[_Shaders.Sharpening.Status] Enabled`, default `false`. When off the effect does not run and no pass is made.
- **Frame position:** in the post chain (`ShaderManager.cpp`, the block that renders Sharpening). Order: ... SMAA, FXAA, **Sharpening**, Cinema, ImageAdjust, Bodycam, SkyDither, Debug. It therefore sees the anti-aliased picture (after FXAA) and its output feeds Cinema and everything after it. It is one of the effects in the chain's "last effect" list, so it can be the final writer to the game target when nothing renders after it.
- **Interior and exterior:** there is no separate on/off per cell type. The strength, clamp and offset values change with the cell type (section 4). The choice is made by the game state flag `isExterior`: exterior uses the `Main` values, interior uses the `Interiors` values. The values are re-chosen every frame and also when settings change.
- **Night:** no special rule. The same values apply at night.
- **Menus:** no special rule in the effect; it runs whenever the post chain runs and the switch is on.
- **Single technique:** technique index 0 only. The C++ asks for index 0 and returns nothing for any other index.

## 3. Inputs

### Textures (in slot order; NVR binds samplers by declaration order)

| Slot | Name | Holds | Filtering / addressing |
|---|---|---|---|
| s0 | `TESR_RenderedBuffer` | The current post-chain picture (input colour; alpha is carried through) | Linear, clamp |
| s1 | `TESR_DepthBuffer` | CombineDepth's combined depth. The first channel stores linear depth divided by the far plane distance. | Linear, clamp |

The C++ supplies these two in this order: first the rendered picture, then the combined depth. If the combined depth is not available, the Vulkan path declines to draw and the D3D9 path is used (what the D3D9 path then binds when the depth is missing is not traced; see section 9).

The rendered picture is the chain's current picture (`TheTextureManager->RenderedTexture`).

### Constants

| Name | Register | Components and meaning |
|---|---|---|
| `TESR_SharpeningData` | not fixed (registered by name) | x = Strength (the boost); y = Clamp (see section 9, not used by the shader); z = Offset (the far distance factor, see section 6); w = 0 (unused) |
| `TESR_ReciprocalResolution` | not fixed (registered by name) | xy = 1 divided by the picture width and height in pixels; z = width / height |
| `TESR_CameraData` | not fixed (used through the depth helper) | y = far plane distance in game units (used to turn the stored depth into game units) |

Units: depth in game units (the same units as the game's world). Offset is a unitless multiplier (see section 6).

## 4. Settings

All keys are read in `Sharpening.cpp`. There are no range comments in the toml for these keys, so ranges are listed as "not stated" (section 9).

| Section | Key | Type | Default | What it changes |
|---|---|---|---|---|
| `[_Shaders.Sharpening.Main]` | `Strength` | float | 0.75 | Outdoors boost. Scaled by 2 in the shader, so the effective multiplier of the edge contrast is 2 x Strength. 0 = no change. |
| `[_Shaders.Sharpening.Main]` | `Clamp` | float | 0.1 | Stored with the constants but NOT used by either shader (section 9). Changing it has no visible effect. |
| `[_Shaders.Sharpening.Main]` | `Offset` | float | 2.0 | Outdoors distance at which the sharpening fades out. Multiplied by 1000 in the shader: 2.0 gives 2000 game units. 0 is a divide-by-zero case (section 6). |
| `[_Shaders.Sharpening.Interiors]` | `Strength` | float | 0.75 | Indoors boost, same meaning as outdoors. |
| `[_Shaders.Sharpening.Interiors]` | `Clamp` | float | 0.1 | Stored, not used (as above). |
| `[_Shaders.Sharpening.Interiors]` | `Offset` | float | 2.0 | Indoors fade distance, same meaning as outdoors. |
| `[_Shaders.Sharpening.Status]` | `Enabled` | bool | false | Turns the whole effect on or off. |

## 5. Passes and outputs

- **D3D9 path:** one pass, technique index 0 (the file's only technique, unnamed). It draws a full-screen quad into the chain's output target for this step. Output format is the chain's format (8-bit per channel LDR), same resolution as the picture. The output alpha is the input alpha.
- **Vulkan path:** the C++ asks for the Vulkan pass named `Sharpening` (SPIR-V built from `Sharpening.frag`). It runs as one full-screen pass into the chain's output (or the game target when it is the last effect). Same result as the D3D9 pass.
- The pass must not write any other target, and must not alter the depth.

## 6. Behaviour details

**Depth falloff.** The depth read is linear distance in game units: stored value times the far plane distance. Let `maxDepth` = Offset x 1000. The falloff factor is `(maxDepth - distance) / maxDepth`, clamped to 0..1, then raised to the power 4. Result: 1 at the camera, falling smoothly to 0 at `maxDepth`, and 0 beyond. The sharpening is blended by this factor, so it is a true fade, not a cut.

- With the default Offset of 2.0 the fade ends at 2000 game units.
- Sky: the sky's stored depth is at the far end, so its falloff is 0 and the sky is not sharpened.
- Offset = 0 makes `maxDepth` 0 and the falloff divides by zero. The new shader must guard this (treat it as "no falloff: no sharpening" or clamp the offset to a small positive value). The existing shader does not guard it.

**Neighbourhood.** A 3 x 3 neighbourhood (offsets -1, 0, +1 in x and y, in texels of the picture, using the reciprocal resolution). The average is a weighted mean. The weight of a tap is intended to fall with its Manhattan distance from the centre: centre 1, edge neighbours 2/3, corners 1/3. FIXED 2026-11-02: both paths use these weights. The old D3D9 source divided two whole numbers, which made every weight 1 (a plain box average); that is gone.

**Contrast and boost.** The contrast measure is the sum of the absolute differences between the pixel and the average in the three colour channels, multiplied by a luma weight of `L / (L + 0.05)`, where `L` is the pixel's BT.709 luma (0.2126 R, 0.7152 G, 0.0722 B). The weight makes dark pixels count less than bright ones, so that dark noise is not sharpened. The boost is then `saturate(contrast x 2 x Strength)`. The output is the pixel plus `(pixel - average) x boost`, blended with the original pixel by the falloff factor.

- Output is not clamped to 0..1 beyond what the 8-bit target does on write. The boost can push values out of range. Values that go below 0 or above 1 are clipped by the target, which can give dark or bright fringes where edges are strong. Clamp per channel to 0..1 before writing, or limit the boost.
- Only the picture is sharpened. Nothing is written back to history.

**First-person weapon.** No special handling. The weapon is near, so it gets the full boost. This is the existing behaviour; it may be wanted or not.

**RenderScale / RenderRect frames.** The C++ supplies the full-target reciprocal resolution. When a render rectangle is active (`RenderRect`, the RenderScale path), the Vulkan pass is off (`DxvkCompute::PassesOn` returns false while a rectangle is active), so the D3D9 pass runs. The neighbourhood step uses the full-target size, not the rectangle. Whether that is right in a rectangle frame is open (section 9).

**Jitter (TAA).** No special handling. This effect runs after TAA's output, on the displayed picture, so the jitter does not reach it.

**Reversed depth.** The depth texture is the NVR combined depth (linear, stored as depth over far distance), not the game's raw depth buffer, so reversed depth does not matter here. The only depth rule needed is "far = sky".

**HDR vs LDR.** LDR only. The chain picture is 8-bit. No HDR handling.

**NaN and inf.** No guard in the source. The luma weight is safe (its denominator is at least 0.05). The falloff divide is not safe at Offset 0 (see above). Averages with zero total weight cannot occur (the centre tap always has weight 1, and under the D3D9 rule all taps have weight 1).

**Anything that would look wrong if missed:** sharpening sky or far fog (the falloff must reach 0 beyond the Offset distance); dark noise being boosted (keep the luma weight); halos on high-contrast edges (the boost must not run away); forgetting the Interiors values when indoors (the interior and exterior values are separate).

## 7. Vulkan path

- **Files:** `src/hlsl/NewVegas/Compute/Sharpening.frag` (source; SPIR-V is `Sharpening.spv` in the same folder).
- **Selection:** the C++ `SharpeningEffect::ExtRender` is the Vulkan entry. It is used only when NVR DXVK is loaded and `[Main.Main.Performance] NvrDxvkPasses` is on, and the frame is not a render-rectangle frame (`DxvkCompute::PassesOn`). Otherwise the D3D9 technique runs. If the pass fails to build or draw, it falls back to D3D9 for the rest of the session.
- **Technique:** only technique index 0 is handled; any other index returns "not drawn".
- **Bindings (descriptor set 1):** binding 0 = the rendered picture (`t_rendered`), binding 1 = the combined depth (`t_depth`). Both are sampled 2D textures.
- **Sampler:** one sampler, taken from the push block at offset 48 (linear, clamp).
- **Push constants (64 bytes):**

| Offset | Type | Field |
|---|---|---|
| 0 | vec4 | `TESR_SharpeningData` (x Strength, y Clamp, z Offset, w unused) |
| 16 | vec4 | `TESR_ReciprocalResolution` |
| 32 | vec4 | `TESR_CameraData` (y far plane distance) |
| 48 | uint | sampler index (linear clamp slot) |
| 52 to 63 | uint x 3 | padding |

The shader's neighbourhood weights are the only place the two paths differ (section 6).

## 8. Look and cost

- **Correct:** near edges, textures and foliage look crisper and more defined, with no ringing or bright or dark outlines along strong edges. Distant land and the sky look unchanged. Strength 0 gives exactly the unsharpened picture. Default settings should look like a gentle firming of the image, not a visible filter.
- **Typical wrong result:** bright and dark halos around high-contrast edges (too strong a boost with no clamp); grain or noise made sharper (the luma weight missing or wrong); a sharpened sky or horizon (falloff not reaching 0); a sudden switch at the Offset distance (falloff not smooth); a visible jump when crossing from outside to inside (interior and exterior values not separated).
- **Cost class:** per-pixel loop, single pass. Nine colour reads plus one depth read per pixel. Cheap compared with the full-screen blur effects, but not free.

## 9. Open questions

1. **Clamp is stored and never used.** Both shaders ignore the `Clamp` value. Decide whether it should limit the boost (the name suggests so) or be removed from the settings. The toml gives it no description.
2. **Neighbourhood weights (FIXED 2026-11-02).** Both paths now use the Manhattan weights. The D3D9 box average was the integer-division bug; the Vulkan look was the intended one.
3. **Strength range is not stated** in the toml (no `[min-max]` comment). The ImGui menu may impose one; not checked.
4. **Offset range is not stated.** Zero is a divide-by-zero case in the shader; the valid range should be written down.
5. **Rectangle frames.** The neighbourhood uses full-target size. Confirm this is the intended behaviour under RenderScale (where the picture is at full size after upscaling, so probably correct, but not verified).
6. **Output clipping (FIXED 2026-11-02).** The boosted colour is clamped to 0..1 before the write; the 8-bit target does the same, so the picture is unchanged.
7. **Alpha.** The output keeps the input alpha. Confirm nothing downstream depends on it.
8. **Missing depth.** The D3D9 path's behaviour when CombineDepth's texture is absent was not traced. Confirm it cannot happen in the chain, or specify the fallback.
