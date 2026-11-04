# BloomLegacy

Source reviewed (wave 3, 2026-11-02): the new D3D9 effect src/hlsl/NewVegas/Effects/BloomLegacy.fx.hlsl (with the shared
Includes/RenderRect.hlsl); the new Vulkan passes src/hlsl/NewVegas/Compute/BloomLegacyBloom.frag, BloomLegacyBlur.frag and
BloomLegacyCombine.frag (branch claude/rw3-colour); C++ src/effects/BloomLegacy.cpp, BloomLegacy.h (unchanged); settings
resource/VegasShaders.defaults.toml; frame order in src/core/ShaderManager.cpp.

## 1. Purpose

An older, alternative bloom that works on the finished (tonemapped) picture rather than the HDR scene. Bright areas are pulled out,
blurred in two directions, and added back over the picture, while the picture is darkened behind the glow. The result is a softer,
bleeding light look around bright lights, with a visible darkening of the surroundings of the glow. It is off by default. Its glow is
not read by the tonemapper and does not replace Bloom (the HDR glow); both can be on at once.

## 2. When it runs

- Position: post-tonemap, in the weather block of RenderEffects (ShaderManager.cpp). The order is: rain, snow, GPU particles, then
  BloomLegacy, then heat haze, volumetric smoke, barrel haze, coloring and LUT, and so on.
- Enabled: [Shaders.BloomLegacy.Status] Enabled (bool, default false).
- Gates: [Main.Main] RenderEffects on, a player cell present, overlay off. No ShouldRender override.
- Interior and exterior: the section is chosen per frame. Exteriors section when outdoors (day and night share it), Interiors section
  otherwise. No transition blend: the switch is immediate.

## 3. Inputs

### Textures

| Slot | Name | Holds |
|---|---|---|
| s0 | TESR_RenderedBuffer | the current picture (the output of the previous pass, or the picture for the bright pass) |
| s1 | TESR_SourceBuffer | the picture as it was before this effect ran (read by the combine pass) |

Both LINEAR, CLAMP, no mips.

### Constants (registered by name, no fixed registers)

- TESR_BloomLegacyData: x Luminance, y MiddleGray, z WhiteCutOff (the current category).
- TESR_BloomLegacyValues: x BloomIntensity, y OriginalIntensity, z BloomSaturation, w OriginalSaturation.
- TESR_ReciprocalResolution: 1 / width and 1 / height of the render (texels).

## 4. Settings

[Shaders.BloomLegacy.Status]: Enabled (bool, default false).

[Shaders.BloomLegacy.Exteriors] and [Shaders.BloomLegacy.Interiors] (same keys; defaults in brackets). Unchanged:
- Luminance [0.06]: the bright pass multiplies the picture by MiddleGray / (Luminance + 0.001).
- MiddleGray [0.18]: the mid-grey level the bright pass maps to.
- WhiteCutOff [0.8]: the boost in the bright pass is (1 + v / WhiteCutOff^2); a smaller value boosts bright values more.
- BloomIntensity [1.4]: gain of the glow when added back.
- OriginalIntensity [1.0]: gain of the picture the glow is added to.
- BloomSaturation [1.2]: saturation of the glow (1 unchanged, 0 grey).
- OriginalSaturation [1.0]: saturation of the picture before the glow is added.

No night category.

## 5. Passes and outputs

One technique (index 0) with four passes. Each pass reads the current picture and writes into the frame chain; the last pass may write
the game target directly when it is the chain's final effect.

1. Bright pass. Per channel: v = c * MiddleGray / (Luminance + 0.001); v' = v * (1 + v / WhiteCutOff^2); output (v' - 5) / (10 + max(v', 0)).
   Negative for most of the picture (about -0.5 for a dark value), zero at v' = 5, approaching 1 for very bright values. Not clamped.
2. Horizontal blur. Reads the bright-pass result. A 13-tap separable blur along x (offsets -6..6 texels), weights a Gaussian of sigma 2.1
   texels, normalised by their own sum. The centre weight is about 0.19 and the outer taps about 0.003.
3. Vertical blur. The same kernel along y, reads pass 2.
4. Combine. Reads the blurred glow (the chain's current picture) and the source picture (TESR_SourceBuffer):
   - glow g = saturate(blurred), so negative blur values become 0 and values above 1 are capped at 1;
   - glow colour = lerp(luma(g), g, BloomSaturation) * BloomIntensity;
   - original colour = lerp(luma(o), o, OriginalSaturation) * OriginalIntensity, where o is the source pixel;
   - darkening = max(max(o, 1) - g, 0) per channel;
   - output = original colour * darkening + glow colour, alpha 1.

Output: the combined picture in the chain (or the game target), same size as the render, float format of the chain. Opaque.

## 6. Behaviour details

- Negative values are part of the blur on purpose: dark areas give negative values in pass 1, which the blur averages with the bright
  ones, so a dark surround lowers the glow near bright sources. Clamping before the blur would change the look.
- The darkening uses max(o, 1), so a picture above 1 is darkened by its own brightness; on a normal LDR picture it is 1 - g.
- No sky, depth, first-person or TAA-jitter handling.
- HDR vs LDR: the input is the tonemapped picture. Pass 1 has no upper clamp; the combine caps the glow at 1.
- NaN and infinity: pass 1 divides by (10 + max(v', 0)), at least 10. The Luminance term carries +0.001. WhiteCutOff is clamped to at
  least 0.001 before it is squared and divided by (wave 3 fix).
- RenderScale (rect frames, UNOFFICIAL): every blur tap is kept inside the drawn part (clampToRectIfOn with the full-frame texel), so a
  tap cannot read the stale part of the target (wave 3 fix; D3D9 only, see section 7).
- Enabling this with Bloom on adds the two glows; nothing sets one off when the other is on.

## 7. Vulkan path

- Choice: [Main.Main.Performance] NvrDxvkPasses (default true) with the Vegas Shaders DXVK extension. The effect's ExtRender runs the four passes
  when the flag is on and the effect has not failed. All pipelines are made before any pass is drawn, so a refused pipeline leaves the
  frame to the D3D9 technique. The Vulkan path needs technique 0 and the source buffer to be the chain's source surface.
- Files (src/hlsl/NewVegas/Compute): BloomLegacyBloom.frag (pass 1); BloomLegacyBlur.frag (passes 2 and 3, direction from the push block);
  BloomLegacyCombine.frag (pass 4; binding 0 the blurred glow, binding 1 the source picture).
- Push blocks:
  - Bright pass: 0 TESR_BloomLegacyData (vec4); 16 sampler heap index (uint).
  - Blur pass: 0 TESR_ReciprocalResolution (vec4); 16 offset mask (vec2: (1, 0) or (0, 1)); 24 sampler heap index (uint).
  - Combine pass: 0 TESR_BloomLegacyValues (vec4); 16 sampler heap index (uint).
- The blur weights and the combine are the same maths as the D3D9 passes.
- No RenderScale rect steps on the Vulkan path.

## 8. Look and cost

- Correct result: bright lights bleed a soft, slightly saturated halo over nearby surfaces, and the surface right around the light is
  slightly darker than before. At the defaults the halo is noticeable but not a haze over the scene.
- Typical wrong results:
  - A grey or dark ring around bright lights: the darkening is too strong (OriginalIntensity or BloomIntensity mismatch).
  - A glow over dark areas: MiddleGray too high or Luminance too low.
  - Hard glow edges: the taps wrong (they must be one texel apart).
  - A glow that never reaches far: WhiteCutOff too high.
- Cost class: multi-pass at full resolution. Pass 1 is one read; passes 2 and 3 are 13 reads each; pass 4 is two reads. Likely the heavier
  of the two blooms per frame; not measured.

## 9. Open questions

Changes made in wave 3 (2026-11-02):
- FIXED: the blur taps are clamped to the drawn part in a rect frame (RenderScale).
- FIXED: WhiteCutOff is floored at 0.001 (no divide by zero).
- CHANGED (look may shift a little): the blur kernel is a normalised Gaussian of sigma 2.1 texels, not the old kernel's values. The spec's
  shape (symmetric, peak about 0.2, ends about 0.002, sum about one) is matched at the centre (0.19) and the ends are about 45 percent
  higher (0.0032); the exact old values
  were not copied. A tester look check is needed.

Still open:
1. Both Bloom and BloomLegacy can be enabled; no check stops it, and no note says what the intended combination is.
2. Night: there is no night section; the Exteriors values are used at night. This is by design in the code, but the toml does not say so.
3. Vulkan path: no rect steps (section 7). Not checked in a rect frame.
