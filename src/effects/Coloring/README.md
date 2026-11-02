# Coloring

Source reviewed (wave 3, 2026-11-02): the new D3D9 effect src/hlsl/NewVegas/Effects/Coloring.fx.hlsl; the new Vulkan pass
src/hlsl/NewVegas/Compute/Coloring.frag (one file for both techniques) (branch claude/rw3-colour); C++ src/effects/Coloring.cpp,
Coloring.h, LUT.h (unchanged); settings in resource/VegasShaders.defaults.toml and the section reader in
src/core/SettingManager.cpp; the frame order and the combined-LUT choice in src/core/ShaderManager.cpp (line ~2826).

## 1. Purpose

Coloring is the colour grade of the picture: the overall look of a place (saturation, contrast, gamma, a bleach-bypass style
overlay, colour curves and a split tone between shadows and highlights). It works on the tonemapped picture. Each cell, or
worldspace, can have its own grade through a settings section, with a Default fallback. When the LUT effect is also running,
Coloring can apply the LUT in the same pass.

## 2. When it runs

- Position: post-tonemap, in the screen-space colour block of RenderEffects (ShaderManager.cpp). The order is: rain, snow,
  BloomLegacy, heat haze, volumetric smoke, barrel haze, then Coloring, then LUT (unless the LUT is combined into Coloring or runs
  before tonemapping), then depth of field, motion blur, lens effects, DitherBuster, SMAA, FXAA, sharpening, cinema, image adjust,
  bodycam, sky dither and debug.
- Enabled: [Shaders.Coloring.Status] Enabled (bool, default false).
- Gates: [Main.Main] RenderEffects on, a player cell present, overlay off. No ShouldRender override.
- Combined with LUT: the C++ asks for technique 1 (ColoringLUT, looked up by name in ShaderManager.cpp) when Coloring and LUT both
  render this frame and the LUT is not set to run before tonemapping. In that case the LUT is not run as its own pass.
- Per-location settings (the section is chosen per frame, unchanged): the section named after the player's current cell editor name;
  when outdoors only, the worldspace editor name; then the section named Default. Any [Shaders.Coloring.<name>] section counts except
  Status.

## 3. Inputs

### Textures

| Slot | Name | Holds |
|---|---|---|
| s0 | TESR_RenderedBuffer | the current picture (tonemapped) |
| s1 | TESR_LUTDayBuffer | day LUT (technique 1 only) |
| s2 | TESR_LUTNightBuffer | night LUT (technique 1 only) |
| s3 | TESR_LUTInteriorBuffer | interior LUT (technique 1 only) |

All LINEAR, CLAMP, no mips.

### Shader constants (registered by name; no fixed registers)

| Name | Components |
|---|---|
| TESR_ColoringColorCurve | x ColorCurve (the whole grade's S-curve); y, z, w ColorCurveR, G, B (the tone mask's S-curves) |
| TESR_ColoringEffectGamma | x EffectGamma (tone mask); y, z, w EffectGammaR, G, B |
| TESR_ColoringData | x Strength; y BaseGamma; z Fade; w Contrast |
| TESR_ColoringValues | x Saturation; y Bleach; z BleachLuma; w Linearization |
| TESR_LUTData, TESR_LUTBlend | technique 1 only: the LUT effect's values (see the LUT spec, section 3) |

## 4. Settings

All keys are floats under [Shaders.Coloring.<section>]. Defaults are the [Shaders.Coloring.Default] values. Unchanged.

- Strength (0.5), BaseGamma (1.6), Fade (0.0), Contrast (0.8), Saturation (-0.1), Bleach (0.1), BleachLuma (0.2), ColorCurve (1.2),
  ColorCurveR (0.9), ColorCurveG (1.0), ColorCurveB (1.0), Linearization (1.1), EffectGamma (0.9), EffectGammaR, G, B (1.0).
- [Shaders.Coloring.Status] Enabled (bool, default false).

## 5. Passes and outputs

Two techniques, one pass each, both full-screen quads, writing the frame chain's target (or the game target when final).

- Technique 0 (Coloring; the C++ asks for index 0): the grade of TESR_RenderedBuffer. LDR output, alpha 1.
- Technique 1 (ColoringLUT; the C++ looks it up by name): the same grade, then the LUT grade (day, night, interior, strength,
  HDR compatibility) in the same pass, reading slots s1 to s3.

## 6. Behaviour details

The grade (ten steps, in order). The old spec gave these steps in words only; the formulas below are the new shader's (wave 3):

1. Overlay: the picture is blended toward Overlay(picture, Rec.709 luma of the picture) (the standard overlay: dark base 2 b l, light
   base 1 - 2 (1 - b)(1 - l)). The blend weight is BleachLuma times smoothstep(0.45, 1, luma). The overlay result is capped at 65504.
2. Linearization: sign-preserving power by Linearization.
3. Contrast: the picture is blended toward a floor of 0.01 by Contrast (Contrast 1 is unchanged).
4. Tone mask: m = max(dot(c, (-0.9689, 1.8758, 0.0415)), 0) (the green row of the XYZ-to-RGB matrix; the spec's "not Rec.709" luminance).
   Per channel m_c = SCurve(m, ColorCurve_c), then m^(1/EffectGamma), then m -> lerp(m, 1 - m, Bleach), then m^(1/EffectGamma_c). Values
   are clamped to 0..1 before the inverse.
   SCurve(x, k) = (s(x) - y0) / (y1 - y0) with s(x) = 1 / (1 + exp(-k (x - 0.5))), y0 = s(0), y1 = s(1). It is the identity at zero
   steepness (|k| below 0.001), which avoids the 0 / 0 of the normaliser.
5. BaseGamma: sign-preserving power by 1 / BaseGamma. The picture is kept as beforeSplit for step 10.
6. Split tone: for each channel, a value below the mask 0.5 moves toward its square, one above toward its square root, by an amount that
   grows with the distance of the mask from 0.5 (linear: amount = 2 |m - 0.5|, clamped). Sign is kept. Blended over the picture by Strength.
7. The whole-grade S-curve (ColorCurve) per channel.
8. Channel mixing: Saturation pulls each channel toward the mean of the three (lerp to the mean by Saturation; negative values separate the
   channels). Fade then mixes green and blue toward their mean by Fade (clamped 0..1).
9. Second split tone with the mask set to the Rec.709 luma of the mixed picture, then the sign-preserving power 1 / Linearization.
10. Output: beforeSplit (step 5) blended toward the step 9 result by Strength. Not clamped.

Edge cases the shader handles:
- ColorCurve and the channel curves at zero: identity (the NaN bug of the old grade, fixed in the old code and kept fixed here).
- Gamma settings (EffectGamma, EffectGammaRGB, BaseGamma, Linearization) are clamped to at least 0.01 before a division or a power, so a
  zero setting cannot divide by zero (wave 3 fix).
- Negative values: powers and the split tone keep the sign. The tone mask uses max(0).
- The overlay result is capped at 65504 before the blend.
- Output is not clamped at the end. Values outside 0..1 are passed to the next stage.

Other behaviour:
- Per-location: the grade changes instantly on entering or leaving a cell or worldspace. No blend between sections.
- Sky, depth, first-person weapon, TAA jitter: not handled.
- HDR vs LDR: the tonemapped (LDR) picture.
- RenderScale (rect frames): no rect handling (the grade is per pixel; there are no neighbour reads).
- Technique 1 relies on the LUT effect's constants and textures being set for the frame.

## 7. Vulkan path

- Choice: [Main.Main.Performance] NvrDxvkPasses (default true) with the NVR DXVK extension. Both techniques use the same file, and the
  push flag lutPass selects the LUT (1 for ColoringLUT). If the pass is refused, the D3D9 technique runs for the session.
- File: src/hlsl/NewVegas/Compute/Coloring.frag (the same maths as the D3D9 effect, line for line).
- Bindings (set 1): binding 0 = the picture; bindings 1, 2, 3 = the day, night and interior LUT textures, read only when lutPass is 1.
  Missing LUT textures are bound to the picture.
- One LINEAR CLAMP sampler, its heap index in the push block.
- Push block (104 bytes): 0 TESR_ColoringColorCurve; 16 TESR_ColoringEffectGamma; 32 TESR_ColoringData; 48 TESR_ColoringValues; 64
  TESR_LUTData; 80 TESR_LUTBlend; 96 sampler heap index (uint); 100 lutPass (uint: 1 = ColoringLUT, 0 = grade alone).

## 8. Look and cost

- Correct result: a subtle grade of the whole scene: slightly desaturated at the default, slightly lower contrast, softened shadows and
  highlights. Strength 0 gives the original picture exactly.
- Typical wrong results:
  - A flat, grey or washed look: Contrast or Saturation in the wrong direction, or Bleach too high.
  - Black or NaN pixels: a zero curve or gamma (guarded in wave 3).
  - Banding in shadows: Linearization or BaseGamma too far from 1 for an 8-bit output.
  - Strong colour shifts between two places: a missing per-location section, so Default applies.
- Cost class: single pass, ALU-heavy (several exponentials and powers per pixel); technique 1 adds three LUT samples and their strip
  maths. No extra render targets.

## 9. Open questions

Changes made in wave 3 (2026-11-02): the formulas of steps 1, 4, 6, 8 and 9 are the new shader's choices, because the old spec gave
them in words only. The look is therefore re-derived. A tester look check is needed before this replaces the old grade. The changed
choices are:
- Overlay: standard overlay of the picture with its luma (weight BleachLuma times a smoothstep above 0.45).
- S-curve: the normalised sigmoid in section 6, step 4.
- Split tone: the amount is linear in the mask's distance from 0.5.
- Fade: green and blue are moved toward their mean (not swapped).
- FIXED: divide-by-zero guards on the gamma settings.

Still open:
1. Output range: the grade is not clamped; the next stage decides. Not checked.
2. Per-location sections: the table is read when settings load. Whether a section added while playing is picked up without a restart was
   not checked.
3. Interior cells with no matching section do not try the worldspace name (by the code). Not checked against intent.
4. Two luma definitions are used: Rec.709 for the overlay and the XYZ green row for the tone mask. Both kept, from the old spec.
5. Technique 1 needs the LUT's constants and textures set for the frame. The order of the LUT update relative to the Coloring pass is in
   ShaderManager.cpp and was not traced again here.
6. Strength is applied twice (steps 6 and 10). Kept; whether the double use is intended was not established.
7. The toml section [Shaders.Coloring.Status] has no description for Enabled.
