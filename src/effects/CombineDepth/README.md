# CombineDepth

Source of this spec: the rewritten shader (wave 3 buffers, branch claude/rw3-buffers): src/hlsl/NewVegas/Effects/CombineDepth.fx.hlsl (techniques CombineDepth and DepthNormals), src/hlsl/NewVegas/Compute/CombineDepth.frag (Vulkan separate pass). The C++ (src/effects/CombineDepth.cpp) is unchanged. Written from the previous spec and the C++; the old shader was not read.

## 1. Purpose

CombineDepth makes the picture's depth buffer that almost every NVR effect reads. The game's own depth (the world, and the first-person weapon and hands drawn in a separate depth pass) is turned into one linear depth per pixel, in game units, so that the effects can measure distances and rebuild positions without caring which pass the surface came from. The player sees nothing directly: its quality is what makes the other effects' results right (shadows, ambient occlusion, reflections, fog, depth of field and so on).

## 2. When it runs

- Every frame where NVR's effects run: first step of the depth and normals stage in RenderEffectsPreTonemapping, before any other NVR post-process.
- Enabled: no settings section; on whenever the effect loaded. ShouldRender is the default (true).
- Two ways it runs:
  - Separate (the default, MergedDepthNormals off): technique CombineDepth renders the depth target alone; then Normals renders the normals target from it.
  - Merged (MergedDepthNormals on, the normals would run, no ReShade depth binding kept): technique DepthNormals writes the depth and the normals in one draw. Falls back to separate passes if the merged path is not allowed or fails, or if its one-off check finds the draw was dropped.
- RenderScale v2: reads are clamped to the drawn part (section 6).
- Interiors, exteriors, night: the same.

## 3. Inputs

### Textures (bound by name, not written by the effect)

- TESR_DepthBufferWorld (stage 0, POINT, CLAMP): the world depth, INTZ raw values (reversed in FNV).
- TESR_DepthBufferViewModel (stage 1, POINT, CLAMP): the first-person depth; 0 where nothing was drawn.

### Constants

- TESR_DepthConstants: x = view-model near plane (JohnnyGuitar clip distance when above 0, at least 0.3; else the camera near plane); z = 1 when the depth is reversed, else 0.
- TESR_CameraData: x = world near plane, y = far plane.
- TESR_ReciprocalResolution: 1 / the full frame size (the rect clamp's texel, and the merged path's neighbour step).
- TESR_InvProjectionTransform: the merged path's view-space positions.
- TESR_ProjectionTransform: declared, not read.
- TESR_RenderScale (RenderRect include): the drawn fraction, for the read clamp.

## 4. Settings

Unchanged:
- [Main.Main.Performance] SlimDepthBuffer (bool, true; UNOFFICIAL): R32F target (default) instead of G32R32F. Read at target creation; restart to change.
- [Main.Main.Performance] MergedDepthNormals (bool, false; UNOFFICIAL, experimental): the merged draw (the C++ key is DisableMergedNormals, its inverse).
- [Main.Main.Performance] NvrDxvkPasses (bool, true): the Vulkan passes.

## 5. Passes and outputs

### Separate pass (technique CombineDepth, index 0, one pass, PS CombineDepthPS)

Target: the full-frame depth target. Format R32F (SlimDepthBuffer) or G32R32F.

Output, per pixel (what every reader depends on):
- R = viewZ / farZ: viewZ is the view-space distance along the camera's forward axis in game units. Readers multiply by far to get game units.
- G (only with G32R32F; always written) = the reversed-Z post-projection depth from viewZ with the world near plane: near x (far - viewZ) / (viewZ x (far - near)). Readers recompute it from R, so G is redundant.
- B = 1, A = 1.

Conversion of a raw depth d to viewZ, with near and far (the view model uses its own near plane):
- Reversed (TESR_DepthConstants.z > 0.5, FNV): viewZ = near x far / (near + d x (far - near)). d = 1 gives near, d = 0 gives far.
- Standard (z = 0): viewZ = near x far / (far - d x (far - near)). d = 0 gives near, d = 1 gives far.
- The world depth is used unless the view-model depth is above 0; then the view-model depth is converted with the view-model near plane (no blending: the weapon's edge is a hard edge).
- Sky and nothing drawn: d at the clear value, so viewZ = far and R = 1.

### Merged pass (technique DepthNormals, one pass, PS DepthNormalsPS, two targets)

- COLOR0 = the depth target, same encoding as above.
- COLOR1 = the normals target, same encoding as the Normals spec (n x 0.5 + 0.5, view space, +z away from the camera, the Normals spec's neighbour rule).
- The neighbour depths are recomputed from the two raw depth textures (CombinedViewZ), at one and two texels to each side, clamped to the drawn part.

## 6. Behaviour details

- Reversed depth: the flag (TESR_DepthConstants.z) now selects the conversion (section 5). With FNV's reversed buffer the output is the same as before; with a non-reversed buffer the output is now correct instead of wrong.
- Guards: the viewZ conversion is floored at 1e-6, the view-model near plane at 1e-4 (no division by zero). A raw depth of 0 (reversed) is the far plane, R = 1.
- RenderScale v2: reads are clamped to the drawn part with the shared rect clamp (clampToRectIfOn). Output is written across the target; the viewport limits the drawn part.
- Merged normals of a degenerate neighbourhood (zero-length cross product, e.g. at a flat edge clamp) return (0, 0, -1), facing the camera. No NaN.
- Depth is point sampled; no mips.
- The merged path's one-off check, the failure flag and the ReShade rule are C++ behaviour, unchanged.

## 7. Vulkan path

- Separate pass: Compute\CombineDepth.frag (technique 0 line for line): set 1 binding 0 = world depth, binding 1 = view-model depth, POINT CLAMP heap sampler at push offset 32; push offset 0 = depth constants, offset 16 = camera data. Same encoding. No RenderScale clamp (the push block has no rect data; open, see section 9).
- Merged pass: Compute\DepthNormals.frag. The file is kept from before wave 3 (not in the removed list) and includes Compute\Normals.glsl for its maths. The new Normals.glsl does not provide the function it calls (viewNormalFromTaps), so DepthNormals.frag does not compile now. Until it is rewritten, the C++ merged Vulkan path fails and the D3D9 merged path (or separate passes) runs.

## 8. Look and cost

Correct: no visible output of its own. Smooth distances, no weapon halo in depth-based effects, sky counted as far.

Typical wrong: a dark outline or halo round the weapon in SSAO, shadows or fog (view-model near plane or convention mismatch); sky counted as near (wrong convention); flicker on edges (a depth test on a filtered texture).

Cost: one full-screen pass (separate) or one pass with two targets (merged). A few depth reads per pixel.

## 9. Open questions

1. Whether FNV can run with a non-reversed buffer is still unconfirmed (the stock game is assumed to clear to 0). The shader now handles both, so the question no longer changes the output.
2. Whether any reader uses the G channel directly (the shared depth helper reads R only).
3. The view-model depth at a different size or filter (RenderScale, DLSS) is not checked.
4. The JohnnyGuitar clip distance when absent is assumed to be 0.
5. Vulkan separate pass has no RenderScale clamp; add it if rect frames show stale depth on the Vulkan path.
6. DepthNormals.frag (kept file) must be rewritten against the new Normals.glsl, or the Normals.glsl function it calls restored. Until then the batch compile-compute.sh run stops at it.
7. SlimDepthBuffer's restart requirement is as the previous spec wrote it; not re-verified in the settings loader.
