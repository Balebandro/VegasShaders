# Effect spec: Flashlight

REWRITTEN 2026-11-02 (branch claude/rw3-lights): the shaders are new, written from this spec (Effects/Flashlight.fx.hlsl; Compute/Flashlight.glsl, FlashlightShadows.frag, FlashlightDepthBlur.frag, FlashlightLight.frag, FlashlightBoxBlur.frag, FlashlightCombine.frag). The C++ and the settings are unchanged. Wave 3 readings and choices are listed in section 9 (items 10 to 17).

Reference behaviour: the D3D9 effect `src/hlsl/NewVegas/Effects/Flashlight.fx.hlsl` with its C++ `src/effects/Flashlight.cpp`.
Related effect with its own spec, not covered here: `FlashlightBeam` (the in-air shaft). It feeds this effect through one texture (section 3).

## 1. Purpose

The player's flashlight is drawn as a post-process light in the world, replacing the game's Pip-Boy light. A cone of warm, over-bright light falls from the weapon (or from the camera or head when no weapon rides it) onto the lit surfaces in front of the player. Each surface's light depends on its angle to the light, its distance from the light, and the cone's cookie pattern. The light is added to the frame, so the lit pool is brighter than the scene around it. Surfaces that stand in front of the light can cast a soft shadow onto what is behind them, if shadows are enabled.

## 2. When it runs

Enable conditions:
- Effect switch: `[_Shaders.Flashlight.Status]` `Enabled` (default true). The effect also needs its D3D9 effect file to have compiled.
- Light state: the effect is active only when the Pip-Boy light magic effect is on for the player (the game's own check, read every frame in the constants update), or when the hidden test key `TestForceOn` is true. Both must hold with `Enabled`.
- Player has a valid camera and scene graph; if not, nothing is published and the effect does not draw.
- No menu or cell-type gate is applied inside this effect. Interior and exterior differ only in which look is used (section 4).
- Night and underwater: the effect has no explicit night or underwater rule. It runs wherever the active-light condition holds.

Interior and exterior: `GameState.isExterior` is false in an interior cell. Outdoors the `[_Shaders.Flashlight.Main]` look is used; indoors the `[_Shaders.Flashlight.Interiors]` look is used. The look is re-chosen every frame.

Frame order (`src/core/ShaderManager.cpp`):
1. Start of frame: the effect's constants update runs (this publishes the shared spot light constants and the lamp list entry, so every later effect sees the flashlight as spot light 0).
2. Shadowing, ambient light, and bounce light run first (they are not part of this effect).
3. Blood flow runs, then wet world.
4. If the FlashlightBeam effect's control value is above zero, the beam march renders into its own half-resolution buffer.
5. This effect renders: target = the game's scene target, pass set chosen by `TechniqueThisFrame()`, clears the target first, source = the image before the effect.
6. Specular runs after this effect. Specular's merge with bounce light is disabled while this effect renders.

The effect draws on the post-tonemapping or pre-tonemapping HDR surface depending on `RenderPreTonemapping` (section 6).

## 3. Inputs

### 3a. Textures (D3D9 `.fx`, sampler registers in declaration order)

All samplers use CLAMP addressing. The register numbers are fixed by the `.fx` declaration.

| Slot | Name | Holds | Filter |
|---|---|---|---|
| s0 | `TESR_SourceBuffer` | the image before this effect (linear or sRGB depending on `TESR_FlashLightComposite.x`) | LINEAR, no mips |
| s1 | `TESR_RenderedBuffer` | the chain's current image; used as the light buffer in the light, blur and composite passes, and as the shadow mask in the shadow passes | LINEAR, mips declared |
| s2 | `TESR_DepthBuffer` | linear depth (view distance divided by the far plane) in red | declared MIN ANISOTROPIC, effectively LINEAR (no mips) |
| s3 | `TESR_PointShadowBuffer` | declared but not read by this effect | LINEAR |
| s4 | `TESR_NormalsBuffer` | packed world-facing normals (view-space normal is rebuilt from it, see section 6) | LINEAR |
| s5 | `TESR_ShadowSpotlightBuffer0` | the spot light's shadow depth map, or the traced shadow in the traced variant | LINEAR |
| s6 | `TESR_SpotLightTexture` | the cookie, resource `Effects\Flashlight.png` (repo file `resource/Textures/Effects/Flashlight.png`) | LINEAR with mips |
| s7 | `TESR_VolumetricBuffer` | the FlashlightBeam half-resolution shaft | LINEAR, no mips |

Vulkan binding order is the same list without s3 (see section 7).

### 3b. Constants

The `.fx` declares these; the C++ registers the first six by name. Registers are not fixed by the C++; the compiler assigns them.

| Name | Components | Units / meaning | Source |
|---|---|---|---|
| `TESR_FlashLightViewProjTransform` | 4x4 matrix | view times projection of the light, right-handed, 1.0 aspect, field of view 2 x cone angle, near 0.1, far = light radius | registered by this effect |
| `TESR_FlashLightPosition`, `TESR_FlashLightDirection`, `TESR_FlashLightColor` | vec4 each | registered by this effect; the shader does not read them (it uses the shared `TESR_SpotLight*` below) | registered by this effect |
| `TESR_FlashLightTuning` | vec4 | x near fade distance (game units, 0 disables); y soft edge flag (1 = wrap lighting); z hotspot limit (0 disables); w cookie strength 0 to 1 | registered by this effect |
| `TESR_FlashLightComposite` | vec4 | x source-is-linear flag (1 = linear HDR source, 0 = sRGB source to decode); y EdgeFix 2 flag (1 = the shaft also gets the edge treatment); z, w = 1 / half-resolution beam width and height | registered by this effect |
| `TESR_SpotLightPosition` | vec4 | xyz light position in world units; w light radius in world units (the Distance setting) | shared, published by the effect's constants update |
| `TESR_SpotLightDirection` | vec4 | xyz unit axis of the light; w outer cone angle in degrees (the Angle setting) | shared |
| `TESR_SpotLightColor` | vec4 | rgb light colour (the ColorR, ColorG, ColorB settings); w dimmer (the Dimmer setting) | shared |
| `TESR_SunColor` | vec4 | the sun colour; only its luma is used (to normalise the flashlight brightness) | shared |
| `TESR_CameraPosition` | vec4 | eye position in world units | shared |
| `TESR_CameraData` | vec4 | x near plane, y far plane (game units) | shared |
| `TESR_ViewTransform`, `TESR_ProjectionTransform` | 4x4 each | camera view and projection | shared |
| `TESR_ReciprocalResolution` | vec4 | 1 / frame width, 1 / frame height, in xy | shared |
| `TESR_VolumetricControl` | vec4 | x beam strength; 0 when the FlashlightBeam effect is off; the rest unused here | shared |
| `TESR_RenderScale` | vec2 | the drawn part of the frame (used only by the box blur, D3D9 only; see section 6) | shared |
| `TESR_DebugVar` | vec4 | declared, used only by an unused debug helper | shared |
| `TESR_SunDirection`, `TESR_CameraForward` | vec4 | declared, not read by this effect | shared |

The light's shadow matrix uses the registered view-projection matrix. The depth constants in the depth include (`TESR_DepthConstants`, inverse matrices) are declared but not needed by this effect's reads.

## 4. Settings

All keys are read in `UpdateSettings` and `ApplyLook`. Ranges are the ones the defaults file implies; the C++ clamps only `EdgeFix`.

Section `[_Shaders.Flashlight.Main]` (outdoor look, and common settings):

| Key | Type | Range | Default | Meaning |
|---|---|---|---|---|
| ColorR, ColorG, ColorB | float | any; values above 1 are allowed (brighter than white) | 2.0, 2.0, 1.0 | colour of the beam (warm yellow-white at default) |
| Dimmer | float | any, 0 off | 1.0 | overall strength of the beam |
| Angle | float | degrees | 30.0 | the cone's outer edge, measured from the axis; the full-strength edge is half this angle; the cookie projection field of view is twice this angle |
| Distance | float | game units | 1000 | light radius: nothing lights beyond it |
| OffsetX, OffsetY, OffsetZ | float | game units, in the weapon's or camera's frame | 0.0 | shift of the light position relative to the weapon or camera |
| RenderShadows | bool | | false | use the shadow map (technique with shadows) |
| AttachToWeapon | bool | | true | the light rides the weapon bone instead of the camera or head (third and first person) |
| NearFade | float | game units, 0 disables | 0.0 | darkens the light on surfaces closer to the light than this |
| HotspotLimit | float | 0 disables | 0.0 | soft compression of the brightest part of the added light |
| CookieStrength | float | 0 to 1 | 0.0 | procedural rings and mottling in the beam |
| SoftEdges | bool | | false | wrap lighting instead of clamped diffuse (removes speckle on foliage) |
| HidePipBoyLight | bool | | true | hidden in the developer list; while the flashlight is on, the game's Pip-Boy light gets no brightness and no reach; restored when off |
| TestForceOn | bool | | false | hidden test switch: forces the light state on |
| EdgeFix | int | 0, 1, 2 (clamped) | 1 | 0 off; 1 depth-aware edge treatment for the lit surfaces; 2 also for the beam shaft |

Section `[_Shaders.Flashlight.Interiors]` (indoor look): ColorR, ColorG, ColorB (defaults 2.0, 2.0, 1.0), Dimmer (1.0), Angle (30.0), Distance (1000), NearFade (0.0), HotspotLimit (0.0), CookieStrength (0.0). Offsets, shadows, attach, soft edges and EdgeFix are shared with the Main section and are not read from Interiors.

Section `[_Shaders.Flashlight.MaterialLight]`: Enabled (false), Intensity (0.015), Specular (0.5), NormalStrength (1.0), MaxGeometry (2048), DebugMode (0). The C++ copies these into its settings, but the drawing of the re-lit geometry is done in the material pass (`src/NewVegas/Hooks/MaterialPass.cpp`), which reads its own copy. This effect's shader does not use them. See section 9.

Section `[_Shaders.Flashlight.Status]`: Enabled (true), the effect switch.

Related setting read by the enclosing frame code: `[Main.Main.Performance]` `NvrDxvkPasses` decides whether the Vulkan passes are used (section 7).

## 5. Passes and outputs

Two unnamed techniques exist in the `.fx`, plus two named techniques `EdgeFix` and `EdgeFixShadows`. The C++ asks for them by index (0 and 1) or by name (`EdgeFix`, `EdgeFixShadows`, looked up once per loaded effect). A technique missing from the loaded `.fx` is treated as absent and the original techniques run.

Selection:
- RenderShadows off, EdgeFix 0: technique 0.
- RenderShadows on, EdgeFix 0: technique 1.
- RenderShadows off, EdgeFix 1 or 2: technique `EdgeFix`.
- RenderShadows on, EdgeFix 1 or 2: technique `EdgeFixShadows`.
- A traced shadow (section 7, FlashlightShadowsRT) forces the shadow technique regardless of RenderShadows (`EdgeFixShadows` if EdgeFix is on and it exists, else technique 1).

Pass list per technique, in order. Each pass writes the image that the next pass reads (through the chain's current image).

Technique 0 (no shadow, EdgeFix off): (1) no-shadow pass, which writes 1 everywhere; (2) light pass; (3) min box blur; (4) average box blur; (5) composite, unmodified version.

Technique 1 (shadows, EdgeFix off): (1) shadow pass; (2) depth-aware blur horizontal; (3) depth-aware blur vertical; (4) light pass; (5) min box blur; (6) average box blur; (7) composite, unmodified version.

Technique `EdgeFix`: as technique 0, with the composite in its depth-aware form.

Technique `EdgeFixShadows`: as technique 1, with the composite in its depth-aware form.

Output of every pass: a colour target the same size as the frame (or the game's scene target), with the same format as the chain. Only the composite pass writes to the game target; the earlier passes write to the chain's intermediate target.

What each pass must produce:
- No-shadow pass: exactly 1.0 in every channel. This is the stand-in for an unshadowed shadow term.
- Shadow pass: a shadow mask in every channel (1 lit, 0 in shadow), inside the light's reach and inside the cookie frame (see below). Outside that region, 1 (lit).
- Depth-aware blur: blurs the shadow mask along one axis, 12 taps plus the centre, weighted, excluding taps whose depth differs from the centre by more than a threshold (350 game units, scaled by the centre's depth divided by the far plane). Pixels beyond the reach are cleared to black with alpha 1.
- Light pass: the lit pool. RGB = light to add, alpha 1. Written before any blur.
- Min box blur: the minimum of five taps (centre, and four diagonal neighbours half a texel away) of the light pass. Blur "erodes" the pool slightly.
- Average box blur: the mean of the same five taps of the min-blurred image, RGB only, alpha 1.
- Composite: the scene plus the added light, output in the same colour space as the input (see section 6). Alpha 1.

Scope of the blur passes: the box blur passes take a scale of 1.0 in the D3D9 version; the drawn part of the frame (`TESR_RenderScale`) limits them, and pixels beyond it are cleared.

## 6. Behaviour details

Depth and world position: the depth buffer holds linear depth divided by the far plane; the view distance is that value times the far plane. A pixel's world position is the camera position plus the view ray (the view direction rebuilt from the view and projection matrices) times that distance. The sky has no geometry: its depth is the far end, so the light is zero there by the distance falloff (the light radius is much smaller than the sky distance). This is derived from the formulas, not observed.

Normals: the normal is read from the normals buffer, unpacked from 0 to 1 into -1 to 1, then rotated by the camera's view matrix to world orientation. Normals are reconstructed from depth where needed, so foliage normals are noisy (the reason SoftEdges exists).

Light model:
- Diffuse: with SoftEdges off, the clamped dot product of the normal and the direction to the light. With SoftEdges on, 0.75 times the square of (dot product times one half plus one half), i.e. wrap lighting at three quarters energy.
- Specular: the normalised half vector between the view direction and the light direction, dotted with the normal, clamped to 0 to 1, raised to the power 5 with sign kept (behaves as a plain power for positive bases).
- Cone: let c = dot(axis, direction from the light to the surface) (wave 3: the spec's "from the surface to the light" would give a negative value inside the cone, so the light-to-surface direction is used). Take the position of c between the cosine of the outer angle (0) and the cosine of half the outer angle (1), clamped to 0 to 1, and square it. Result: 0 outside the outer edge, 1 inside the half-angle edge, smooth in between.
- Distance falloff: with d = distance to light / radius, s = saturate(d squared), attenuation = saturate((1 - s) squared / (1 + 5 s)). It is 1 at the light, 0 at and beyond the radius.
- Near fade (only if NearFade > 0): smoothstep from 0 at distance 0 to 1 at NearFade.
- Light colour: colour times dimmer, divided by max(0.05, luma of the sun colour). This keeps the flashlight visible in bright daylight and also at night, since the sun brightness is the normaliser.
- Light = (diffuse + specular) times light colour times cone times attenuation times cookie times shadow times near fade.
- Luma is Rec. 709 weights (0.2126, 0.7152, 0.0722).

Cookie: the light-space position of the surface is projected with the light's matrix (perspective divide, then 0 to 1 mapping, y flipped). The cookie texture is read there with mips. Outside 0 to 1 the CLAMP address repeats the edge, but the cone term is already 0 there.

Cookie breakup (CookieStrength): if strength is 0.001 or less, the factor is 1. Otherwise: take the cookie's own 0 to 1 coordinates, centre them, compute the radial distance r scaled so the cookie edge is 1 (clamped). Make a ring pattern: sine of (36 r plus a warp), where the warp is a sum of two sines of the coordinates; remap to 0 to 1. Make a mottled pattern: the product of two sines whose arguments are themselves sines, remapped to 0 to 1. Blend the ring amplitude up toward the edge (0.10 at the centre, 0.24 at the edge), add 0.12 times the mottling deviation, and subtract a small edge darkening. The result is then linearly mixed between 1 and the result by the strength (clamped to 1). Exact frequencies are part of the look; a new version should match them as closely as the numbers above allow. (Wave 3: the ring frequency 36 and the amplitudes are the spec's; the warp is 0.4 sin(7 x) + 0.4 sin(5 y) on the centred cookie coordinates x2, the mottling is 0.5 + 0.5 sin(4 sin(3x + 1.7y)) sin(4 sin(2.6y + 0.9x)), and the edge darkening is 0.08 smoothstep(0.7, 1, r): these frequencies are this version's choice.)

Hotspot limit: when HotspotLimit > 0, the added light is compressed so that its luma never exceeds 1 / HotspotLimit, by scaling the whole colour (hue kept) with the Reinhard-style factor limit / max(limit, luma). Dim light is unchanged.

Composite (the colour applied):
- Source colour: if the source is linear (`TESR_FlashLightComposite.x` = 1), used as is; otherwise decode from sRGB (standard sRGB to linear transfer) first, and re-encode at the end.
- Added colour = source colour times max(0, luma(exp(-3.5 times source colour)) times light). This dims the light on already bright surfaces. (Wave 3: read literally. The source colour multiplies the light, as a surface albedo would. The hotspot limit is applied to this light part only, before the beam is added.)
- If the beam is on (`TESR_VolumetricControl.x` > 0), the shaft is added (section 7 for its own pass). The shaft is dimmed where the surface is already inside the cone (a geometric test on the surface, at 0.85 strength at most), then soft-capped by dividing by 1 + 2.5 times its luma.
- Final colour = source colour + added colour (with the hotspot limit applied to the added colour first).
- Output is linear if the source was linear, otherwise re-encoded to sRGB.

EdgeFix (EdgeFix 1 and 2; only pixels at depth edges change): the scene colour is a blend of two things at an edge (a leaf and what is behind it), but the depth buffer holds one sample per pixel, so the light of the wrong side can be used. The fix:
- Edge test: compare depth to its eight neighbours; use the four opposite pairs (left/right, up/down, both diagonals). Break size = the largest of the pairs' break measures. Break measure: the absolute value of dc/a + dc/b - 2, where dc is the centre depth and a, b the two neighbours (wave 3 FIXED: the denominators are floored at 1e-4, not 1.0: the stored depth is 0 to 1, so a floor of 1.0 would make every break zero). That is 0 on a flat surface, however steep. Edge strength ramps from 0 at a break of 2% to 1 at 5%.
- Quick gate: first, two bilinear depth reads at opposite corners of the pixel; if their break measure is under 0.5%, treat the pixel as flat and skip the rest.
- Split: at an edge, pick the far reference (the farthest neighbour) and the near reference (the most colour-different of the near neighbours, among the four direct ones, that are nearer than the midpoint depth). Split the pixel's colour into a near part and a far part by projecting onto the difference between the two references. Light each part with its own reference pixel's light, then blend back by the edge strength.
- The split is weighted off where the two references are nearly the same colour (contrast below about 0.25%-1%).
- EdgeFix 2 also gives the beam shaft this treatment: the shaft is upsampled from its half-resolution buffer depth-aware (texels of similar depth to the pixel are weighted, the rest down-weighted by about 15 to 150 times), and at edges each side's shaft counts by its coverage.
- Off edges nothing changes; EdgeFix 0 runs the original composite.

NaN and infinity guards: the `.fx` has no global guard. Divisions are floored (depth ratios floor the denominator at 1.0; the split uses a 1e-10 floor; the hotspot limit uses max(limit, luma); the shaft's normalisation floors the surface distance at 0.001). Keep these floors in any rewrite.

Shadows (RenderShadows on): the shadow test runs only within the light's reach (distance below 1) and inside the light-space frame. There, the shadow map depth (red) is compared to the surface's distance to the light, normalised by the radius, with a bias of 0.05 times that distance. The result is a 0 or 1 mask. The mask is blurred by the two depth-aware passes, which keep edges between a near occluder and far background from smearing.

Traced shadows (`FlashlightShadowsRT`): when RT lamp shadows are on for the flashlight, the shadow map is replaced by a screen-space traced shadow. The same reach and frame test apply; the mask is read at the screen position, not the light position.

First person: the light sits on the camera, so there is no parallax for the shaft. The shaft's virtual origin (for the beam effect only) is moved right and down in camera space by the beam settings; the surface light still uses the true light position. Section 4 gives the beam settings' names and defaults in its own spec.

Third person and weapon riding: with AttachToWeapon on, or while aiming, the light rides the weapon bone (the aim state is latched: aiming starts the ride at once; it ends only after the aim signal has been off for 0.35 seconds of real time, so bursts do not flip bones). Melee weapons and no weapon out fall back to the head bone (third person) or the camera (first person). The head-bone case uses a fixed 90 degree remap of the bone's axes. Offsets are applied in the rider's frame.

Pip-Boy light: while the flashlight is on, the game's Pip-Boy light node is culled, and with HidePipBoyLight its brightness and reach are also zeroed; the original values are kept and restored when the flashlight goes off, or if the player node changes. The flashlight publishes its own light to the lamp list (spot light 0), so other effects see it.

Off state: when the light is off, the spot light constants are zeroed (position, direction, colour, beam parameters); the spot light's dimmer is set to 0 and shadows off.

Render rects (RenderScale v2): the box blur passes read `TESR_RenderScale` and clear pixels outside the drawn part. The Vulkan version does not run in rect frames (section 7).

Reversed depth: this effect reads linear depth, not the projected depth, so reversed-depth projection does not change its maths. Not checked for every frame setting.

TAA jitter: the effect has no jitter handling of its own; it reads the frame's images as they are.

HDR vs LDR: with `RenderPreTonemapping` on (the default), the source is linear HDR and the composite does not decode or re-encode. With it off, the source is the post-tonemapping sRGB image and the composite must decode, then re-encode.

## 7. Vulkan path

Files in `src/hlsl/NewVegas/Compute` with names starting `Flashlight` (15 files):
- `Flashlight.glsl`: the shared part (bindings, the constants block, helpers). Not a pass.
- `FlashlightShadows.frag`: shadow pass (shadow map).
- `FlashlightShadowsRT.frag`: traced-shadow pass (same reach and frame test; reads the traced mask at the screen position). Wave 3: NOT rewritten (not on the removed list). Its working-tree file was overwritten by mistake in wave 3 and is not committed; the committed version stays until the lead restores it.
- `FlashlightDepthBlur.frag`: depth-aware blur, used for both horizontal and vertical passes (direction from the push data).
- `FlashlightLight.frag`: light pass.
- `FlashlightBoxBlur.frag`: min and average box blur (mode selects).
- `FlashlightCombine.frag`: composite, with the EdgeFix path selected by mode.
- `FlashlightBeam.frag`: the beam shaft; belongs to the FlashlightBeam effect, listed here because it shares the prefix.
- Compiled binaries `.spv` exist for each `.frag`; a new version must produce its own.

Selection (`src/effects/Flashlight.cpp`, `ExtRender`), and the general rule in `src/core/EffectRecord.cpp` `EffectRecord::Render`:
- Vulkan passes are used only when `[Main.Main.Performance]` `NvrDxvkPasses` is on, DXVK compute is available (the NVR DXVK build), the effect is not listed in `NvrDxvkKeepD3D9`, the technique has no SM6 build selected by `NvrSm6Shaders` (`Sm6Covers`), and no render rect is active (`DxvkCompute::PassesOn`, which returns false in rect frames).
- Otherwise, or when `ExtRender` returns false, the D3D9 passes run.
- `ExtRender` returns false (for this frame) when the technique is not 0, 1, `EdgeFix` or `EdgeFixShadows`, when the clear flag is off, when a required constant is not registered, when a required texture is missing (depth, normals, cookie, shadow map when shadows are on, beam buffer when the beam is on), or when a pipeline cannot be created.
- If a later pass fails after the first has drawn, the image is copied back from the source buffer so the D3D9 path starts from the image it would have had. A failing pipeline sets the effect's extension-failed flag for the session (logged).

Pass to shader mapping (what `ExtRender` draws, in order):
- Shadows (technique 1 or `EdgeFixShadows`): shadow pass (or traced variant), depth-aware blur with horizontal offset (1, 0), depth-aware blur with vertical offset (0, 1).
- Light pass: mode 0 when shadows are used (reads the shadow result from its input), mode 1 when not (stands in for the no-shadow 1).
- Box blur: mode 0 (min), then mode 1 (average).
- Composite: mode 1 when EdgeFix (techniques `EdgeFix`/`EdgeFixShadows`), otherwise mode 0.

Bindings (set 1, texture bindings 0 to 6, in this order): 0 source (`TESR_SourceBuffer`), 1 rendered (`TESR_RenderedBuffer`, the chain's current image), 2 depth (`TESR_DepthBuffer`), 3 normals (`TESR_NormalsBuffer`), 4 shadow (`TESR_ShadowSpotlightBuffer0`, or the traced mask), 5 cookie (`TESR_SpotLightTexture`), 6 volumetric (`TESR_VolumetricBuffer`). The point shadow sampler (`TESR_PointShadowBuffer`) is not bound because no pass reads it. Set 2, binding 0: one uniform block holding the constants in this order (std140, 352 bytes): light view-projection matrix, view matrix, projection matrix, camera position, camera data, spot light position, spot light colour, spot light direction, sun colour, reciprocal resolution, flashlight tuning, flashlight composite, volumetric control. Each matrix is copied as stored. Set 0, binding 0: linear sampler, clamp. Set 0, binding 1: linear sampler with mips, clamp (used for the cookie only).

Push data (32 bytes, per pass): offset 0, a 2-float offset mask (horizontal (1,0) or vertical (0,1) for the depth-aware blur); offset 8, a uint mode (light pass: 1 = no shadow; box blur: 0 min, 1 average; composite: 1 = EdgeFix); offset 12, a uint index of the linear sampler; offset 16, a uint index of the mipped sampler; the rest padding.

Differences the new version must keep:
- The Vulkan box blur fixes the scale at 1 and does not multiply by the render scale (it is off in rect frames anyway).
- The Vulkan path and the D3D9 path must match pixel for pixel; the Vulkan side was written line for line from the `.fx`.

Shared sampler sync: the depth buffer uses a linear filter in both paths (no mips, anisotropy 1).

## 8. Look and cost

A correct result:
- A clear cone of warm, bright light from the weapon (or the camera), with a soft falloff to zero at the light radius.
- The lit pool brighter than the scene around it; the pool is slightly softened at its edges by the blur (about a pixel). Not hard-edged, not blocky.
- Brightness that stays visible in full daylight and at night (normalised by the sun colour).
- At the boundary of foliage or characters, the bright part follows the leaf, not the background (EdgeFix 1); no rim of background colour, no jagged or flickering edge.
- With RenderShadows on, a soft, dark shadow behind objects in the cone, with no halo on the far side of an edge.
- With SoftEdges on, grass and leaves are lit without speckle, at the cost of a flatter beam.

A typical wrong result:
- A wall a foot away clipped to white (NearFade is off or too small).
- A hard, aliased pool edge (blur missing).
- Speckle on grass or foliage (normals reconstructed from depth, SoftEdges off).
- A bright rim in the colour of the background around leaves (EdgeFix 0 or broken edge split).
- A flicker along edges as the camera moves.
- A bright hot spot that clips to white (HotspotLimit 0 with high Dimmer or colour).
- A flashlight that goes dark in daylight or bright at night because the sun normaliser is missing.
- The Pip-Boy light still lighting nearby objects and their shadows (HidePipBoyLight broken).

Cost class: multi-pass, per-pixel.
- Light pass: a few texture reads (depth, normals, cookie with mips, shadow or the beam) per pixel; the main cost of the effect.
- Box blurs: five taps each.
- Shadows (when on): one shadow read plus two depth-aware blurs of 12 taps each, plus a depth read per tap.
- EdgeFix: an edge test of 8 depth reads plus 2 quick reads, only where light is present; the EdgeFix comment measures about 0.1 ms at 2560x1440 with grass filling the beam.
- Composite: one colour read and one light read, plus the beam read if the beam is on, plus the EdgeFix work when enabled.

## 9. Open questions

1. `TESR_FlashLightPosition`, `TESR_FlashLightDirection` and `TESR_FlashLightColor` are registered by the C++ but not read by the `.fx`. Confirm whether anything else reads them before dropping them from a new version.
2. The MaterialLight settings are read into this effect's settings, but the re-lit geometry is drawn by `src/NewVegas/Hooks/MaterialPass.cpp` from its own configuration. This spec does not define that pass; it needs its own spec, and it is not clear whether the copy in this effect is used at all.
3. The source of `TESR_ShadowSpotlightBuffer0` (the spot light shadow map) is not in `Flashlight.cpp`; the spec assumes it is produced by another part of the engine or the renderer and is not checked.
4. The traced-shadow variant binds the traced image to the shadow slot (section 7). Its texture filter and format were not checked; they are defined in the RT lamp shadows code, not in this effect.
5. Whether `Sm6Covers` returns true for any Flashlight technique (an SM6 build with `NvrSm6Shaders`) was not checked; no SM6 Flashlight files were found in `Compute`.
6. `NvrDxvkPasses` default in the defaults file (line 153) was not confirmed; the code default is off.
7. `Enabled` for `[_Shaders.Flashlight.Status]` is assumed to be read by the generic effect loader from the Status section; the loader code was not traced.
8. The cookie file's pixel layout (which channel is read: the spec says red) is taken from the shader; not checked against the image.
9. The exact effect of `HidePipBoyLight` on indoor lamps was described from its comment, not tested.
10. Wave 3 (2026-11-02): the shadow map's red is read as distance over radius (the spec's wording), and the test is lit when it is at least 95% of the surface's distance over radius (bias 0.05 x distance). Not checked against the C++ light-space depth write.
11. Wave 3: the depth-aware blur uses 12 taps at 1-texel steps with weights exp(-k^2 / 18), k = 1..6 (the spec gives the taps and the threshold, not the weights).
12. Wave 3: the box blur taps are the centre and the four diagonal neighbours at half a texel (as the spec says); the blur is clamped to a render-scale rect in D3D9, not in Vulkan.
13. Wave 3: the EdgeFix far and near references are taken from the eight and four neighbours as the spec describes; the split uses the source colours projected on the far-to-near difference, and the light of each reference (the blurred light buffer) is blended by that share. The spec's exact quick gate (two bilinear reads) is replaced by the four opposite pairs; the 2 % to 5 % ramp is kept.
14. Wave 3: EdgeFix 2 shaft upsample: four half-resolution taps weighted by bilinear weight over 1 + 100 x gap^2 (the spec says the far taps are down-weighted 15 to 150 times; 100 is the middle of that range).
15. Wave 3: the D3D9 shaft is dimmed by 1 - 0.85 x cone at the surface (the cone squared, as the light), then divided by 1 + 2.5 luma. The 0.001 floor is on the light-to-surface distance in the cone test.
16. Wave 3: Vulkan passes (Compute/Flashlight.glsl and the five pass files above) mirror the D3D9 ones; not run in the game. The traced variant FlashlightShadowsRT is not part of wave 3 (see section 7).
17. Wave 3: TESR_FlashLightPosition, TESR_FlashLightDirection and TESR_FlashLightColor are still declared (the C++ registers them); the shader does not read them (item 1 stays open).
