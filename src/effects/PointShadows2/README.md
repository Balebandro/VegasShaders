# PointShadows2 - effect spec

REWRITTEN 2026-11-02 (branch claude/rw3-lights): the shaders are new, written from this spec (Effects/PointShadows2.fx.hlsl; Compute/PointShadows2.frag). The C++ and the settings are unchanged. Still open: the D3D9 read-after-write (section 6 and section 9 item 9).

files in Compute (PointShadows2.frag, PointShadows2.spv, PointShadowsCommon.glsl for comparison), resource/VegasShaders.defaults.toml,
core/ShaderManager.cpp (frame order), core/EffectRecord.cpp (dispatch), core/DxvkCompute.cpp/.h (Vulkan switch).

---

## 1. Purpose

PointShadows2 darkens or lights the scene for point lamps that PointShadows does not handle. It is a screen-space pass over the
point-shadow buffer: for every pixel it reconstructs the world position from depth and the world normal from the normals buffer, then
adds, to the value already in the buffer, the light that lamps 6 to 11 deliver to that pixel (each shadowed lamp is tested against its
cube shadow map; lamp 11 is lit without a shadow test). The player sees lamps past the first six cast their light and their shadows
on walls, floors and actors, indoors and outdoors wherever the point-shadow pass runs. It is a second pass in the same buffer, not a
composite over the picture.

## 2. When it runs

- Only inside the point-shadow block of the pre-tonemapping stage (ShaderManager, RenderEffectsPreTonemapping), after depth and normals
  exist and after the ray-tracing, capsule and back-depth steps. The order inside the block is: optional lamp mask, the main point-shadow
  pass (PointShadows), the outdoor batches (if ExteriorLightPoints is above 12), then PointShadows2, then the outdoor ray-traced lamp
  mask, then the restore of the fade constant. The indoor lamp-contact pass comes after the whole block.
- The block runs when the buffer is needed: outdoors when the exterior shadows effect is enabled, indoors when the interior shadows
  effect is enabled, except the case where indoor forward point shadows are drawing the lamps themselves and keep no darkening (then the
  buffer is cleared and this effect does not run).
- Gate, exact: it is called only when the loaded PointShadows shader does NOT have the technique named MergedPointShadows AND the
  interior setting LightPoints is greater than 6. The shipped PointShadows.fx has MergedPointShadows, so with the shipped shader this
  effect does not run at all. It runs for a custom or older PointShadows shader that lacks that technique (its lamps 6 to 11 then take
  the original second pass, which is this effect).
- Outdoors it also runs when LightPoints is above 6 (the same setting is read outdoors).
- Before the draw, the fade constant TESR_ShadowFade.z decides the lamp shadows: it is 0 when point shadows are off for the current
  time of day (exteriors: UsePointShadowsDay / UsePointShadowsNight), 1 indoors, and forced to 0 for the indoor forward-shadow case.
  With z at 0 every cube lookup returns "lit" (see section 6).
- Enabled state: the effect has no section of its own in the defaults file. Its on/off comes from the shader menu's status record for
  the name PointShadows2 (see section 4 and section 9).
- Render target must be the shadow pass surface, technique index 0, and the clear flag must be off. Any other call draws nothing.
- Frame chain: it is not a chain step. It writes the shadow pass surface directly and does not run through the post-process chain.

## 3. Inputs

### Textures (D3D9 sampler registers, declared order)

| Slot | Name | Holds | Filtering / addressing |
|---|---|---|---|
| s0 | TESR_DepthBuffer | linear depth, value = view depth / far distance (one channel) | linear, clamp |
| s1 | TESR_NormalsBuffer | view-space normal encoded 0..1 (xyz) | point (no filter), clamp |
| s2 | TESR_PointShadowBuffer | the point-shadow buffer, format G16R16, full screen; the pass's own target | linear, clamp |
| s3 | TESR_ShadowCubeMapBuffer6 | shadow cube map of shadow lamp slot 6 | linear, clamp, cube |
| s4 | TESR_ShadowCubeMapBuffer7 | slot 7 cube map | linear, clamp, cube |
| s5 | TESR_ShadowCubeMapBuffer8 | slot 8 cube map | linear, clamp, cube |
| s6 | TESR_ShadowCubeMapBuffer9 | slot 9 cube map | linear, clamp, cube |
| s7 | TESR_ShadowCubeMapBuffer10 | slot 10 cube map | linear, clamp, cube |

Slot 8 (cube map 11) is reserved and unbound: lamp slot 11 is never shadow-tested. Samplers are bound by declaration order.
An absent texture makes the pass skip (in the Vulkan path, the D3D9 pass runs instead).

### Constants (D3D9 registers are not fixed by Vegas Shaders; the compiler assigns them and the engine finds them by name)

| Name | Type | Meaning and units |
|---|---|---|
| TESR_CameraPosition | float4 | xyz: camera world position, world units. w unused |
| TESR_CameraData | float4 | y: far distance (world units). Used to turn the depth texel into view distance. x (near) and z, w not used here |
| TESR_ViewTransform | float4x4 | world-to-view matrix as the engine uploads it. Used for the view-space to world direction and for the normal rotation |
| TESR_ProjectionTransform | float4x4 | projection matrix. Only its [0][0] and [1][1] entries are used (horizontal and vertical focal scale) |
| TESR_ShadowFade | float4 | x: sun-shadow fade (not read here). y: not read here. z: point lamp shadows on (1) or off (0). w: reach of point shadows (not read here) |
| TESR_ShadowLightPosition[12] | float4 x 12 | per lamp slot: xyz world position of the lamp, w light radius. w = 0 means the slot is empty |
| TESR_LightColor[24] | float4 x 24 | entries 0 to 11 are the shadow lamps' tracked colours (not read here). Entries 6 to 10 are read for the shadow lamps: rgb colour, w dimmer (multiplier). Entry 11 is not read (its lamp has no colour weight) |

Only the entries listed above are read. Empty slots are zero (position w 0, colour 0), so they add nothing.

### Vulkan-side inputs
The same values, packed in one uniform block (section 7). Textures at set 1 bindings 0 to 7 in the same order as above, with the
shadow buffer as a copy (section 6).

## 4. Settings

PointShadows2 has NO settings section of its own. Its on/off is the shader menu status for the name PointShadows2 (no defaults entry
exists for it, so the menu treats it as a status switch; the saved value is not in the defaults file). The keys below decide whether
and where it runs.

| Section | Key | Type | Range | Default (defaults file) | Effect on this pass |
|---|---|---|---|---|---|
| [_Shaders.ShadowsInteriors.Main] | LightPoints | int | 0 to 64 (clamped to the engine maximum of 64) | 24 | Must be above 6 for this pass to run. Caps how many interior lamps are shadowed; the pass only covers the lamps in the shadow slots it reads |
| [_Shaders.ShadowsInteriors.Main] | ExteriorLightPoints | int | 0 to 44 | 12 | Outdoors, lamps beyond 12 are drawn in batches; this pass is unaffected except through the same gate |
| [_Shaders.ShadowsInteriors.Main] | ShadowCubeMapSize | int | 64 to 2048 | 512 | Resolution of each cube map this pass samples. Not read by the pass itself |
| [_Shaders.ShadowsExteriors.Main] | UsePointShadowsDay | bool | 0 or 1 | false | Outdoors in daytime: when 0, the fade constant z is 0 and every lamp counts as unshadowed |
| [_Shaders.ShadowsExteriors.Main] | UsePointShadowsNight | bool | 0 or 1 | false | Outdoors at night, same effect as the daytime key |
| [Main.Main.Performance] | NvrDxvkPasses | bool | 0 or 1 | true | Selects the Vulkan pass (section 7) when the Vegas Shaders DXVK extension is present |
| [Main.Main.Performance] | NvrDxvkKeepD3D9 | string list | names separated by commas | empty | If this effect's name is listed, the Vulkan pass is skipped and the D3D9 pass is used (a test tool) |

Keys this pass does NOT read, though they sit next to it: NearFade and EdgeFade (of [_Shaders.ShadowsInteriors.Main]), DrawDistance
(the interior reach), and the ForwardReachMask, LampReach and edge-window settings that PointShadows uses. Those belong to PointShadows
and do not change this pass.

Visual meaning of the settings for this pass: LightPoints and the UsePointShadows keys decide whether lamps 6 to 11 get shadows at all;
with them off the pass still lights its lamps unshadowed or does not run.

## 5. Passes and outputs

- One unnamed technique with one pass (technique index 0). The C++ requests technique 0 only; any other index draws nothing.
- Vertex stage: the engine's full-screen frame vertex (position and texture coordinate, pass-through).
- Pixel stage: the shadow lamp accumulation described in section 6. Output is opaque (no blending, the target's old value is replaced
  by the new one). Output colour is the value v in all channels: (v, v, v, 1). Because the target is G16R16, the red and green
  channels receive v; the consumers of the buffer read red.
- Render target: TESR_PointShadowBuffer's surface (the shadow pass surface of ShadowsExteriors). Format G16R16, the size of the frame
  (rect size when RenderScale is on), one level.
- Clear: none. The pass reads what the earlier point-shadow passes wrote and adds to it. It must never clear.
- Result range: clamped to 0..1.

## 6. Behaviour details

### Per-pixel result
For each pixel the result is: the buffer's previous red value, plus the lamp light of each of lamps 6 to 11 at the pixel, clamped to 0..1.
The buffer's previous value is that pixel's own texel only; no neighbour is read from the buffer.

### Reconstructing the position and normal
- The depth texel times far distance gives the view distance along the view ray at the pixel. The view ray direction is built from the
  projection's horizontal and vertical focal scales and the view matrix's axes, as the engine's depth helper does. The world position is the
  camera position plus ray times distance.
- Depth coordinates are rect coordinates (RenderScale): the position and normal lookups use them as they come. The ray construction maps them to
  screen coordinates through the rect helper; the Vulkan copy does not, which is why the Vulkan pass is off in rect frames.
- Normal: the normals texel is decoded from 0..1 to -1..1, given w = 1, and rotated by the view matrix (the normal's view-to-world step
  uses the view matrix as a row vector, i.e. the same product the engine's normal helper uses). No normalisation is applied to the rotated normal.

### Lamp light for one slot (lamps 6 to 10, with shadows)
1. Light vector = lamp position minus world position. Normalised distance = length of that vector divided by the lamp radius.
2. If the lamp's w is 0 the slot contributes nothing.
3. Shadow test: the vector from the pixel to the lamp, with x and y negated and z kept, is the cube-map direction. The cube map's stored
   value at that direction is compared with the normalised distance: the lamp is unblocked when the stored value plus 0.018 times the
   distance is greater than the distance. The bias grows with distance, as stated in the engine's comment.
4. The test only applies where the stored value is strictly between 0 and 1. Outside that (empty texels, which are 0, or saturated ones,
   which are 1) the lamp counts as unblocked.
5. When the fade constant z is 0 the test is skipped and the lamp counts as unblocked (value 1). This is the "shadows off" state.
6. Attenuation (the lamp's falloff, same for all slots): with s = clamp(distance squared, 0..1), the falloff is (1 - s) squared divided by
   (1 + 5 s), clamped to 0..1.
7. Facing term: the normalised light direction dotted with the normal. It is blended with 1 by the smoothstep of the distance over 0 to 0.2
   (light bleeding at very close range): near the lamp the facing term fades to 1, beyond 20 percent of the radius it is used as is.
   The facing term is clamped to 0..1 after the product.
8. Lamp amount = shadow test value times falloff times facing term, clamped to 0..1.
9. Added to the total: lamp amount times luma of the lamp colour (Rec.709 weights, 0.2126, 0.7152, 0.0722) times the colour's dimmer
   (w component). Slots 6 to 10 all use this form.

### Lamp 11 (no shadow test)
Lamp slot 11 is lit without a shadow test and without a colour weight. Its contribution is the falloff times the facing term, with
the same distance and facing rules, and no luma and no dimmer. Its radius is still used to normalise the distance. It is therefore
not dimmed by the lamp's colour: a deliberate-looking but unweighted term that a rewrite must keep.

### Edge cases that would look wrong if missed
- Sky and far pixels: no sky mask. The depth at the sky is at the far distance, so the world position lies beyond every lamp's radius and
  the falloff goes to 0. A rewrite must produce 0 from sky pixels, not a lit sky.
- Zero-length light vector (camera or pixel exactly at a lamp): the normalisation is undefined. The original does not guard it. The rewrite must
  give 0 (or the falloff's value at distance 0 with a defined facing term), never NaN, which would leave a black or bright speck.
- Lamp radius 0 with a nonzero vector: distance is infinite, falloff 0, so the lamp adds nothing. Radius 0 with a zero vector is the NaN case above.
- Empty slots: w 0 gives nothing for slots 6 to 10 by the explicit check; slot 11 relies on falloff at infinite distance, which gives 0.
- Output is clamped at the end, so partial sums above 1 are fine within the pass; the stored value is clamped.
- Reverse or inverted depth: not handled here. The depth buffer stores linear depth (view distance over far), so no depth-convention switch exists.
- HDR vs LDR: the buffer is a 0..1 G16R16 target; this pass works in that range and does no tone mapping.
- First-person weapon and player-held objects: no special case. Whatever depth and normals say is lit.
- TAA jitter: none; the pass uses the frame's texture coordinates as they are.
- RenderScale / rect frames: the texture coordinates are in rect space; the position reconstruction maps them through the rect helper (D3D9 path only).
  Vulkan path is not used in rect frames.
- Read-after-write hazard: the D3D9 pass reads the buffer it writes. The intended result is the value from before this pass, at the same
  pixel. A new implementation must read a copy taken before the pass (the Vulkan path does this with a same-size copy made each call).
  Reading the live target gives undefined results on some drivers.
  Wave 3 status: the Vulkan pass reads a copy (done). The D3D9 pass (Effects/PointShadows2.fx.hlsl) still reads TESR_PointShadowBuffer
  directly, because the copy would have to be made by the C++ (EffectRecord / PointShadows2.cpp), which wave 3 does not change. OPEN.

### Interaction with the indoor forward path
When indoor forward point shadows draw the lamps (the indoor forward-shadow case) the fade constant z is forced to 0 before this pass and restored after.
Every lamp then counts as unshadowed here. The pass still adds lamps 6 to 11 unshadowed (weighted by distance and facing) in that case.

## 7. Vulkan path

Files (in src/hlsl/NewVegas/Compute):
- PointShadows2.frag: the pass in GLSL, 4.50, with its own copies of the lamp functions. It does NOT include PointShadowsCommon.glsl
  (that file belongs to PointShadows; the two sets of functions are separate copies, except for the shared helper header Common.glsl,
  which gives the sampler-heap macro and luma).
- PointShadows2.spv: the compiled pass (the engine loads this file by the name "PointShadows2"). Wave 3: rebuilt from the new .frag (see section 9 item 3).

How C++ chooses: effects/PointShadows2.cpp overrides the extension-render entry of the effect base. The choice is made in the effect
base's render (core/EffectRecord.cpp): the Vulkan path runs when the Vegas Shaders DXVK switch is on (NvrDxvkPasses on, not a rect frame, and the
extension is available), the effect's name is not in NvrDxvkKeepD3D9, and the technique is not replaced by an SM6 build (none exists for
this effect). If the Vulkan entry returns false, the D3D9 pass runs for that call. Conditions that return false: technique index not 0;
clear requested; target not the shadow pass surface; a texture missing; a register-table entry missing; the pipeline fails to build; or the
constants or samplers are missing. The pipeline is tried once and kept.

Render steps of the Vulkan path (in order): copy the shadow buffer into a same-size, same-format texture (made once and remade when the
size or format changes); fill the constants block from the registered values; bind the textures and samplers; draw the full-screen
pass into the shadow pass surface with no blending.

### Bindings
- Set 1 (textures, binding = slot index): binding 0 depth (2D), 1 normals (2D), 2 the shadow buffer copy (2D), 3 to 7 the cube maps of
  slots 6 to 10 (cube). Samplers are not in these bindings.
- Set 2, binding 0: one uniform block, std140, 752 bytes (the C++ checks this size at compile time). Byte offsets:
  - 0: camera position (vec4), TESR_CameraPosition
  - 16: camera data (vec4), TESR_CameraData
  - 32: view transform (mat4, 64 bytes), TESR_ViewTransform, stored as the D3D9 upload stores it
  - 96: projection transform (mat4), TESR_ProjectionTransform
  - 160: shadow fade (vec4), TESR_ShadowFade
  - 176: shadow lamp positions, 12 vec4 (192 bytes), TESR_ShadowLightPosition
  - 368: lamp colours, 24 vec4 (384 bytes), TESR_LightColor
- Matrix convention: the Vulkan side indexes a matrix as the HLSL's [row][column] does; the products that the HLSL writes as "vector times
  matrix" are written the same way in the Vulkan shader. Keep this order when rewriting.
- Push constants: 8 bytes. Offset 0: sampler heap index for LINEAR, CLAMP (used for the depth, the shadow copy and all cube maps). Offset 4:
  sampler heap index for POINT, CLAMP (used for the normals only). The engine fills both indices.
- Sampler heap: set 0, binding 0 (engine-owned).
- Output: one colour target, the shadow pass surface's format (G16R16), opaque.

## 8. Look and cost

Correct look: lamps 6 to 11 add their light where they reach. Shadowed areas behind walls or furniture from those lamps stay dark
(the lamp's light is removed there), lit surfaces facing the lamp take the full falloff, and nothing lights the sky. Edges of the lit
area follow the cube-map shadow edges, which are sharp at the cube resolution (512 by default). Shadows fall off with distance to
the radius; past the radius nothing is added. Lamp 11 always appears lit with no shadow, so it can light a wall behind another object.

Typical wrong results:
- Lamp 11 looks like it casts no shadow (correct, by design, but a visible mismatch with other lamps).
- A black or white speck at the lamp centre (the zero-vector NaN case).
- Light on the sky or on distant geometry beyond the radius (missing falloff).
- A lit sky or a bright band at the far edge (sky not kept at 0).
- Whole-screen darkening or brightening when the shadow buffer is not copied (the read-after-write case).
- The shadow pass drawn over a cleared target: this pass must not clear, so lamps 0 to 5 would vanish.

Cost class: per-pixel loop, single full-screen pass. Per pixel: three buffer-style reads (depth, normals, shadow buffer copy), five cube-map
reads (slots 6 to 10), and six lamp evaluations (five with a shadow test, one without). The Vulkan path adds one full-buffer copy per call.
Outdoors it runs at most once per frame.

## 9. Open questions

1. The on/off value of PointShadows2 itself: it has no defaults section. Which value the shader menu status record starts with, and
   where it is saved, was not determined. Whether a fresh install runs it is therefore unknown.
2. Whether the shipped PointShadows.fx's merged technique covers lamps 6 to 11 (and so makes this pass dead code with that file) was not
   checked against the merged technique's body; only its presence was confirmed.
3. FIXED (wave 3): PointShadows2.spv is rebuilt from the new PointShadows2.frag with compile-compute.sh (spirv-val passes).
9. Wave 3: the D3D9 pass's read-after-write (section 6) is still open: it needs a copy of the buffer made by the C++ before the pass.
10. Wave 3: lamp 11 uses the same facing term as the other lamps (the spec's rule); no colour weight, as the spec says.
4. D3D9 register numbers of the constants were not determined (the constant table assigns them at run time).
5. The meaning of the cube-map stored value (normalised distance to the nearest caster, divided by the lamp radius) is inferred from the
   comparison; the producer of the cube maps (the shadow cube drawing code) was not read.
6. The consumer of the buffer's red channel (the composite that darkens or lights the scene) was not read here; the sun-shadow comment in the
   exterior apply code says that channel is shared with the sun term. Whether lamp 6 to 11 values reach the picture the way PointShadows does
   was not confirmed.
7. Whether the Vulkan path's copy of the shadow buffer matches the D3D9 values exactly on every driver was not tested.
8. The Vulkan entry's last argument (the full-screen helper's flag) was not traced; it is assumed to mean "the pass covers the whole target".
