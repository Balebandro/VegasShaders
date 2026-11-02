# MotionBlur - effect spec

Rewritten 2026-11-02 (wave 3, branch claude/rw3-screen, commit 80cbf4a7). New shaders: src/hlsl/NewVegas/Effects/MotionBlur.fx.hlsl (D3D9)
and src/hlsl/NewVegas/Compute/MotionBlur.frag (Vulkan, drawn twice). The C++ is unchanged.
FIXED 2026-11-02: a GaussianWeight of zero (or below) is held at 0.001 (section 6, no divide by zero); the tap weights are shifted by their
largest exponent so they cannot overflow for small weights (same normalised result, the look is unchanged); the D3D9 taps are clamped to
the drawn part of a render rectangle (section 6, RenderScale). Kept: the full-target texel scaling (section 9, open).

## 1. Purpose
When the player turns the view quickly, the whole picture smears along the direction of the turn, the way a camera with an open shutter smears a pan. A still camera, or a slow turn, leaves the picture sharp. The smear is one-directional: the image is averaged with copies of itself shifted along the turn direction on screen, so bright edges trail across the frame. It works on the final LDR picture after tonemapping, so it smears everything in that picture, including the sky and the first-person weapon.

## 2. When it runs
- Gated by the effect's menu switch, which reads the Status section (`[_Shaders.MotionBlur.Status] Enabled`, default off). Off means the effect does not draw. (The mapping from menu switch to this key was read from the settings code, not traced end to end.)
- Per frame, the effect draws only if its smoothed turn data is non-zero (horizontal or vertical component not exactly 0). A still camera draws nothing and the picture passes through unchanged.
- No interior, exterior, night or menu rule is applied by this effect.
- Frame order (src/core/ShaderManager.cpp, post-tonemap block): TAA, rain, snow, legacy bloom, heat haze, volumetric smoke, barrel haze, coloring, LUT, depth of field, then MotionBlur, then blood lens, water lens, low-high-frequency, dither buster, SMAA, FXAA, sharpening, cinema, image adjust, body camera, sky dither, debug. MotionBlur therefore reads the depth-of-field result and feeds the lens effects.
- Render call: the chain image is the rendered surface; the post-tonemap copy is passed as the source surface, which this effect does not read.

## 3. Inputs
Textures:
- Slot 0, named TESR_RenderedBuffer (D3D9 sampler register s0; Vulkan set 1, binding 0). Holds the chain's current image. Linear filtering, clamp addressing on both axes. This is the only texture the effect binds.

Constants (names as the C++ registers them; the Vulkan push block carries the same values):
- TESR_ReciprocalResolution (float4, set by the shader manager, not by this effect): x = 1 / render target width, y = 1 / render target height, z = aspect ratio, w = 0 (reserved). Only x and y are used. Note these are the full render target's size (see section 6).
- TESR_MotionBlurParams (float4, written by this effect each frame from settings):
  - x = GaussianWeight (weight spread), unitless, must stay above zero.
  - y = BlurScale (multiplier on the offset length), unitless.
  - z = BlurOffsetMax (clamp, texels, applied to each offset component).
  - w = unused by the shader.
- TESR_MotionBlurData (float4, written by this effect each frame):
  - x = horizontal blur vector (from yaw change), in degrees of turn, smoothed over three frames.
  - y = vertical blur vector (from pitch change), same units and smoothing.
  - z, w = unused.
  The shader treats x and y as a 2D direction; the number is scaled by one texel and BlurScale in the sampling.

Shared use: the average-luminance compute pass (src/effects/AvgLuma.cpp) reads this same TESR_MotionBlurData value, so the constant update must run before that pass in the frame.

## 4. Settings
Read from resource/VegasShaders.defaults.toml. The effect reads the first-person block when the camera is first-person, otherwise the third-person block.

Section `[_Shaders.MotionBlur.Status]`:
- Enabled (bool, default false). The menu switch (see section 2). Comment: blur in the direction of the camera when turning.

Section `[_Shaders.MotionBlur.FirstPersonView]`:
- BlurCutOff (float, default 15.0). Threshold on the squared turn size (horizontal squared plus vertical squared, degrees). Below it both components are zeroed for the frame, so small jitter does not smear. Higher = more turn is needed.
- GaussianWeight (float, default 6.0). Weight spread. Higher flattens the weights.
- BlurScale (float, default 4.0). Length multiplier. Higher = longer streaks. The most visible control.
- BlurOffsetMax (float, default 24.0). Cap on the offset per component, in texels. Limits the streak length on violent turns.
- Enabled (bool, default true). Not read by this effect's code.

Section `[_Shaders.MotionBlur.ThirdPersonView]`:
- The same four keys with the same meanings, defaults 15.0, 6.0, 4.0, 24.0.
- Enabled (bool, default false). Not read by this effect's code.

## 5. Passes and outputs
Technique 0 is the only technique. It has two passes running the same pixel shader with the same constants. Any other technique index draws nothing.

- Pass 1: reads the chain image, writes the blurred result to a chain surface (D3D9: the current render target).
- Pass 2: reads the result of pass 1, blurs it again with the same constants, and writes the final result. Two passes give two averaging steps in total. Only pass 2 may write straight into the game's target, and only when this effect is the chain's last effect for the frame.
- Output: same size and format as the chain image (full render target, LDR). Output alpha is 1.

## 6. Behaviour details
Turn measurement (once per frame; the update is guarded so a second call in the same frame does nothing):
- Yaw is the player's rotation about the vertical axis, pitch the rotation about the horizontal axis, both converted from radians to degrees.
- Horizontal change = this frame's yaw minus last frame's yaw, taken the shorter way round the circle (a turn across north must not produce a 360-degree spike). Vertical change = pitch minus last pitch.
- If horizontal squared plus vertical squared is below BlurCutOff, both changes are zeroed for this frame.
- Blur vector = the mean of this frame's change and the two previous frames' changes (a three-frame history, shifted once per frame). The mean stops one jumpy frame from flashing.
- Pitch is not scaled by aspect ratio here. (The body camera effect does scale its pitch term by aspect; this effect does not.)
- BlurParams are rewritten each frame from the active settings block.
- The history starts at zero on construction, so the first turn does not spike.

Sampling in the shader:
- Each offset component is clamped to plus and minus BlurOffsetMax before use.
- 12 step indices (i = 0 to 11, 24 taps in total). At step i the shader samples the pixel's coordinate plus the offset vector times one texel times BlurScale times i/24, and also the negated offset. Both samples of a step share one weight.
- Weight for step i: exp( i squared / (5 times GaussianWeight squared) ), divided by GaussianWeight. The weight rises with i, so far taps count more than near ones. This is the defined shape of the effect; changing it changes the look and needs approval.
- Result: the weighted sum of RGB divided by the total weight (brightness is preserved; the blur averages, it does not add light). Alpha is 1.
- No linearisation: samples are used as the chain holds them. No NaN or infinity guard. FIXED 2026-11-02: a GaussianWeight of zero or below is held at 0.001 (no divide by zero), and the weights are computed relative to the largest one.
- The effect does not read depth, normals, sky or weapon masks. Sky and weapon are smeared with the rest.
- No TAA jitter is applied; the coordinate is un-jittered and there is no reprojection.
- Reversed depth: not used. HDR: not applicable, the chain image is LDR at this point.

RenderScale and RenderRect:
- The offset is scaled by the full render target's reciprocal size. If the game draws into a sub-rectangle (RenderRect / TESR_RenderScale), the smear length is measured in full-target texels, not rectangle texels. FIXED 2026-11-02: the taps are clamped to the drawn part of the rectangle (half a texel in from its edges), so they do not read the undrawn area. The scaling is unchanged (full-target texels); whether it should follow the rectangle is still an owner decision (section 9).

Edge cases:
- A camera cut (teleport, cell change) gives one large turn. This effect does not read the cut flag (GameState.isCameraCut); it relies on the BlurOffsetMax clamp and the three-frame mean.
- Turn data is updated from player rotation only; menus do not change it.

Things that look wrong if missed:
- Updating twice per frame: the smear is about a third of the intended strength and loses its smoothing.
- No shorter-way yaw difference: a turn through north gives a full-screen streak for one frame.
- No BlurOffsetMax clamp: a fast turn smears across the screen.
- Weight shape reversed: a softer, different look.
- Addressing other than clamp: dark or bright edge halos.

## 7. Vulkan path
Files:
- src/hlsl/NewVegas/Compute/MotionBlur.frag (GLSL 450 fragment pass; the same algorithm as section 6, including the 24 taps, weights, clamps and normalisation).
- src/hlsl/NewVegas/Compute/MotionBlur.spv (compiled form of the above; regenerate, do not hand-edit).
- ObjectMotion.glsl is not included by this shader.

Route choice (src/effects/MotionBlur.cpp ExtRender, src/core/EffectRecord.cpp Render):
- The Vulkan route is used when the NVR DXVK extension is present, the NvrDxvkPasses key is on (`[Main]` group in the defaults toml, default true), the effect is not on the keep-on-D3D9 list, and the technique has no SM6 build selected (Sm6Covers). Otherwise the D3D9 technique runs.
- Native D3D9 and stock DXVK never take this route.

Failure behaviour:
- If pass 1 fails, the effect returns false and the whole technique runs on D3D9 for this effect; the effect is then marked failed for the session (logged).
- If pass 2 fails after pass 1 has been committed, the effect returns success with the pass-1 result left in place, so no second D3D9 blur runs.

Binding and push-constant layout (the C++ fills it, the shader reads it), 64 bytes total:
- Texture: t_rendered at set 1, binding 0, sampled with a sampler chosen by the index in the push block (linear, clamp).
- Offset 0: reciprocalResolution (vec4), same content as TESR_ReciprocalResolution.
- Offset 16: blurParams (vec4), same content as TESR_MotionBlurParams.
- Offset 32: blurData (vec4), same content as TESR_MotionBlurData.
- Offset 48: samplerIndex (uint32), index into the shared sampler table for linear filter with clamp addressing.
- Offsets 52 to 63: three uint32 pad words, zero.

## 8. Look and cost
Correct: a brisk turn streaks the picture along the turn with soft, trailing bright edges; the streak fades within a few frames of the turn stopping. Slow turns show little or no smear. Overall brightness is unchanged.

Typical wrong results:
- Smear lingering after the turn stops (history not shifting, or the once-per-frame guard broken).
- Smear while standing still (BlurCutOff too low, or the zero-turn skip missing).
- Full-screen streak for a frame after a fast turn or a teleport (no yaw wrap, or clamp removed).
- Edge halos (wrong addressing).
- Doubled strength (pass 2 repeated, or a D3D9 fallback after a committed pass 1).

Cost class: multi-pass per-pixel loop. Two passes, each 24 texture taps and 24 weight evaluations per pixel, at full target size. Skipped entirely when the turn is zero, so it costs only while the view turns.

## 9. Open questions
- The first-person and third-person `Enabled` keys are not read by this effect's code. Whether the menu switch maps only to the Status key was read from the settings code but not traced in full.
- Which first-person test selects the settings block in unusual camera states (cutscenes, bodycam) was not checked.
- Whether BlurScale and BlurOffsetMax should be measured in output pixels when RenderScale is below one was not decided; the code uses full-target texels.
- Whether the distance-rising weight shape is intended is not stated in the source; it is kept as the defined look.
- Whether the second pass writes the game target depends on the chain's last-effect order; only the order list in ShaderManager.cpp was read, not the chain's full rule.
- D3D9 register numbers are assigned by the shader manager at registration and are not fixed in this effect's source; only names and order are given here.
