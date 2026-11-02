# AvgLuma

Source of this spec: the rewritten shaders (wave 3 buffers, branch claude/rw3-buffers): src/hlsl/NewVegas/Effects/AvgLuma.fx.hlsl (D3D9 pass) and src/hlsl/NewVegas/Compute/AvgLuma.comp (exact reduction). AvgLumaHistogram.comp is kept from before (not rewritten here). The C++ (src/effects/AvgLuma.cpp) is unchanged. Written from the previous spec and the C++; the old shader was not read.

## 1. Purpose

AvgLuma measures how bright the picture is, and where the focus of the depth of field should sit, and keeps both smoothed over time. Its result is a single 1 x 1 texel. Exposure reads the brightness to decide how much to darken or brighten the frame, and the depth of field effect reads the focal distance. The player sees no image of AvgLuma itself: the result shows as the picture slowly brightening or darkening, and the depth of field's focus changing when the view changes.

## 2. When it runs

- In RenderEffectsPreTonemapping, in the HDR block, after AO, ambient light and fog, before exposure. It measures the picture as it is at that point.
- Only when avglumaRequired is set (Exposure sets it every frame), so in practice whenever Exposure is loaded.
- Compute path: when AvgLumaCompute or the Exposure Histogram is on and NVR DXVK is present, and the frame is not in RenderScale v2 rect mode (a rect frame always takes the D3D9 pass). Otherwise the D3D9 pass.

## 3. Inputs

### Textures

- TESR_RenderedBuffer (stage 0, LINEAR, CLAMP): the frame before exposure. Read at 112 positions (section 6).
- TESR_AvgLumaBuffer (stage 1, LINEAR, CLAMP): the same 1 x 1 texture the pass writes. Its previous value is read (section 9).
- TESR_DepthBuffer (stage 2, LINEAR, CLAMP): CombineDepth's output. Read at the centre and 12 taps.

### Constants

- TESR_MotionBlurData (xy): camera rotation speed (focus search spread).
- TESR_DepthOfFieldData (x): hyperfocal distance in thousands of game units.
- TESR_GameTime (w): frame time, seconds.
- TESR_ExposureData (z dark adaptation, w light adaptation; x and y are Exposure's).
- TESR_CameraData (y): far plane (the depth read's scale).
- TESR_ReciprocalResolution (xy): for the rect clamp of the taps.
- TESR_RenderScale (RenderRect include): the drawn part.

## 4. Settings

Unchanged:
- [Main.Main.Performance] AvgLumaCompute (bool, false; UNOFFICIAL, needs NVR DXVK).
- [Main.Main.Performance] AvgLumaComputeCheck (int, 0; test tool).
- [Shaders.Exposure.Main] Histogram (bool, false; needs NVR DXVK), HistogramLowPercent (10), HistogramHighPercent (5).

## 5. Passes and outputs

### D3D9 pass (technique unnamed, one pass, PS AvgLumaPS)

Target: TESR_AvgLumaBuffer, 1 x 1 A16B16G16R16F. Output, all four channels:
- R = newLuma: (average + 7 x previous R) / 8.
- G = animatedLuma: the previous G moved toward newLuma by at most dt x DarkAdapt down or dt x LightAdapt up (dt = TESR_GameTime.w).
- B = focus: the focal distance as a fraction of the hyperfocal distance, 0 to 1.
- A = 1.

### Compute pass (AvgLuma.comp, two dispatches)

Same four values, same smoothing, adaptation and focus. The average is the exact mean (every pixel), not the 112-sample mean.

## 6. Behaviour details

### The average (into R)

- D3D9: 112 samples of linear colour, each read at level 0: a 10 x 10 grid at 0.05, 0.15 ... 0.95 of the drawn part, plus 12 taps on a golden-angle spiral out to the frame edge (offsets from the centre, kept on the drawn part's edge). The mean linear colour's BT.709 luminance is the average. (The exact 12 offsets were not in the old spec; this spiral is the chosen set.)
- Compute: every pixel's linear BT.709 luminance, tile partial sums (16 x 16), then one group; divided by the pixel count.
- The average is always the mean of linear luminance.

### Smoothing and adaptation

- newLuma = (average + 7 x previous R) / 8.
- Adaptation as in section 5. Dark: delta < 0, moved by max(delta, -dt x DarkAdapt). Light: moved by min(delta, dt x LightAdapt).

### Focus (B)

- speed.xy = saturate(|motion.xy|) + 0.1.
- Centre depth c (game units). Twelve taps at 0.3 x tap offset x speed; depth sum = c + the twelve depths; meanDepth = sum / 12 (kept as written: 13 terms over 12, a small upward bias).
- depth = c if c > meanDepth (iron sights: the centre wins when it is the farther surface), else meanDepth. depth = saturate(depth / max(1e-7, hyperfocal x 1000)).
- step = pow(max(old B, depth), 0.1). B moves toward depth by at most 3 x step x dt nearer, 1 x step x dt farther.

### Edge behaviour

- Rect frames: the average and the taps are on the drawn part (the tap positions are clamped to it).
- Sky (far) gives focus 1.
- Guards: hyperfocal floored at 1e-7 (existing). pow(0, 0.1) is 0, no NaN.
- The texture starts at zero; the first frames rise from zero.

## 7. Vulkan path

- AvgLuma.comp (new): set 1 binding 0 = the frame (texelFetch), 1 = the depth (textureLod through the heap sampler), 2 = partial sums (float array), 3 = state in, 4 = state out. Push block as the C++ fills it (128 bytes; the phase at 16, the sampler index at 20, motion at 32, depth of field at 48, game time at 64, exposure at 80, camera at 96, histogram at 112 unused).
- The state buffer (32 bytes, shared with AvgLumaHistogram.comp): offset 0 vec4 cur; offset 16 two uints (cur as four half floats: the texel's 8 bytes); offset 24 the average (float); offset 28 share (float, set to 1).
- The Vulkan focus taps use screen uv without the rect clamp (the compute path is not used in rect frames).
- AvgLumaHistogram.comp is kept and must use this same state layout. Not checked against its source in this pass (see section 9).

## 8. Look and cost

Correct: exposure drifts (no flicker from frame to frame); focus pulls in quickly and relaxes slowly; a sun disc does not drag the whole exposure (histogram on).

Typical wrong: flicker from a blinking light (adaptation too fast, or an exact average with a small bright source); exposure stuck after a cut (adaptation too slow); focus jumping when a tap lands on another surface.

Cost: D3D9: a 1 x 1 target with 112 samples and 13 depth reads (cheap). Compute: two dispatches reading every pixel (moderate).

## 9. Open questions

1. Feedback hazard, open: the D3D9 pass reads TESR_AvgLumaBuffer and writes the same 1 x 1 texture. The C++ provides one target for this effect. The read returns the previous value only if the driver keeps it; this is not guaranteed. Needs a second 1 x 1 target (ping-pong) from the C++ to be safe.
2. The 13/12 focus bias is kept as written.
3. The 8-frame average is a one-eighth exponential average, not a true mean.
4. The 12 tap offsets of the D3D9 average are the spiral above; the old offsets were not recorded in the spec.
5. AvgLumaHistogram.comp (kept) must agree with the state layout in section 7. Not verified.
6. The exact average (compute) and the 112-sample average (D3D9) still differ, as before; the D3D9 pass keeps its sampling.
7. Histogram bin layout (126 log2 steps) is unchanged; it lives in the kept histogram shader.
