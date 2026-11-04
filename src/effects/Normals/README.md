# Normals

Source of this spec: the rewritten shader (wave 3 buffers, branch claude/rw3-buffers): src/hlsl/NewVegas/Effects/Normals.fx.hlsl (techniques Normals and NormalsSmooth), src/hlsl/NewVegas/Compute/Normals.glsl (shared maths), Normals.frag and NormalsSmooth.frag. The C++ (src/effects/Normals.cpp) is unchanged. Written from the previous spec and the C++; the old shader was not read.

## 1. Purpose

Normals builds a per-pixel surface normal for the whole picture, in view space, from the CombineDepth buffer, and stores it in a full-frame texture that most Vegas Shaders effects read. The player does not see it directly. Its quality decides how smooth lit surfaces, reflections, shadows and snow look on low-poly ground and rocks. An optional smoothing step (off by default) evens out the normals of flat-shaded low-poly geometry in place, so every reader gets the same smoothed normal.

## 2. When it runs

- Every frame where Vegas Shaders' post-process runs: RenderDepthAndNormals (first step of RenderEffectsPreTonemapping), right after CombineDepth.
- It is skipped when the merged CombineDepth pass already wrote the normals (see the CombineDepth spec).
- After the normals pass, the smoothing (SmoothNormals > 0) runs in place (two passes), then the CapturedNormals merge (not specified here).
- Enabled: no settings section; on whenever loaded.
- RenderScale v2: reads are clamped to the drawn part; the target stays full size.

## 3. Inputs

### Textures

- TESR_DepthBuffer (stage 0, POINT, CLAMP): the CombineDepth output. R = viewZ / far.
- Smoothing passes only: TESR_NormalsBuffer (stage 1, POINT, CLAMP) for pass 0; TESR_NormalsSmoothTemp (stage 2, POINT, CLAMP) for pass 1. The normals pass reads only the depth, so it never reads the normals target it writes.

### Constants

- TESR_CameraData: x = near, y = far (game units).
- TESR_ReciprocalResolution (xy): 1 / the full frame size.
- TESR_InvProjectionTransform: the normals pass's view-space positions.
- TESR_ProjectionTransform: the smoothing pass's view-space positions (its x and y terms only).
- TESR_NormalsSmoothData (x): the smoothing reach in texels (0 = off; set by the C++ from SmoothNormals, 0 to 256).
- TESR_RenderScale (RenderRect include): the rect clamp.
- TESR_NormalsData: registered by the C++, not declared or read by this shader.

## 4. Settings

Unchanged:
- [Main.Main.Quality] SmoothNormals (int, 0; UNOFFICIAL, live): smoothing reach in pixels, 0 = off.
- [Main.Main.Quality] CapturedNormals (bool, true; UNOFFICIAL): a later pass may overwrite parts of the buffer (not specified here).
- [Main.Main.Performance] MergedDepthNormals (bool, false): the normals come from the merged CombineDepth draw.
- NvrDxvkPasses (bool, true): the Vulkan passes.

## 5. Passes and outputs

### Normals pass (technique Normals, one pass, PS NormalsPS)

- Target: TESR_NormalsBuffer, A16B16G16R16F, full frame.
- Output: RGB = the unit view-space normal n as n x 0.5 + 0.5 (decode: stored x 2 - 1). Alpha = 1.
- View space: +x right, +y up, +z away from the camera. A surface facing the camera has n.z < 0. The normal is not flipped toward the camera.

### Smoothing passes (technique NormalsSmooth, two passes, SmoothX and SmoothY)

- SmoothX: reads TESR_NormalsBuffer (stage 1), writes the spare TESR_NormalsSmoothTemp, along x.
- SmoothY: reads the spare (stage 2), writes TESR_NormalsBuffer, along y.
- Each pass reads one target and writes the other, so there is no read-and-write of the same texture. Alpha of the output = alpha of the input.

## 6. Behaviour details

### How the normal is built (normals pass)

For the pixel and its neighbours at one and two texels to each side:
1. Each neighbour's linear depth is read (clamped to the drawn part, so an edge uses the nearest valid neighbour), and its view-space position rebuilt through the inverse projection with the reversed-Z value of that depth.
2. Horizontal derivative: the vector from the left neighbour to the centre, or from the centre to the right neighbour. The side is the one whose two-texel extrapolation predicts the centre's depth better; left only when strictly better (ties go right).
3. Vertical derivative: from the down neighbour to the centre, or from the centre to the up neighbour; down only when strictly better (ties go up).
4. Normal = normalise(vertical derivative x horizontal derivative). Both derivatives point along +x and +y, so a surface facing the camera gives n.z < 0.
5. Guard (new): a zero-length cross product returns (0, 0, -1) instead of NaN.

### Smoothing (in place, once per frame)

- Reach = SmoothNormals texels. Six taps per pass at k x reach / 3 texels, k = -3, -2, -1, 1, 2, 3, along the pass's axis. Taps are read at clamped positions; a tap outside the drawn part counts zero.
- Each tap's weight is the product of: plane factor saturate(1 - distance / tolerance), where distance is the tap's view-space offset from the centre's tangent plane and tolerance = max(viewZ x 0.0005 x |k| x reach / 3, 0.5); and normal factor smoothstep(0.5, 0.8, dot(tap normal, centre normal)); and the on-screen factor.
- Result = normalise(centre normal + sum of weight x tap normal), re-encoded. Pixels at or beyond 0.99 x far (sky) return the input unchanged.
- The view positions for the smoothing come from the projection's x and y terms (the same in D3D9 and Vulkan), with the depth as z. Approximately the inverse-projection positions of the normals pass.

## 7. Vulkan path

- Normals.frag (technique 0 line for line), with Compute\Normals.glsl for the maths (nvrProjectedFromViewZ, nvrViewPosition, nvrViewPositionFromProjection, nvrNormal). Set 1 binding 0 = the depth. Push: 0 reciprocal (vec4), 16 camera data (vec4), 32 inverse projection (mat4), 96 sampler index.
- NormalsSmooth.frag: set 1 binding 0 = the source (normals or spare), binding 1 = the depth. Push: 0 reciprocal, 16 smoothing data (x reach), 32 camera data, 48 projection (mat4), 112 sampler index, 116 pass (0 along x, 1 along y).
- The Vulkan smoothing has no RenderScale clamp (open, see section 9).
- Normals.glsl changed: DepthNormals.frag (kept, merged CombineDepth pass) includes it and calls a function it no longer provides. See the CombineDepth spec.

## 8. Look and cost

Correct: smooth lit surfaces where the geometry is smooth; creases (wall to floor, a box edge) stay sharp; silhouettes do not smear into the background; with SmoothNormals 4 to 8 the facets of low-poly rock and ground go.

Typical wrong: facets (smoothing off, or reach too small); smeared silhouettes (the side choice failed); inverted lighting (sign); stripes on slopes (the tie rule).

Cost: one full-screen pass with nine depth reads per pixel. Smoothing: two passes of six taps (only when on).

## 9. Open questions

1. CapturedNormals: its encoding and when it overrides this output are not specified here.
2. The sign of the normal for surfaces seen from behind is not checked for each reader; most readers flip it themselves.
3. The clamped smoothing taps at the drawn part's edge are not tested on the GPU.
4. Smoothing and AmbientLight's own normal smoothing were not compared numerically.
5. TESR_NormalsData is registered and unused; not checked whether any reader needs it.
6. Vulkan smoothing has no RenderScale rect clamp.
7. Feedback hazard: resolved by the pass structure (the normals pass does not read the normals target; the smoothing passes alternate targets). The C++ still unbinds stage 1 and 2 around the smoothing draws.
