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
