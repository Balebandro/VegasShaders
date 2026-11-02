# Bloom

Source reviewed (wave 3, 2026-11-02): the new D3D9 effect src/hlsl/NewVegas/Effects/Bloom.fx.hlsl and the new Vulkan passes
src/hlsl/NewVegas/Compute/Bloom.glsl and Bloom{DownFirst,Down,Up,UpLast}.frag (branch claude/rw3-colour); C++ src/effects/Bloom.cpp,
Bloom.h (unchanged); the tonemap hand-off in src/hlsl/NewVegas/Shaders/ISHDRBLENDINSHADERCIN.pso.hlsl (not edited); the tonemap constants
in src/effects/Tonemapping.cpp; the frame order in src/core/ShaderManager.cpp and src/NewVegas/Hooks/Render.cpp.

## 1. Purpose

Bloom is the soft glow that spreads out from bright light sources (lamps, neon, fire, the sun, bright sky) so they look
like they are shining into the lens. It is computed from the HDR scene before the game's tonemapper, as a stack of
progressively smaller blurred levels that are summed back up into one half-resolution glow image. The game's tonemap
shader then mixes that glow into the picture in place of the game's own bloom. Two other effects read the glow image
too: the dirt lens overlay (Lens) and the rain/precipitation effect. Bloom has no depth, sky or first-person masking:
the glow is taken from the whole scene.

## 2. When it runs

- Entry point: the pre-tonemap chain (ShaderManager::RenderEffectsPreTonemapping), called from the render hook in
  src/NewVegas/Hooks/Render.cpp. With [Main.Main.Misc] RenderPreTonemapping = true (the default) the chain runs on the
  HDR game surface before the game's image-space pass, which contains the tonemap. With it false, the chain runs on the
  output surface after image space.
- Order inside the chain: after the average-luma pass and the exposure pass (both can be skipped), before Lens and before
  any pre-tonemap LUT. The effect is rendered by the bloom block of that function, not by the generic post-effect path.
- Gates, in order: [Main.Main] RenderEffects must be on; the player must be in a cell; the pre-tonemap chain returns
  early while the overlay is on (terminal, lockpicking) and the menu background is ready. The effect itself is skipped
  when [Shaders.Bloom.Status] Enabled is off (default true), and then the glow buffer is cleared to black when it is
  switched off.
- Tonemap flag: the constant TESR_BloomExtraData.x is set to 1 when Bloom is enabled and the overlay is off (0 otherwise).
  The tonemap shader reads it to choose between the glow path and the game's own bloom path (section 5). The render hook
  sets the flag to 0 for the frames of the menu scene guard ([Main.Main.Performance] MenuSceneGuard).
- Interior, exterior, night: the settings section is chosen per frame from the game state through a transition value.
  Exterior: a blend between the Night section and the Main section, weighted by the day/night transition curve. Interior:
  the Interiors section outright.

## 3. Inputs

### Textures (the names the shader declares, their slots, and what they hold)

Slot order is fixed by the explicit register numbers in the shader, s0 to s8:

| Slot | Name | Holds |
|---|---|---|
| s0 | TESR_BloomBuffer | glow level 0: half of the render width and height, the level the tonemap reads |
| s1 | TESR_BloomBuffer2 | glow level 1: a quarter of the render size |
| s2 | TESR_BloomBuffer4 | glow level 2: an eighth |
| s3 | TESR_BloomBuffer8 | glow level 3: a sixteenth |
| s4 | TESR_BloomBuffer16 | glow level 4: a thirty-second |
| s5 | TESR_BloomBuffer32 | glow level 5: a sixty-fourth |
| s6 | TESR_BloomBuffer64 | glow level 6: a one-hundred-and-twenty-eighth |
| s7 | TESR_BloomBuffer128 | glow level 7: a two-hundred-and-fifty-sixth |
| s8 | TESR_RenderedBuffer | the current HDR scene (technique 0 only) |

- Level i is 1 / 2^(i+1) of the render size in each axis. The name suffix is 2^i (none for level 0).
- Format of every level: 16-bit float per channel, four channels (A16B16G16R16F). Values are not clamped to 0..1.
- All samplers: LINEAR, CLAMP, no mips.

### Shader constants (names, registers, components, units)

No fixed registers: the C++ registers them by name.

| Name | Components |
|---|---|
| TESR_BloomResolution | xy: size of the level being written (texels); zw: 1 / size |
| TESR_BloomData | x, y: upsample tent radius (uv units). z: PassBlending, 0..1. w: 1 / Passes (not read by the new shader) |
| TESR_BloomGlow | x: spread weight per wider level (1 + Spread, clamped 0.25..3). y: scale of the last upsample. z: GlowBoost. w: GlowLightLevel times the scene's light (the glow threshold) |
| TESR_BloomGlowExtra | x: AntiFlicker (1 or 0). y: LinearLight (1 or 0) |
| TESR_BloomExtraData (C++ only; declared by the tonemap shader) | x: 1 when the glow is on for the tonemap, else 0 |

Derived by the C++ per frame:
- Defaults: x = 0.005, y = 0.005 times (render width / render height), z = PassBlending, w = 1 / Passes.
- PassBlending above 0: before each upsample x and y are the texel size of the level being upsampled from (level i+1).
- Last upsample scale y: 1 / Passes when Spread is 0, else 1 / (sum over the passes of spread weight^level).

### Other inputs

- Settings only, no other textures. The depth buffer and the normals are not read.

## 4. Settings

All keys are in [Shaders.Bloom.*] sections. Ranges are the clamps the C++ applies. Unchanged by the rewrite.

### [Shaders.Bloom.Status]
- Enabled (bool, default true): replaces the game's own bloom (the tonemap uses this glow). Also feeds the dirt lens and the
  rain effects. Off: the glow buffer is black, the tonemap takes the game's bloom path.

### [Shaders.Bloom.Main], [Shaders.Bloom.Night], [Shaders.Bloom.Interiors]

Keys present in all three sections:
- Blending (float, default 0.04): read by the tonemap shader (not by the Bloom C++). The mix weight between the tonemapped
  picture and the glow, clamped to 0..1 when used.
- Strength (float, default 1.0): read by the tonemap shader. The gain applied to the glow before mixing.
- Passes (float, default 8; used as an integer, clamped 2..8): how many levels.
- PassBlending (float, default 0.0; clamped 0..1): 0 means the levels are added together. Above 0 each upsample is a blend
  between the level's own downsample and the upsampled level above, weighted by this value (section 5).
- Spread (float, default 0.0; clamped -0.75..2.0; UNOFFICIAL): 0 is as before. Above 0 wider levels weigh more and the glow
  reaches further; below 0 the glow is tighter. Only has an effect with PassBlending 0.
- GlowBoost (float, default 0.0; clamped 0..20; UNOFFICIAL): 0 is as before. Above 0, bright lights (neon, lamps, fire)
  count up to 1 + GlowBoost times as much as other pixels in the first downsample.

Keys present only in [Shaders.Bloom.Main] (read for all times of day):
- GlowLightLevel (float, default 1.5; UNOFFICIAL): what counts as a bright light for GlowBoost, as a multiple of a white
  surface in the scene's own light. Values at or below 0 become 1.5; otherwise clamped to 0.25..50.
- LinearLight (bool, default false; UNOFFICIAL): the chain blurs linear light instead of display-encoded values.
- AntiFlicker (bool, default false; UNOFFICIAL): the first downsample weighs each of its five boxes by 1 / (1 + luma of the box).

Settings that look similar but belong to other effects: [Shaders.Lens.*] BloomExponent (dirt lens) and
[Shaders.Precipitations.Main] Bloom (rain strength).

## 5. Passes and outputs

### The D3D9 chain (the reference path), Bloom.fx.hlsl

Let N = Passes (clamped 2..8). The chain is N downsample passes then N - 1 upsample passes, each a full-screen quad through the
frame vertex buffer. The C++ calls the techniques by index.

Downsample (techniques 0 to 7):
- Technique 0: reads the HDR scene (TESR_RenderedBuffer), writes level 0. The only pass that applies GlowBoost, LinearLight and
  AntiFlicker.
- Techniques 1 to 7: technique t reads level t-1 and writes level t.
- Each downsample reads 13 taps (the centre, four diagonal taps one texel away, and eight taps two texels away on the axes and
  diagonals; the texel is the source level's). They make five overlapping 2 x 2 boxes: the centre box weighs 1/2, the four
  corner boxes 1/8 each.

Upsample (techniques 8 to 14):
- Technique 14 - i upsamples into level i. Technique 14 is always the last one and writes level 0.
- Each upsample takes the 9-tap tent (centre 4, edges 2, corners 1, over 16) of level i+1 at radius (Data.x, Data.y).
- Add mode (PassBlending 0): level i = own_i + Spread * tent(level i+1). Each wider level is weighted by the spread weight, so the
  weight of level j in level 0 is spread^j.
- Blend mode (PassBlending above 0): level i = lerp(own_i, tent(level i+1), PassBlending). The result is a convex blend, so
  neither the spread weight nor the last-level scale is applied.
- The last upsample (technique 14, level 0): in add mode the result is multiplied by the scale y (TESR_BloomGlow.y). With
  LinearLight on, the result is encoded back to display values (the blend mode too).
- Output: level 0 (TESR_BloomBuffer, half size, A16B16G16R16F) holds the finished glow. It is the only level read outside the
  chain.
- Read and write: the upsample reads its own level (own_i) from the texture it writes (the D3D9 chain has no separate target
  for it). See section 9.

### The Vulkan chain (NVR DXVK)

Same passes and maths. Each downsample level below the top goes into its own target (NVR_BloomLevel0..6), so no pass reads and
writes one target. The upsamples write the chain's levels and read the downsample results from those targets.

### Hand-off to the game's tonemap (exact, unchanged)

1. Bloom writes level 0 (TESR_BloomBuffer) in the pre-tonemap chain.
2. The tonemap shader (ISHDRBLENDINSHADERCIN and AM variant) samples that texture at its slot s8 under the name TESR_BloomBuffer
   at the picture's own texture coordinate.
3. TESR_BloomExtraData.x selects the path. 1: the glow is read, linearised as the tonemap does for display values, multiplied by
   Strength (TESR_HDRBloomData.y) and mixed into the picture with Blending (TESR_HDRBloomData.x). 0: the tonemap uses the game's
   own bloom from its slot s0 and scales it by the same two constants with a different meaning.
4. The tonemapper runs on the blended picture, so the glow is added in HDR before tonemapping.

Other readers of level 0: the dirt lens (Lens.cpp), the rain/precipitation effect (Rain.cpp), and the debug view. They see a black
buffer when Bloom is disabled.

## 6. Behaviour details

- Filter kernels: the downsample is the 13-tap box filter of section 5. The upsample is the 9-tap tent.
- Linear light (GlowBoost or LinearLight on, first downsample): each tap is converted to linear light by a square (c * c, negatives
  clamped to 0), then the bright taps are boosted (below). Display values come back by a square root. The square pair is an exact
  inverse of itself. It replaces the old cubic fit (see the change note in section 9).
- GlowBoost brightness: the Rec.709 luma of the linear tap, mixed toward its peak channel by its saturation (1 minus the minimum over
  the peak). Threshold t = max(TESR_BloomGlow.w, 0.002). Amount = clamp((brightness - t) / t, 0, 1). The tap is multiplied by
  1 + GlowBoost times the amount. Unless LinearLight is on, the tap is then encoded back to display values before the boxes are
  averaged.
- AntiFlicker: the five boxes are weighted 1/8, 1/8, 1/8, 1/8 and 1/2 each divided by (1 + max(luma of the box, 0)), and the sum is
  normalised by the sum of the weights. First downsample only.
- Spread: wider levels are weighted by spread^j in level 0 (section 5). Spread 0 (weight 1) is the old behaviour.
- Sky, depth, first-person weapon: no masking. Bloom reads the whole HDR scene, sky included.
- Jitter (TAA, FSR, DLSS): not used.
- Reversed depth: not applicable.
- HDR vs LDR: all levels are linear HDR values, unclamped. Level 0 may be above 1.
- NaN and infinity guards: the first downsample's box weights are all positive, so the normalising sum cannot be zero. The glow
  threshold is floored at 0.002. Negative values are clamped before the square root and the square. The blend uses a convex mix.
- Passes below 8: levels deeper than N - 1 are not written and not read.
- Menu scene guard: TESR_BloomExtraData.x is set to 0 for menu frames so the frozen glow is not shown in the Pip-Boy or menus.
- RenderScale (rect frames, UNOFFICIAL): in the D3D9 chain each level draws into its top-left part; every tap (downsample and tent)
  is kept inside the drawn part (clampToRectIfOn), and after the upsample level 0 is stretched back to full size for its readers.
  Full-size frames run the same code with no clamp. The Vulkan chain has no rect steps (section 7).
- Vulkan refusal: if a Vulkan pass is refused, the effect falls back to the D3D9 chain for the session and that frame is redone.

## 7. Vulkan path

- Choice: [Main.Main.Performance] NvrDxvkPasses (default true) and the NVR DXVK extension (DxvkCompute::PassesOn). RenderBloomExt runs
  the chain. A pass that fails sets the effect's failure flag for the session. The D3D9 chain does the frame in that case.
- Files (src/hlsl/NewVegas/Compute):
  - Bloom.glsl: the shared include: the push block, the taps, the box and tent functions. Helper sampling is at point of use
    (GLSL forbids a sampler built outside the call).
  - BloomDownFirst.frag: technique 0 (scene into level 0, with GlowBoost, LinearLight and AntiFlicker).
  - BloomDown.frag: techniques 1 to 7.
  - BloomUp.frag: techniques 8 to 13 (level i+1 at binding 0, level i's own downsample at binding 1).
  - BloomUpLast.frag: technique 14 (writes level 0).
  - The .spv files beside each .frag are compiled with tools/nvr-dxvk/compile-compute.sh.
- Bindings (set 1): binding 0 is the first texture, binding 1 the second for the upsample passes. One LINEAR CLAMP sampler, its heap
  index in the push block at offset 64.
- Push block (80 bytes), the same layout in every pass: offset 0 TESR_BloomData, 16 TESR_BloomResolution, 32 TESR_BloomGlow, 48
  TESR_BloomGlowExtra, 64 sampler heap index (uint), 68..79 padding.
- Differences from the D3D9 chain: no RenderScale rect steps, and the separate level targets described in section 5.

## 8. Look and cost

- Correct result: a soft, wide glow around lamps, neon, fire, the sun and the bright sky, roughly even at all angles, that fades
  smoothly. At the default it is a faint bloom around bright sources.
- Typical wrong results:
  - A grey veil over the whole picture: Blending too high, or Spread too high.
  - Boxy or blocky glow edges: a wrong kernel or wrong level sizes.
  - Glints flickering on and off with the camera: AntiFlicker off with GlowBoost on.
  - A seam or missing area at the edges of a rect frame: RenderScale path (section 6).
  - Bloom not reaching the tonemap: the tonemap flag set to 0 by mistake.
- Cost class: multi-pass. The D3D9 chain is about 2N - 1 passes on a pyramid. Most of the cost is the first downsample at half
  resolution (13 reads of the full scene). The GlowBoost and LinearLight variant adds per-tap maths but no reads.

## 9. Open questions

Changes made in wave 3 (2026-11-02):
- FIXED: the RenderScale rect clamp is applied to every downsample and tent tap (clampToRectIfOn, as section 6 describes); before
  this the spec listed it but the rect-frame taps could reach outside the drawn part.
- FIXED: the glow threshold is floored at 0.002 before any division (as before), and negative values are clamped before the square
  root and the square (no NaN in the linear path).
- CHANGED (not a setting change): the linear-light conversion is a square / square root pair instead of the old cubic fit. The
  old fit was about 12 percent bright at 3. The new pair is exact as an inverse of itself, but it is not sRGB. The look of GlowBoost and LinearLight may shift a little: a tester
  check is needed.
- CHANGED (interpretation): PassBlending is the weight of the upsampled level in a blend with the level's own downsample (lerp
  own -> tent by PassBlending). The spec did not give the direction; the new reading keeps 0 as the add mode.
- CHANGED: in blend mode the last upsample applies no spread scale (the blend is already convex), as the spec's "or" implies.

Still open:
1. Read and write of the same texture (D3D9 only): the upsample reads its own level from the texture it writes. The C++ does not
   give a separate target for the D3D9 path (only the Vulkan path has one), so the shader could not avoid it without a C++ change.
   The old shader did the same; it is kept and reported. Whether it is well-defined on every D3D9 driver is not established.
2. Comment versus code in the tonemap shader (NVR-bloom branch): the glow is described as linear but the tonemap linearises it.
   Confirm the intent before a new tonemap is written.
3. RenderScale on the Vulkan chain has no rect steps. Whether a rect frame with the Vulkan chain shows a stale or stretched glow
   was not tested.
4. The tonemap path with the flag at 0 uses Blending and Strength with a different meaning. Whether that is intended for the game's
   own bloom is not stated in the code.
5. The blur radius of the upsample is in uv units at the default; whether it looks the same at every level was not measured.
6. Strength: the toml describes it for NVR and vanilla; the code reaches the tonemap as a gain on the NVR path and as a power-scaled gain
   on the game's path. Confirm the vanilla look against the toml wording.
