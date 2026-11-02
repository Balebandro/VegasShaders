# DitherBuster

Source reviewed (wave 3, 2026-11-02): the new D3D9 effect src/hlsl/NewVegas/Effects/DitherBuster.fx.hlsl with its shared include
src/hlsl/NewVegas/Effects/Includes/DitherNoise.hlsl and Includes/RenderRect.hlsl; the new Vulkan passes
src/hlsl/NewVegas/Compute/DitherBuster.frag and DitherBusterStable.frag (branch claude/rw3-colour); C++ src/effects/DitherBuster.cpp,
DitherBuster.h (unchanged); settings in resource/VegasShaders.defaults.toml; frame order in src/core/ShaderManager.cpp.
The toml comments describe an edge-blending pass (MaskPower, Radius) that no technique calls. The new shader does not implement it.

## 1. Purpose

DitherBuster removes the "dither" artefact that alpha-tested geometry (grass, foliage, fences and similar) produces under DXVK:
single pixels that flicker between the object's colour and the background in a checkerboard-like pattern. Each such isolated pixel
is blended toward the average of itself and its four neighbours, so the dithered coverage becomes solid. Ordinary edges are left
alone, and SMAA later in the chain handles them. An optional soft gate and a de-dither judgement reduce frame-to-frame twinkle on
still pictures.

## 2. When it runs

- Position: post-tonemap, in the lens group of RenderEffects (ShaderManager.cpp): after the blood lens, water lens and low
  high-frequency effect, and before SMAA, FXAA and sharpening.
- Enabled: [Shaders.DitherBuster.Status] Enabled (bool, default true).
- Gates: [Main.Main] RenderEffects on, a player cell present, overlay off. No ShouldRender override.
- Technique choice (made each frame by the C++, TechniqueIndex): technique 1 when the soft gate is above 0, or DeDither is on, or
  DebugView is not 0; otherwise technique 0. With all three options off the plain pass runs.
- Interior and exterior: no separate sections.

## 3. Inputs

### Textures

| Slot | Name | Holds |
|---|---|---|
| s0 | TESR_RenderedBuffer | the current picture (LDR, tonemapped, after the earlier post effects) |

LINEAR, CLAMP, no mips. The depth sampler the old shader declared at slot s1 is gone (no technique read it; the C++ never bound it).

### Constants (registered by name; no fixed registers)

- TESR_ReciprocalResolution: 1 / width and 1 / height of the render (texels).
- TESR_DitherBusterData (float4): x Strength (used clamped to 0..1); y MaskPower (not read by either technique); z DitherThreshold
  (0 turns the isolation step off); w Radius (not read by either technique).
- TESR_DitherBusterGate (float4; technique 1 only): x GateSoftness (0..1); y DeDither flag (above 0.5 on); z DebugView flag (above
  0.5 on); w unused.
- TESR_ToneMappingClip (float4; technique 1 only; set by the tonemap effect): y the tonemap's dither size (Sky Dither strength / 255,
  0 when off); z its frame counter. x and w are not read.

## 4. Settings

[Shaders.DitherBuster.Main] (no night or interior sections). Unchanged by the rewrite:
- Strength (float, default 1.0): how far an isolated pixel moves toward its neighbourhood average.
- MaskPower (float, default 0.5): NOT USED by either technique (section 9).
- DitherThreshold (float, default 0.08; 0 disables): the isolation threshold in luma units.
- Radius (float, default 1.0): NOT USED by either technique (section 9).
- GateSoftness (float, default 0.0; clamped 0..1): 0 is a hard step at DitherThreshold; above 0 the weight ramps smoothly from
  DitherThreshold * (1 - softness) to DitherThreshold * (1 + softness).
- DeDither (bool, default false): judge the isolation test on the picture with the tonemap's dither noise taken out (the decision
  only). Needs Sky Dither with the AtToneMap option; without it the noise size is 0 and DeDither changes nothing.
- DebugView (int, default 0): 1 shows the picture with the recomputed tonemap noise removed, for checking DeDither.

[Shaders.DitherBuster.Status]:
- Enabled (bool, default true): "Reduces dithering artifacts via luma-edge anti-aliasing."

## 5. Passes and outputs

Two techniques, one pass each. Full-screen quads through the frame vertex buffer, writing the frame chain's target (or the game
target when this is the chain's final effect). Output is the same size as the render, alpha 1.

- Technique 0 (DitherOnly; the C++ asks for index 0): the plain isolated-pixel pass (section 6).
- Technique 1 (DitherOnlyStable; the C++ asks for index 1 when the soft gate, DeDither or DebugView is on): the same pass with the
  soft gate, the de-dither judgement and the debug view.

## 6. Behaviour details

Live pass, in words (both techniques share the test):

1. Five taps: the centre and its four direct neighbours (left, right, up, down), one texel away. Each tap is kept inside the drawn part
   of a rect frame (clampToRectIfOn).
2. Luma: Rec.709 weights (0.2126, 0.7152, 0.0722).
3. Isolation: the sum over the four neighbours of |centre luma - neighbour luma|, minus four times the length of the central-difference
   gradient ((right - left) / 2, (up - down) / 2 in luma), floored at 0. On a step edge the two cancel (about 0); on a one-pixel
   checkerboard the gradient is 0 and the sum stays large, so the measure picks out dither and not edges.
4. Decision: with DitherThreshold above 0 and the isolation above it, the pixel is blended toward the five-tap mean by Strength (clamped
   0..1). Otherwise the pixel is written unchanged. Blending a pixel toward a mean of values already in the picture keeps the range.

Technique 1 only:
- Soft gate: with GateSoftness above 0 the weight is a smoothstep of the isolation from DitherThreshold * (1 - softness) up to
  DitherThreshold * (1 + softness). With softness 0 it is the hard step of technique 0. The blend amount is Strength times the weight.
- De-dither: the isolation is judged on the luma with the tonemap's dither noise removed at each tap's own whole-pixel position (the
  shared include DitherNoise.hlsl: the noise is the sum of two gradient noises minus one, times the dither size, removed in proportion
  to saturate(luma * 128)). Only the decision uses the corrected luma; the blend still uses the dithered picture.
- Debug view: the output is the centre with the recomputed noise removed (same proportion), clamped to 0..1. Blend and gate are not used.

Edge cases:
- DitherThreshold 0: the isolation step is off; the picture is unchanged (apart from the debug view).
- Strength 0: no blend.
- Thin bright or dark features (wires, grass blades) can have high isolation without being dither. The threshold, the soft gate and
  de-dither are the controls for this.
- The sky's own dither is one luma step (about 0.004), far under the default threshold.
- First-person weapon, TAA jitter, reversed depth: not handled.
- HDR vs LDR: LDR (tonemapped) input only.
- NaN and infinity: the isolation uses the length of a vector; the smoothstep is only used with an upper edge above the lower edge
  (threshold above 0 and softness above 0).

## 7. Vulkan path

- Choice: [Main.Main.Performance] NvrDxvkPasses (default true) with the NVR DXVK extension present. Technique 0 uses DitherBuster.frag,
  technique 1 DitherBusterStable.frag. A refused pass makes the D3D9 technique run for the session.
- Bindings (set 1): binding 0 = the picture. One LINEAR CLAMP sampler, its heap index in the push block.
- Push block for technique 0: 0 TESR_ReciprocalResolution (vec4); 16 TESR_DitherBusterData (vec4); 32 sampler heap index (uint).
- Push block for technique 1: 0 TESR_ReciprocalResolution; 16 TESR_DitherBusterData; 32 TESR_DitherBusterGate; 48 TESR_ToneMappingClip;
  64 sampler heap index (uint).
- Pixel position is floor(gl_FragCoord.xy), the same whole-pixel position as the D3D9 pass.
- No RenderScale rect steps on the Vulkan path (no rect constant in the push block).

## 8. Look and cost

- Correct result: alpha-tested grass and foliage edges look solid instead of speckled; straight edges, text and geometry edges are
  unchanged by this pass (SMAA does those). Strength 1 gives a full blend of isolated pixels.
- Typical wrong results:
  - Thin wires or fine detail softened: DitherThreshold too low.
  - Shimmering on a still scene: GateSoftness 0 with DeDither off (the hard step flips at the threshold).
  - Dither still visible: DitherThreshold too high, or Strength too low.
- Cost class: single pass, five texture reads per pixel, cheap. Technique 1 adds the noise recomputation for the five taps when DeDither
  is on (the toml records about 0.05 ms at its test size).

## 9. Open questions

Changes made in wave 3 (2026-11-02):
- FIXED: the neighbour taps are clamped to the drawn part in a rect frame (RenderScale), so no tap reads the stale part of the target
  (D3D9 only; see section 7).
- CHANGED: the unused depth sampler is no longer declared (no visible change; the C++ never bound it).

Still open:
1. MaskPower and Radius are not used by either technique. The toml describes an edge-blending pass that is not called by any technique.
   Neither value changes the picture. They are kept in the settings as they were and are not read by the new shader.
2. The toml recommends GateSoftness 0.5 for the grass case while the default is 0. Whether the default should change is a look decision.
3. The de-dither noise must match the tonemap's noise exactly. The shared include DitherNoise.hlsl is the reference; the tonemap spec
   should confirm the same constants.
4. The Vulkan passes have no rect steps (section 7). Not checked in a rect frame.
5. SMAA also has a DeDither option that uses the same tonemap noise; that effect is outside this spec.
