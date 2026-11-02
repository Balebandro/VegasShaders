# BloodLens effect spec

Status 2026-11-02 (branch claude/rw3-lens): `src/hlsl/NewVegas/Effects/BloodLens.fx.hlsl` and `Compute/BloodLens.frag` were written fresh from this spec as a minimal correct pass; the C++ still never starts a film (open question 1, not changed here). Settings, constants, samplers and push layout are unchanged. Choices the spec left open: the warp is 0.02 of the frame times the warp strength (z), so the warp reads the file's red and green at the pattern offset; the noise is the file's alpha at the warped coordinate plus the pattern offset, saturated; the blotch is (noise - threshold) x 4 clamped to 0..1, the film is blotch x amount (not clamped above 1, so Intensity above 1 still deepens the colour); the gloss is the screen-space slope of the film (ddx and ddy), clamped. Time is still in rendered frames in the C++ (open question 2); the zero-Time guard (open question 3) is in the C++ and is not touched.


## 1. Purpose

When the player is hit, a red, wet, blood-like film appears over the screen. The picture is pushed and warped by a blotchy noise pattern, the blotches take on the blood colour, and each blotch gets a glossy highlight. The film then fades out linearly over a fixed number of rendered frames. Nothing else in the picture changes: the pattern is static while it is on, and it fades in strength rather than moving.

## 2. When it runs

- Off by default. The effect's on/off switch is [Shaders.BloodLens.Status] Enabled, default false. The switch is read through the menu-shader mechanism at load and when settings change.
- Start: a random roll on each hit. The trigger (the game-event manager's hit handler) is only taken while the effect is idle. It draws a whole number from 1 to 99 (inclusive, uniform) and starts the effect if the roll is at or below [Shaders.BloodLens.Main] Chance. A hit during an active effect is ignored.
- Per frame: the effect is only evaluated when Enabled is true. It draws only while its fade level is above zero.
- Underwater: while the game reports the player as underwater, the fade level is forced to zero at once, so the film stops on the spot and draws nothing.
- Interior, exterior, night and menu rules: none. The effect does not check the cell type, the time of day or whether a menu is open. Only the hit trigger and the underwater state gate it.
- Frame order (src/core/ShaderManager.cpp): inside the post-process chain it sits in the lens group, after MotionBlur and before WaterLens, LowHF and DitherBuster. Earlier effects are Rain, Snow, BloomLegacy, HeatHaze, VolumetricSmoke, BarrelHaze, Coloring, LUT, DepthOfField, MotionBlur. It runs after tonemapping (the TAA note in ShaderManager says the post-tonemap image is what the later stages see). The effect is never the final effect in the chain unless everything after it is idle.
- Constants update once per frame, before the effect draws. The fade step is counted once per rendered frame, not once per constants update.

## 3. Inputs

Textures (sampler declaration order is the slot order; NVR binds by declaration):

| Slot | Name | Contents | Filtering / addressing |
|---|---|---|---|
| s0 | TESR_RenderedBuffer | The current frame image (the chain's input, after the earlier effects). | LINEAR min/mag/mip, CLAMP in U and V |
| s1 | TESR_BloodLensSampler | The file texture Effects\bloodlens.dds. Channels used: red and green = a two-channel warp offset field, alpha = a noise field used as the blotch pattern. Blue is not read. | LINEAR min/mag/mip, WRAP in U and V |

Constants (float4, bound by name; no fixed register is specified by the effect):

- TESR_BloodLensParams
  - x: pattern offset, a random value in [0.25, 0.75), drawn once when the effect starts. It is used as the warp sampling offset and as the noise sampling offset, so it is not animated over time.
  - y: blotch threshold, random in [-0.1, 0.5), drawn once at start. Noise values below it give no blood.
  - z: warp strength, random in [-2, 2), drawn once at start. The sign can flip the direction of the warp.
  - w: effective amount = Intensity times the fade level. It runs from Intensity down to zero over the fade. It is recomputed every constants update.
- TESR_BloodLensColor
  - x, y, z: the blood colour (red, green, blue) from the settings ColorR, ColorG, ColorB.
  - w: not written and not read.

Internal state, not sent to the shader: the fade length (taken from the Time setting, in rendered frames), the fade counter and the fade level (1 at start, falling linearly to 0).

## 4. Settings

All keys are in [Shaders.BloodLens.Main] unless stated. Type float for all of them. Ranges are not clamped in code; the ranges below are the sensible ones.

| Key | Default | Range | Effect on the picture |
|---|---|---|---|
| Chance | 10.0 | 0 to 100 (percent) | Share of hits that start the film. 0 = never, 100 = every hit. |
| Intensity | 0.8 | 0 up | Peak strength of the blood. It scales the amount of colour replacement and the highlight, so 0 hides the effect and high values make a dense red film. |
| Time | 900.0 | above 0 | Length of the fade, in rendered frames (not seconds). 900 is about 15 seconds at 60 frames per second and longer at lower frame rates. Zero or less is not guarded (see section 9). |
| ColorR | 0.92 | 0 to 1 | Blood colour, red channel. |
| ColorG | 0.16 | 0 to 1 | Blood colour, green channel. |
| ColorB | 0.16 | 0 to 1 | Blood colour, blue channel. |

Also in the defaults file: [Shaders.BloodLens.Status] Enabled = false (the on/off switch, see section 2). The settings are re-read when the settings change (UpdateSettings), so Intensity and the colours apply live.

Visual meaning: Intensity scales how far the picture is pushed toward the blood colour and how bright the highlight is. Chance does not change the look, only how often it appears. Time sets how long the look lasts.

## 5. Passes and outputs

- D3D9 path: one technique, unnamed (technique index 0), with one unnamed pass. The C++ asks for technique 0 only. Vertex shader: pass-through of the frame quad position and texture coordinate. Pixel shader: the blood pixel shader in the .fx.hlsl. Shader model 3.0 for both.
- Output: one colour target, the same size as the frame being rendered (the full frame, or the rect part in RenderRect frames, see section 6). It writes the whole target and sets alpha to 1. The target is the frame chain's spare buffer, or the game's final target when this is the last effect in the chain.
- Vulkan path (section 7) draws the same single pass to the same target.
- The effect does not clear the target before drawing (ClearRenderTarget is false), because it writes every pixel.
- It does not need the source buffer sampler (the .fx.hlsl declares none). The chain may still copy the frame before the pass; that copy has no look effect.

## 6. Behaviour details

- Sky and depth: no depth or normal input. The sky and the world are treated the same way; the blood film covers everything on screen, including the sky and the HUD, because it is a screen-space pass with no masking.
- First-person weapon and body camera: no special case. The film covers the whole picture, including the weapon.
- RenderRect and RenderScale: in rect frames the D3D9 pass draws into the top-left rect of the target (viewport set from the rect). The Vulkan path is turned off in rect frames by its own gate, so the D3D9 pass is used there.
- Jitter (TAA): none. The effect does not read or undo any sub-pixel jitter. The warp and the noise both sample at the pixel's UV plus the offsets, so a jittered frame is not compensated.
- Reversed depth: not applicable (no depth read).
- HDR vs LDR: the effect works on whatever values the chain holds at this point (post tonemap, so normally LDR). Output is not clamped, but the blood term is bounded: the amount is between 0 and Intensity, so the mix stays inside the input range plus the blood colour and a small highlight term. Values above 1 can appear only if the input already has them, or if the highlight term pushes past 1 at high Intensity.
- NaN and inf guards: none in the shader. The noise is saturated before use, so the amount is always between 0 and Intensity. The normal uses screen derivatives of the amount, so the highlight is zero where the amount is flat. Derivatives are taken per 2x2 pixel quad, so the highlight edges can look blocky at a flat area boundary.
- Warp and wrap: the warp can push a sample outside 0 to 1 (strength up to 2 times a value up to 1). The file sampler is WRAP, so these wrap around the texture rather than stretching the edge. This is intended; a clamped sampler would smear.
- Fade: the level is 1 when the film starts, falls by one step per rendered frame, and stops at zero. The film is not on at all once the level reaches zero, so the shader is skipped.
- Re-trigger: a new hit during a film does nothing until the film has faded out. A hit when the film is already at zero starts a new film with new random values.
- Known wrong looks if missed:
  - The film stays after the fade (the fade level not reaching zero, or not being applied).
  - The pattern moves or shimmers while it is on (the offsets must stay fixed for the whole film; only the amount fades).
  - The screen goes red with no blotch structure (the threshold or the warp is missing).
  - The film draws underwater (the underwater stop is missing).
  - The film fades at a rate that changes with the constants-update count rather than the rendered-frame count.
  - The blue channel of the file is used for the pattern instead of alpha.

## 7. Vulkan path

- Vulkan source: src/hlsl/NewVegas/Compute/BloodLens.frag (GLSL 450, a Vulkan version of the D3D9 blood shader, written line for line). Compiled binary: src/hlsl/NewVegas/Compute/BloodLens.spv, loaded at runtime from the deployed Shaders\VegasShaders\Compute\ folder under the name BloodLens.
- Selection: the Vulkan path is used only when the effect's extension path is on, that is, [Main.Main.Performance] NvrDxvkPasses is on and the NVR DXVK extension is present. It is not used in RenderRect frames, when the effect is listed in NvrDxvkKeepD3D9, or when an SM6 build covers the pass (none exists for BloodLens; checked by name). When the Vulkan path cannot run (missing file texture, or a failed pipeline), the D3D9 pass is used for the frame, and after a pipeline failure for the rest of the session.
- Technique: only technique index 0 is supported by the Vulkan path; any other index is refused and falls back.
- Descriptor layout (set 1):
  - binding 0: sampled 2D image, the rendered frame (slot 0, the frame manager's rendered texture). Matches TESR_RenderedBuffer.
  - binding 1: sampled 2D image, the blood file texture (slot 1). Matches TESR_BloodLensSampler.
- Samplers (both are sampler slots with their index passed in the push block):
  - slot 0: linear filter, clamp. Its index is at push offset 32. Used for the rendered frame.
  - slot 1: linear filter, wrap. Its index is at push offset 36. Used for the blood file texture.
- Push block (48 bytes, one push block for the whole pass):
  - offset 0: params (vec4), the same content as TESR_BloodLensParams.
  - offset 16: bloodColor (vec4), the same content as TESR_BloodLensColor (w unused).
  - offset 32: samplerIndex (uint), the heap index of the clamp sampler.
  - offset 36: wrapIndex (uint), the heap index of the wrap sampler.
  - offsets 40 and 44: two uint pads, unused.
- Pipeline: one graphics pipeline, made from the BloodLens shader with the two bindings above and a 48-byte push block. It draws one full-screen pass, last in its effect, so it can write the game target directly when it is the final effect.
- Matching rules for a new Vulkan version: keep the descriptor binding numbers, the sampler push offsets (32 and 36), the 48-byte push size and the components above. A new version must read the warp from the red and green channels and the noise from alpha of the file texture.

## 8. Look and cost

- Correct look: during the film, the picture is tinted toward the blood colour in blotches. Blotches are shaped by the file's noise, which is warped by the file's warp field. Each blotch has a glossy edge highlight. Strength is highest at the start and falls to nothing over the fade length. The pattern holds still during the film.
- Typical wrong results: a flat full-screen red tint (no pattern); pattern shimmering or moving frame to frame; the film lasting past its fade; the film on in water; the highlight too strong (white or washed-out areas); blocky highlight edges on flat areas (acceptable only where the amount has a hard edge).
- Cost class: cheap. One full-screen pass with three texture reads (the frame, the warp offset, the noise) plus screen derivatives. It only runs for the length of a film, so there is no cost between hits.

## 9. Open questions

1. Trigger not wired (most important): The game-event manager's hit handler has no call site anywhere in the tree (only its definition; the manager itself is created from src/NewVegas/Managers.cpp). Nothing else in the tree sets the fade level to 1 for BloodLens. So as this tree stands, the film is never started and the effect never draws. Confirm where the hit event is meant to come from (a hook in another branch, or a removed caller) before relying on the look.
2. Time units: the setting is named Time, but the code counts rendered frames (the C++ comment says so). Confirm that the spec's wording (frames) is what the design intends.
3. Time at zero or less: the fade divides by the Time value with no guard, so Time = 0 gives an infinite or undefined fade, and the fade level is then clamped. The start-of-film frame may be affected. Decide whether the new version should clamp Time to at least one frame.
4. Channel of the file texture: the spec assumes red and green hold the warp field and alpha holds the noise. Blue is not read. The file's actual contents were not inspected (it is a binary asset).
5. Vulkan mip filtering: the D3D9 file sampler uses a linear mip filter; the extension's wrap sampler mip setting was not checked. Confirm the file has no mip chain or that the wrap sampler matches.
6. Vulkan UV mapping: the Vulkan pass uses the default full-screen mapping of the extension; whether it matches the D3D9 frame quad's half-pixel placement was not verified (the effect's D3D9 pass uses a pass-through vertex shader).
7. Enabled key: the on/off switch is read as [Shaders.BloodLens.Status] Enabled through the menu-shader mechanism; the mapping was inferred from the defaults file and the shared switch code, not traced line by line.
8. Random seeding: the start values use the C library random generator seeded from wall-clock time at each start. Whether the game also seeds it was not checked; the effect does not depend on it.
9. Dependency on other blood systems: BloodLens does not read BloodFlow, HandBlood or the Blood status section ([Shaders.Blood.Status] is marked not used in the defaults). It reads only its own settings, its own file texture and the rendered frame. The blood-flow effect (src/effects/BloodFlow.cpp) is a separate effect and is not specified here.
10. RenderRect frame texture size: the exact size of the rect in rect frames and how the D3D9 viewport maps the pattern into it was not traced beyond the viewport set.
