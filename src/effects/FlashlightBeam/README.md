# FlashlightBeam - effect spec

REWRITTEN 2026-11-02 (branch claude/rw3-lights): the shader is new, written from this spec (Effects/FlashlightBeam.fx.hlsl; Compute/FlashlightBeam.frag). The C++ and the settings are unchanged. Wave 3 readings are listed in section 9 (items 8 to 13).


## 1. Purpose

While the flashlight (the Pip-Boy light replacement) is on, the air in its cone shows a soft, faint shaft of scattered light: dust in the beam glows toward the viewer and is brighter when looking along the beam. The shaft is computed in its own half-resolution buffer and is then added into the picture by the Flashlight effect's composite step, so it is tonemapped and bloomed with everything else. It is an in-air effect only: it does not light surfaces (the Flashlight effect does that), and it does not change anything when the flashlight is off.

## 2. When it runs

Enabled conditions (all must hold for the beam to draw):
- `[_Shaders.FlashlightBeam.Status] Enabled` is true (default true). The Status key is read through the effect's enable flag; the effect's own comment says it requires the Flashlight shader.
- The Flashlight effect is enabled (`[_Shaders.Flashlight.Status] Enabled`, default true) AND its spot light is active (the flashlight is currently on, which the Flashlight effect reports).
- The view toggle for the current camera mode is on: in third person (player's `isThirdPerson`) the `ThirdPerson` key applies; otherwise the `FirstPerson` key applies. If the player object is absent, the `FirstPerson` key applies.
- The gate value computed on the CPU is above zero. This is the only test the frame uses: the frame draws the beam only when the gate is above zero, and the Flashlight composite reads the beam only when its own copy of the gate is above zero.

The gate is folded into one number (the strength setting, or zero). The settings-change path clears the gate to zero on every settings change, whether or not the effect is enabled, so disabling the effect (or turning off the flashlight) cannot leave a stale shaft in the buffer.

Interior, exterior, night and menu rules: the effect has none of its own. It has no ShouldRender restriction. Whatever the flashlight does in those places, the beam follows the gate above. Whether the frame draws it while a menu is open is not established by this effect's code (see section 9).

Frame order (src/core/ShaderManager.cpp, the post-process chain of the world frame):
1. The effects that run before the flashlight in the chain (the WetWorld and BloodFlow steps, with the render target still the frame chain's current image).
2. FlashlightBeam: if the gate is above zero, the beam is rendered into its own buffer (clearing it first), and then the render target is set back to the frame's current target.
3. Flashlight: its own pass (the composite). It reads the beam buffer when the same gate is above zero.
4. Specular, Underwater, Reflections and later effects follow.

The beam therefore reads the scene depth as it is at that point in the chain (after the earlier effects), and the Flashlight composite is the only consumer.

## 3. Inputs

### 3a. Textures and samplers (D3D9 technique, slot order = declaration order)

| Slot | Name | Holds | Sampler state |
|---|---|---|---|
| s0 | TESR_DepthBuffer | Scene depth, linear, stored as view distance divided by the far distance (R32F). Read at level 0 only. | POINT magnification and minification, no mips, CLAMP in U and V |
| s1 | TESR_SpotLightTexture | The flashlight cookie, file `Effects\Flashlight.png` (shared with the Flashlight effect). Only the red channel is used. | LINEAR magnification and minification with LINEAR mips, CLAMP in U and V |

Only these two textures are bound. The output target is not an input.

Vulkan version (section 7): the same two inputs, depth at set 1 binding 0 and cookie at set 1 binding 1, samplers taken from the heap index values in push data (section 7).

### 3b. Constants

Registers: the D3D9 constants are not given fixed registers in the shader source; the compiler assigns them. The C++ side binds each one by name (the names below are the ones it registers or looks up). Units and components:

| Name | Who fills it | Components |
|---|---|---|
| TESR_VolumetricControl | this effect (CPU gate and settings, section 2) | x: strength (0 = off; also the composite gate). y: anisotropy g of the phase function. z: height falloff factor. w: maximum march distance, world units. |
| TESR_VolumetricData | the Flashlight effect (per frame) | xyz: world position the march uses for the light (the true light position in third person; in first person, the true light position moved by the first-person offsets, section 6). w: first-person strength floor (0 in third person, otherwise the FirstPersonStrength setting). |
| TESR_SpotLightPosition | Flashlight | xyz: true light position, world units. w: light range (radius), world units. |
| TESR_SpotLightColor | Flashlight | rgb: light colour (linear). w: dimmer. |
| TESR_SpotLightDirection | Flashlight | xyz: unit direction the light points (the first matrix column of the light's world rotation). w: outer cone angle, in degrees. |
| TESR_SunColor | the game's sun colour state | rgb used; its luminance sets the energy normalisation (section 6). |
| TESR_ReciprocalResolution | the frame | xy: 1 over the frame's render size in pixels, as the frame sets it. The noise pattern divides the screen coordinate by this to get a pixel position. |
| TESR_CameraPosition | the frame | xyz: camera world position. |
| TESR_CameraData | the frame | x: near distance; y: far distance (used to turn depth into view distance). |
| TESR_FlashLightViewProjTransform | Flashlight | 4x4 matrix: world to the flashlight's clip space (projects a march point into the cookie's texture coordinates). |
| TESR_ViewTransform | the frame | 4x4 matrix: the camera view transform, used to rebuild a view ray per pixel. |
| TESR_ProjectionTransform | the frame | 4x4 matrix: the camera projection, used to rebuild a view ray per pixel. |

Matrices are passed as the D3DX matrices in memory (row layout as stored); the Vulkan block keeps the same memory layout (section 7).

## 4. Settings

All keys are read in the C++ (src/effects/FlashlightBeam.cpp) from section `Shaders.FlashlightBeam.Main`. The defaults file header is written with a leading underscore (`[_Shaders.FlashlightBeam.Main]`), the way other sections are in that file; the code uses the name without it.

| Key | Type | Range | Default | What it changes |
|---|---|---|---|---|
| FirstPerson | bool | on/off (range not stated in the defaults file) | true | Shows the beam in first person. Off: no beam in first person. |
| ThirdPerson | bool | on/off | true | Shows the beam in third person. Off: no beam in third person. |
| Strength | float | not stated (0 turns it off) | 1.0 | Overall amount of light the air scatters into the beam. Scales the whole shaft linearly. 0 = no beam, no buffer work. |
| Anisotropy | float (phase g) | not stated; a forward-scatter value between 0 and 1 is the intended range | 0.45 | Forward scattering. Higher makes the shaft brighter when looking along the beam towards the light, dimmer when looking across it. |
| HeightFalloff | float | not stated | 1.0 | How quickly the airborne dust thins with height above the player's feet (see section 6). Higher: the shaft is brighter near the ground and fades sooner upward. |
| MaxDistance | float (world units) | not stated | 1500.0 | Distance past which the march stops. Also the depth reach for the sky (see section 6). Lower: a shorter shaft. |
| FirstPersonStrength | float | not stated | 0.15 | First-person strength floor. In first person the light sits on the camera, so this value is the minimum strength the shaft keeps and it also flattens the cookie and the fade (section 6). |
| FirstPersonOffsetRight | float (world units) | not stated | 8.0 | Sideways offset, in camera space to the right, of the virtual light the first-person march uses (creates the parallax the shaft needs). Set by the Flashlight effect's code, read here through this effect's settings. |
| FirstPersonOffsetDown | float (world units) | not stated | 4.0 | Downward offset, in camera space, of the same virtual light. Same path as FirstPersonOffsetRight. |

Related keys in other sections (not owned by this effect, but they change what the beam looks like):
- `[_Shaders.Flashlight.Status] Enabled` and `[_Shaders.Flashlight.Main]` (Angle, Distance, colour, Dimmer, etc.) drive the cone and the light this beam uses. The cone angle comes from the Flashlight's Angle, through TESR_SpotLightDirection.w.
- `[_Shaders.Flashlight.Main] EdgeFix` value 2 also reads the beam buffer at edges of the flashlight's surface pool (the Flashlight effect's own choice; the beam shader does nothing differently for it).
- `[Main.Main.Performance] NvrDxvkPasses` selects the Vulkan route (section 7).

## 5. Passes and outputs

Technique and pass: the D3D9 effect has one unnamed technique (technique index 0) with one pass: a vertex shader of profile vs_3_0 that passes the full-screen vertex data through, and a pixel shader of profile ps_3_0 (the march). The C++ requests technique index 0 only. Any other index draws nothing.

The beam is rendered into one render target:
- Name: TESR_VolumetricBuffer (texture), surface VolumetricSurface.
- Size: half the render width and half the render height. The C++ comment says "full with Max Quality" (see section 9 for the max quality path).
- Format: A16B16G16R16F (four channels, 16-bit float each), linear HDR values, not clamped.
- Clear: the C++ clears the target before the pass. The pass also writes every pixel.

Output per pixel:
- rgb: the in-scattered light for this view ray, linear HDR (values above 1 are allowed). It is already scaled by the strength and the camera fade (section 6).
- a: the transmittance along the marched part of the ray (1 = clear air, lower = the march's air absorbed some light). The Flashlight composite reads rgb only; alpha is informational.
- Where the view ray misses the cone, or the strength is zero: (0, 0, 0, 1) exactly.

The buffer persists between frames but is rewritten whenever the frame draws the beam. It must never be read by the composite when the gate is zero (the composite checks its own copy of the gate).

## 6. Behaviour details

Ray and depth:
- The view ray for a pixel is rebuilt from the camera view and projection matrices (not normalised by distance: the ray length is kept, and the scene distance is the ray length times the depth value times the far distance).
- Scene depth bounds the march: the march ends at the scene distance (the surface the pixel sees), and never beyond MaxDistance. Sky pixels: the depth buffer holds the far value there, so the march is limited only by MaxDistance. There is no separate sky branch.
- Near the surface the shaft fades to zero over 256 world units (smooth ramp from the surface toward the camera side). This avoids a hard edge where the air meets a wall or the ground.

Cone geometry:
- The cone axis is TESR_SpotLightDirection.xyz; its outer half-angle is TESR_SpotLightDirection.w degrees. The inner angle used for the falloff is half the outer angle.
- Inside the cone, the spot falloff is the cosine interval between outer and inner angle (smoothstep-like linear in cosine), squared.
- Range: the light's range is TESR_SpotLightPosition.w. Beyond it the light contributes nothing.
- Distance attenuation: let s be (distance to light / range) squared, clamped to 0..1. The attenuation is (1 - s) squared, divided by (1 + 5 s), clamped to 0..1.
- The segment of the view ray that lies inside the cone is found analytically first (the ray entering and leaving the cone and the range sphere, plus the case where the camera or the sample is inside). Only that segment is marched. A ray that misses the cone returns (0, 0, 0, 1) without any further work. The brackets may be a little loose; every march sample still evaluates the exact cone and attenuation, so looseness costs steps but not correctness.

March:
- A fixed 24 steps across the in-cone segment, evenly spaced.
- The first sample is offset by a per-pixel fraction of one step taken from a fixed interleaved-gradient pattern computed from the screen pixel position. This hides the banding of a fixed step count. The pattern is static from frame to frame (it is not temporal and it does not follow the TAA jitter). Expect a fine static dither pattern at low step counts; it must not shimmer.
- Each sample: the cone factor and attenuation at that point, the cookie value at that point (projected through TESR_FlashLightViewProjTransform into the cookie's texture coordinates, sampled with a bilinear/trilinear CLAMP filter, red channel), the height density, the phase function, and the surface fade.
- Phase function: Henyey-Greenstein with g = Anisotropy. The angle is between the view ray direction and the direction from the sample toward the light (negated). The denominator is kept above a tiny positive floor. (Wave 3: p = (1 - g^2) / (4 pi (1 + g^2 - 2 g cos)^1.5) with cos = dot(view ray, direction from the sample to the light); cos = 1 when the viewer looks straight at the light, which is the forward peak.)
- Height density: at a sample, height above ground = sample world z minus (camera z minus 128 units). Density = exp(-(max(0, height above ground)) * HeightFalloff * 0.0005). The 128 units and 0.0005 are fixed values of this effect.
- Transmittance: each step multiplies the running transmittance by exp(-density * 0.5 * step length * 0.001). The scatter adds transmittance times sigma times the phase times step length, where sigma is density times cone times attenuation times cookie times surface fade.
- The loop is always 24 iterations (no early exit). With a miss the step length is zero and the loop adds nothing.

Output scaling and light colour:
- Light colour = TESR_SpotLightColor.rgb times TESR_SpotLightColor.w (dimmer) times (1 divided by the luminance of TESR_SunColor, where the luminance is clamped below at 0.05). This normalises the shaft against the sun, so the shaft keeps a steady ratio to the surface pool as exposure moves through day and night.
- Calibration: a fixed factor 0.0008 converts the scatter integral to the surface pool's range, so Strength near 1 reads as a multiplier around 1.
- Final in-scatter = scatter times (Strength times 0.0008 times camera fade) times light colour.

Camera fade and first person:
- Camera fade = clamp((distance from light to camera minus 40) / 60, 0 to 1). The shaft is zero when the light is within 40 units of the camera and full at 100 units or more. This stops the in-scatter piling onto the surface pool when the light sits on the eye.
- First person: the Flashlight effect's code (not this shader) moves the march's light position (TESR_VolumetricData.xyz) by FirstPersonOffsetRight along the camera's right axis and FirstPersonOffsetDown along the camera's down axis, so the march has parallax. It sets TESR_VolumetricData.w to FirstPersonStrength. The surface pool and the cookie keep the true light position.
- In the shader, the first-person blend = clamp(TESR_VolumetricData.w, 0, 1). It (a) replaces the cookie value with 1 (flat) by linear blend, and (b) floors the camera fade at the same value. In third person w is 0, so neither applies.

Edge cases that look wrong if missed:
- Negative or zero Strength: the output must be exactly (0, 0, 0, 1). Nothing is marched.
- Missing or stale gate: the composite must not add the buffer when the beam is off. The gate is zero whenever the effect is off, the flashlight is off, or the view toggle is off.
- Camera inside the cone: the segment starts at the camera (handled by the segment logic). Without it, the shaft is missing right next to the flashlight holder.
- Sky: the shaft must still reach MaxDistance into the sky; a pixel on sky must not get a zero shaft.
- Normalisation by sun luminance: a shaft that does not follow the sun's luminance will look bright at night and wash out in daylight.
- Direction of the phase: the value must be large when looking along the beam toward the light and small when looking across it, with Anisotropy 0.45 by default.
- Divisions: the light direction divides by the distance to the light with a small floor (0.001); the phase denominator is floored above 0; the cookie texture coordinates may fall outside 0..1 (CLAMP handles this, so the cookie reads its edge value, which is the cookie's own edge - it must be treated as CLAMP, not wrapped).
- Depth read: the depth buffer must be read at mip level 0 with POINT filtering; a filtered or mipped read changes edges.
- HDR: the beam stays linear HDR; no tonemap, no clamp to 1. Clamping here would cut the bright core of the shaft.
- Rect frames (RenderRect, RenderScale): the Vulkan route is off in rect frames (section 7), so the D3D9 technique draws. The D3D9 path draws the full target as written; the shader does not look at the rect. The new implementation must work in both paths and must not assume that the render target is the full frame in the rect case.
- Reversed depth: the depth is read as linear view distance over the far distance (C++ TESR_CameraData y). The shader does not handle a reversed-depth buffer; the depth as it arrives in this chain is assumed normal (near = 0, far = 1 in the stored value).
- TAA jitter: the beam is computed at half resolution, and the composite reads it bilinearly. The shader itself applies no jitter; the view matrix as supplied by the frame is used as is.
- Single-pass only: there is no shadow map, so the shaft is not occluded by geometry between the light and a sample. The shaft therefore shows through walls and props that stand between the light and the viewer. This is a known first-pass limit, not a bug to fix in this effect.

## 7. Vulkan path

Files:
- Vulkan pass: src/hlsl/NewVegas/Compute/FlashlightBeam.frag (GLSL 450, includes Common.glsl). The built binary sits beside it as FlashlightBeam.spv.
- The Flashlight composite that reads the beam, for the same buffer: src/hlsl/NewVegas/Compute/FlashlightCombine.frag (its in-air shaft term, line group near the beam comment). Its Vulkan binding for the beam is the Flashlight C++ side (set 1, binding 6, sampled image: TESR_VolumetricBuffer).
- The D3D9 twin: src/hlsl/NewVegas/Effects/FlashlightBeam.fx.hlsl (the Vulkan file is documented as the same algorithm; the early return of the D3D9 shader is replaced by a zero-length march so every texture read is in uniform control flow, with the same (0, 0, 0, 1) result).
- C++ side: src/effects/FlashlightBeam.cpp (the Vulkan path is the override of the effect base class's extended render hook in the same file) and src/effects/FlashlightBeam.h.

How the C++ chooses (src/core/EffectRecord.cpp, the base class Render):
- The Vulkan route runs when all of these are true: the `[Main.Main.Performance] NvrDxvkPasses` setting is true, the DXVK extension is available and not failed, the frame is not a rect frame (RenderRect not active), the effect is not named in `[Main.Main.Performance] NvrDxvkKeepD3D9` (a test list), and the technique has no SM6 build covering it (`NvrSm6Shaders`; none exists for FlashlightBeam). These checks are made in the shared extension gate and the base class render.
- If the Vulkan pass cannot be built or any of its inputs is missing (depth texture, cookie texture, any constant, the sampler pair, the uniform buffer), the effect returns to the D3D9 technique for that frame. A failed pass sets the effect's Vulkan path to off for the session and logs it once.
- The beam's Vulkan pass is created once (on the first attempt) with the bindings listed below. Each frame it draws into the beam's half-resolution surface (one colour target) using the full-screen triangle with the screen-quad UV mode. The frame chain is not touched.

Bindings (in the C++ binding list order):
- set 1, binding 0: sampled image, depth (TESR_DepthBuffer), 2D.
- set 1, binding 1: sampled image, cookie (TESR_SpotLightTexture), 2D.
- set 2, binding 0: uniform buffer, the constants block below (std140, 336 bytes; the C++ asserts the size).
- Samplers are indexed from push data, not bound as sampler descriptors:
  - sampler index at push offset 0: POINT, CLAMP (for the depth buffer).
  - sampler index at push offset 4: LINEAR with mips, CLAMP (for the cookie).
- Push data: 8 bytes, two unsigned 32-bit values (pointSamplerIndex at 0, cookieSamplerIndex at 4).

Constants block (one std140 uniform block named Constants in the shader, filled by the C++ each frame). Offsets are bytes from the start of the block; each vec4 is 16 bytes, each matrix is 64 bytes:

| Offset | Name in the block | Equivalent D3D9 name | Type |
|---|---|---|---|
| 0 | volumetricControl | TESR_VolumetricControl | vec4 |
| 16 | volumetricData | TESR_VolumetricData | vec4 |
| 32 | spotLightPosition | TESR_SpotLightPosition | vec4 |
| 48 | spotLightColor | TESR_SpotLightColor | vec4 |
| 64 | spotLightDirection | TESR_SpotLightDirection | vec4 |
| 80 | sunColor | TESR_SunColor | vec4 |
| 96 | reciprocalResolution | TESR_ReciprocalResolution | vec4 |
| 112 | cameraPosition | TESR_CameraPosition | vec4 |
| 128 | cameraData | TESR_CameraData | vec4 (x near, y far) |
| 144 | flashLightViewProj | TESR_FlashLightViewProjTransform | mat4 |
| 208 | viewTransform | TESR_ViewTransform | mat4 |
| 272 | projectionTransform | TESR_ProjectionTransform | mat4 |

The matrices are the D3DX matrices copied as they are stored in memory. The shader indexes them as its element [i][j] = the D3D9 matrix's element [i][j]; mul(vector, matrix) in D3D9 becomes matrix times vector in the shader. The C++ copies the matrices with the same memory layout as the D3D9 constants.

Vulkan-specific behaviour the new version must keep: no early return. A miss sets the step length to zero, so the loop adds nothing, and the final colour is chosen by the hit test (hit: in-scatter and transmittance; miss: (0, 0, 0, 1)). All cookie and depth reads happen every pixel.

## 8. Look and cost

Correct result:
- Air inside the flashlight cone shows a soft, faint shaft that is brightest where the beam points toward the viewer and fades across the beam.
- The shaft has no hard edge at the cone boundary; it fades with the cone falloff and the range.
- Near a wall or the ground the shaft fades to nothing over about 256 units rather than stopping on the surface.
- The shaft is steady: no static banding of the step count, no temporal shimmer.
- In first person there is a visible shaft, not a fog over the screen; the cookie's pattern is flattened there.
- Turning the flashlight or the beam off removes the shaft at once.
- The shaft keeps its brightness ratio to the flashlight's surface pool as the sun and exposure change.

Typical wrong results:
- Hard-edged cone outline in the air (the cone falloff or the cookie is missing, or the segment is not bracketed).
- Bands or rings in the shaft (step count or the noise offset is wrong).
- Shaft through walls and props between the light and the viewer (known first-pass limit).
- Shaft bright on the eye when the flashlight is held close (camera fade missing).
- Shaft lit in first person as a flat fog across the whole screen (first-person strength floor or the camera fade floor not applied).
- Shaft visible after switching the flashlight off (gate not cleared).
- (Wave 3: the spec's own list is kept; the fixed interleaved-gradient offset uses the full-frame pixel, from TESR_ReciprocalResolution.)
- Shaft that is too bright at night or too dim in daylight (sun luminance normalisation wrong).
- Shaft that stops at the sky horizon (MaxDistance or the sky depth handling wrong).

Cost class: per-pixel loop, single pass at half resolution, 24 fixed samples per pixel, each with one cookie texture read and a few exponentials; the segment test first skips every pixel whose ray misses the cone. Multi-pass: no. Its cost is mostly the number of pixels the cone covers.

## 9. Open questions

- Setting ranges: the defaults file gives no range for any of the eight float or bool keys except as implied by the comments; the menu's own range for each key is not established from this read (the ImGui menu source was not read for this effect).
- Whether the frame draws the beam while a menu is open is not determined here; the effect itself has no menu rule.
- Max quality: the C++ comment says the beam buffer is "full with Max Quality" next to a half-resolution call. The Max Quality code path for this buffer was not traced; the spec assumes half resolution in the normal case.
- The D3D9 register numbers of the constants are assigned by the compiler and were not read from a compiled shader; the C++ binds by name, so the names are what matters.
- The Vulkan extension's coordinate convention for the screen-quad UV mode and the half-pixel rule were not read in full (see src/core/EffectRecord.h notes); the Vulkan shader assumes the same UV as the D3D9 full-screen triangle.
- Whether the beam's interplay with EdgeFix value 2 in the Flashlight composite (the beam sampled at edges) needs a separate spec: not covered here beyond the buffer contract.
8. Wave 3: the in-cone segment is bracketed analytically: the range sphere, the cone quadric (between its roots when the ray is steeper than the cone's wall; loose otherwise) and the nappe. Loose brackets are allowed by the spec; the exact cone is tested at each sample.
9. Wave 3: the cookie is read with the implicit mip level (the march loop is unrolled, so the derivatives are in uniform flow). The D3D9 and Vulkan passes both do this.
10. Wave 3: the Henyey-Greenstein normalisation (1 / 4 pi) is the standard one. The spec's calibration factor 0.0008 is kept as written, so the overall level is the spec's; if the old level is wanted, the factor is the place to tune it.
11. Wave 3: the sun-luminance floor of 0.05 uses the Rec. 709 luma of TESR_SunColor.
12. Wave 3: the surface fade is smoothstep over 256 units from the scene distance (the spec says "smooth ramp").
13. Wave 3: rect frames: the D3D9 pass draws the whole beam target and maps its uv with the same rect helpers as the other effects (identity with no rect). Not run in a rect frame.
- The noise offset is per screen pixel and static; whether TAA jitter should move it was not decided in the code.
- Reversed depth: the beam assumes the normal depth convention in this chain; no evidence was checked for a reversed-depth frame.
