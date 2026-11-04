# Exposure

Source of this spec: the rewritten shaders (wave 3 buffers, branch claude/rw3-buffers): src/hlsl/NewVegas/Effects/Exposure.fx.hlsl (D3D9) and src/hlsl/NewVegas/Compute/Exposure.frag (Vulkan). The C++ (src/effects/Exposure.cpp) is unchanged. Written from the previous spec and the C++; the old shader was not read.

## 1. Purpose

Exposure is NVR's automatic brightness correction. It measures how bright the frame is (through the AvgLuma result) and darkens or brightens the whole picture toward a pivot brightness, so that a dark interior, a bright exterior at noon or a night scene do not stay at the same level. The picture settles gradually; the correction is one multiplication of the frame's linear colour, so colours keep their hue.

## 2. When it runs

- In RenderEffectsPreTonemapping, in the HDR block, after AvgLuma and before bloom, lens and the LUT.
- Enabled: [Shaders.Exposure.Status] Enabled (default true).
- Skipped when SkipIdentityExposure is on and MaxBrightness (TESR_ExposureData.y) is exactly 0 (the usual night case). The C++ skips the draw; nothing changes in the shader.
- Each frame, UpdateConstants marks AvgLuma as required; the four constants are recomputed on a settings or day-light change.

## 3. Inputs

### Textures

- TESR_RenderedBuffer (stage 0, LINEAR, CLAMP): the frame before exposure.
- TESR_AvgLumaBuffer (stage 1, LINEAR, CLAMP): the 1 x 1 texel from AvgLuma. Only its G channel (the animated average luma) is read.

### Constants

- TESR_ExposureData (float4): x = MinBrightness (the pivot), y = MaxBrightness (the strength), z and w = adaptation speeds (read by AvgLuma, not here).

## 4. Settings

Unchanged: [Shaders.Exposure.Main] MinBrightness (0.2), MaxBrightness (0.5), DarkAdaptSpeed (0.2), LightAdaptSpeed (0.2), Histogram (false), HistogramLowPercent (10), HistogramHighPercent (5); [Shaders.Exposure.Night] (MinBrightness 0.3, MaxBrightness 0); [Shaders.Exposure.Interiors] (MinBrightness 1.0, MaxBrightness 0.3); [Shaders.Exposure.Status] Enabled; [Main.Main.Performance] SkipIdentityExposure.

## 5. Passes and outputs

### Pass (technique unnamed, one pass, PS ExposurePS)

- Target: the chain's spare texture or the render target, full frame, A16B16G16R16F.
- Output: RGB = the corrected colour, delinearised (the input is linearised first, the correction is applied in linear, and the result is delinearised again). Alpha = 1.

## 6. Behaviour details

### The correction

Let A = AvgLuma's G, m = MinBrightness, M = MaxBrightness, L = the linear colour.
- A and m are first floored at 1e-6 (new guard; see section 9).
- up = max(ln(A / m), 0) + 1, down = max(ln(m / A), 0) + 1, F = up / down.
- Output (linear) = lerp(L, L / F, M). Then delinearised. Alpha 1.

A screen brighter than the pivot is darkened by a factor that grows with the log of the ratio; a darker one is lifted the same way. The factor is the same for R, G and B, so the hue is kept.

### Guards (changed)

- Divide by zero and ln(0): a black frame drives A toward zero. A is now floored at 1e-6, so ln and the division stay finite and the output is finite (black stays black). Before, the shader had no guard.
- A pivot of 0 (not a default) is floored the same way.

### Adaptation

Not in this shader: AvgLuma's G moves toward the new average by at most dt x speed per frame.

### Interaction

Bloom, lens and the LUT run after it. Fog, AO and the sky effects run before it.

## 7. Vulkan path

- Exposure.frag: the same formula line for line, with the same floors. Set 1 binding 0 = the frame (texture size from textureSize), binding 1 = the AvgLuma texel (read at (0.5, 0.5)). Push: offset 0 = exposure data (x pivot, y strength), offset 16 = the sampler heap index (linear, clamp).
- The C++ skip (MaxBrightness 0) applies before either path.

## 8. Look and cost

Correct: a dim room lifts toward the interior pivot without a flash; a bright sky is pulled down gently; hue kept; changes take a second or two.

Typical wrong: pumping (adaptation too fast, see AvgLuma); a black or grey patch after a long black screen (the floor keeps it black now); washed-out colours (strength too high).

Cost: one full-screen pass, two reads per pixel. Skipped at night with the default MaxBrightness 0.

## 9. Open questions

1. Fixed: the black-frame guard (floor 1e-6 on A and the pivot). Not checked on a GPU.
2. Constant refresh on an interior/exterior switch with no day-light change: not traced in this pass (C++).
3. The defaults file describes MinBrightness and MaxBrightness as thresholds; the code and this shader use a pivot and a strength. Kept as the code does.
4. TESR_DebugVar is not read.
5. No clamp after the correction; a large negative exposure could exceed the half-float range. Not checked.
6. Night MaxBrightness 0 disables the pass; kept.

## 10. GT7 adaptation ([Shaders.Exposure.Main] Mode 1)

An alternative to the classic correction above, chosen with `Mode` (0 = classic, the default; 1 = GT7). Everything in sections 1-9 is Mode 0 and is unchanged: Mode 1 draws technique 1 ("GT7") of both AvgLuma.fx and Exposure.fx instead of technique 0.

### What it does

Auto-exposure and auto white balance after the design Polyphony Digital presented in "Driving Toward Reality: Physically Based Tone Mapping and Perceptual Fidelity in Gran Turismo 7" (SIGGRAPH 2025, slides 188-222):
- Two adaptation stages: short-term (pupil and neural gain: a few EV in seconds) inside the range of a long-term one (receptor sensitivity: minutes). Light adaptation is faster than dark adaptation.
- A light adaptation limit: scenes brighter than it get no correction, so full sun stays as the Tonemapping Exposure set it ("mildly glaring"). The limit is defined after the Tonemapping Exposure (display-referred) and converted back with the Exposure in use, so it follows every setup's calibration. The dark end is 3 EV below it, so nights stay dark.
- Metering of the light reaching the eye from the whole field of view outdoors (cosine-weighted), not only the screen: the rendered sky dome (Sky's spherical-harmonic projection, TESR_SkyIrradiance), whether the sky is open around the camera (the top-down height map Skylighting uses, TESR_OrthoMapBuffer), the sun shadow at the camera and the lit and shaded levels seen on screen (TESR_ShadowAtlas, one cascade lookup). Falls back to the screen plus the sky on screen where those maps are missing (interiors, sun shadows off).
- Auto white balance toward D65 from luminance-weighted references (the frame and the weather's sun, sky and ambient colours), steadied by a faint virtual light, partly blended with the ideal light and limited to a camera-like range (2500-10000 K, small tint).
The slides give the structure, not the numbers: every value marked ESTIMATE in Effects/Includes/GT7Adaptation.hlsl is this port's choice.

### Settings (per Main / Night / Interiors, blended as the other values)

- `GT7AutoExposure` [0-1], 1: auto-exposure strength.
- `GT7WhiteBalance` [0-1], 1: white balance strength.
- `GT7WeatherLight` [0-1], 1 outdoors / 0 indoors: how much the weather's lights count in the white balance against the screen's colour.
- MinBrightness, MaxBrightness and the adapt speeds are not used in Mode 1. Calibrate the Tonemapping Exposure once in full midday sun.

### Constants

- TESR_GT7AdaptData (float4): x GT7AutoExposure, y GT7WhiteBalance, z GT7WeatherLight, w Mode.
- Also read: TESR_HDRData.y (the Tonemapping Exposure, for the limit), TESR_SunColor, TESR_SkyColor, TESR_SunAmbient, TESR_SunDirection.

### Pass (technique GT7, PS ExposureGT7PS)

Reads the frame and AvgLuma's GT7 state (r: short-term level, a: colour temperature in mired); white-balances the linear colour (Bradford, toward D65, tint from the weather lights), then multiplies it by 2^(max(limit - r, 0) x GT7AutoExposure). Delinearised, alpha 1. Until the state is initialised (a below 50) the colour passes unchanged.

### C++

- EffectChain draws Exposure::Technique() for both effects; SkipIdentityExposure skips the pass in Mode 1 only when both strengths are 0 (Exposure::IsIdentity).
- When Mode changes, the AvgLuma texel is cleared so neither technique reads the other's state (it re-initialises on the next frame).
- Vulkan: no port; Exposure::ExtRender returns false for technique 1, so the D3D9 pass draws it under NvrDxvkPasses too. AvgLuma::RenderCompute stands aside in Mode 1 (AvgLumaCompute and Histogram are not used).

### Look and cost

Correct: full sun unchanged; shade under open sky brightened a little, held back by the sky; walking indoors brightens by about 2 EV within seconds and up to 3 EV over minutes; walking out gives a short glare; nights brighten at most 3 EV; blue moonlight and warm interiors partly neutralised.
Cost: AvgLuma's GT7 pass is one pixel (about 1070 ps_3_0 slots: 100 screen samples, 32 field directions with three height-map lookups each, a few shadow lookups); the Exposure pass is one full-screen pass, about 200 slots.

### Open questions

1. Not tested on a GPU in this form (written against 1.0.2, both effects compile with d3dcompiler_47 for fx_2_0 / ps_3_0, technique 0 compiles to the same code as before).
2. Debug's view of AvgLuma reads r as a luma; in Mode 1 it is a log2 level.
