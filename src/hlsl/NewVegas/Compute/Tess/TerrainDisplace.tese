#version 450
#extension GL_EXT_nonuniform_qualifier : enable   // a runtime-sized sampler heap (the index is uniform for a draw)
// UNOFFICIAL 2026-10-27 NVR DXVK tessellation displacement (fork patch 0021, extension 1.7-displace; docs/next/FORK2-DISPLACE.md).
// [Main.Main.Performance] NvrDxvkDisplacement, [Shaders.Terrain.Displacement].
//
// Tessellation evaluation shader of the land: every generated point gets the vertex shader's outputs interpolated from its
// triangle's corners, and is moved along the land's normal by the land textures' height (their diffuse alpha, the height map the
// land's parallax reads), weighted by the layer blend as the pixel shader blends the colours. The land's pixel shader (SLS2092-2146)
// then runs unchanged on the displaced surface: real bumps with their own silhouette and depth (so fog, AO, SSR and the deferred
// shadows see them). Compiled once per layer count (TEX_COUNT 1-7: one texture per layer, exactly the ones that pixel shader reads)
// and per winding (cw keeps the game's triangle winding with Vulkan's upper-left tessellation domain; WINDING_CCW the test variant).
//
// Strength 0 and FixedFactor 1 give the game's own triangles exactly: the clip position is the vertex shader's, interpolated, plus
// the displacement through ModelViewProj.
//
// 2026-10-27 (user: black outlines where the bumpy land meets rocks, terrain blending on or off alike):
//  - The cause: the pixel shader's forward sun shadows look up projectionPosition (TEXCOORD6), which stayed the FLAT land's. Where
//    the land rises against a rock whose side slopes outward towards its foot (most rocks and mounds), the flat point under the
//    raised land lies inside the rock, so the rock shadowed it: a dark band round every rock, as wide as the rise over the rock's
//    slope (wider with more Strength). projectionPosition now rises with the land (never below the flat land: the sun's shadow
//    maps still hold the flat land, and a point under it would shadow itself, bulk round 2). Fog reads it too (a few units nearer).
//  - ObjectBlend (OBJECT_DEPTH variants, TEX_COUNT 1-6): the displacement fades to ObjectBlendKeep where the land meets anything
//    drawn before it (rocks, buildings, characters' feet, the near grass), so they meet the ground as on flat land instead of
//    sinking into it. The scene's depth as the land starts (terrain blending's copy, Terrain.cpp) is read at the pixel shader's s6,
//    BaseMap[6]'s slot, which is free below 7 layers (the TES can only read textures that pixel shader uses, s0-s7: NVR DXVK
//    1.7-displace); 7-layer land has no free slot and keeps its bumps (the plain variant).
//
// 2026-10-27 (plan step 2: the bumps cast their own sun shadows): SHADOW_CASTER variants (TerrainDisplace-t<N>-s / -sd [-ccw]) run the
// same displacement for the land drawn into the sun's shadow cascades (core/DisplaceShadows.h, Shadows/ShadowMapLand.vso/.pso). The
// vertex shader there gives the TCS the main camera's eye (so the splitting, the distance fade and the quadrant borders are the main
// view's, point for point), the shadow map's clip position at TEXCOORD6, the shadow map's ModelViewProj times the normal in the w of
// TEXCOORD3/4/5/7, and last frame's main-camera ModelViewProj in c0-c3 (ObjectBlend: last frame's depth copy, the newest there is when
// the shadow maps are drawn). It writes only TEXCOORD6, the shadow map's clip position, which the shadow pixel shader turns into depth.
// With the bumps in the shadow maps, the main view's projectionPosition follows the displaced land again, below the flat land too
// (Center > 0): the push data says so (viewParams + 256) while the land is drawn into the cascades.
//
// 2026-10-28 (plan step 3, BendNormals; Conor: can the tessellation "use normals"?): the camera's variants tilt the lighting frame by
// the bumps' slope. Until then the tangent, binormal and normal stayed the flat land's, so a bump's sun side and shaded side were lit
// alike and only the cast shadows and AO showed its shape. Each layer's height is read twice more at the same detail level, one texel
// of that level along u and along v; the blended slope goes through the same Contrast easing, Strength, weight and ObjectBlend share as
// the displacement, and into game units through this triangle's own uv-per-position map (land uv is a plane over the land's x / y, so
// every triangle of a quadrant has the same one: no seam along the game's edges). Normal = n - the slope along the surface (exact for a
// height moved along a constant normal); tangent and binormal re-orthogonalised to it, each keeping its length and w (the main vertex
// shader writes only their xyz; TerrainTemplate.hlsl). Off ([Shaders.Terrain.Displacement] BendNormals false: viewParams + 128): no extra
// lookups, the flat frame as before. The shadow casters have no lighting and keep one lookup.
//
// 2026-10-28 (BumpSize; Conor, shipped defaults: "how would this work with smooth terrain?", a dense field of spikes): the height's
// detail level followed the spacing of the generated points (12 units near the camera), and Contrast 10 stretched each texture's grain
// to full ups and downs from one point to the next. Measured offline on the landscape textures: 2-5 game units between neighbouring
// points on ordinary dirt and scrub at Strength 10, pure grain. The detail level is now never finer than BumpSize game units
// (viewParams + 512 x BumpSize; uv per game unit from this triangle's own uv map, the same in every triangle of a quadrant, rounded to
// 1/256 of a level so neighbours agree to the bit), for the camera and the shadow casters alike (the same landHeight). Contrast's
// default is 4 (in the DLL).
#ifndef TEX_COUNT
  #define TEX_COUNT 1
#endif

#ifdef WINDING_CCW
layout(triangles, fractional_odd_spacing, ccw) in;
#else
layout(triangles, fractional_odd_spacing, cw) in;
#endif

#define PASS_IN(n) layout(location = n) in vec4 i_v##n[];
PASS_IN(1) PASS_IN(2) PASS_IN(3) PASS_IN(4) PASS_IN(5) PASS_IN(6) PASS_IN(7) PASS_IN(8) PASS_IN(9) PASS_IN(10)
#undef PASS_IN
#ifdef SHADOW_CASTER
layout(location = 7) out vec4 o_v7;   // the shadow map's clip position (ShadowMapLand.pso reads only this)
#else
#define PASS_OUT(n) layout(location = n) out vec4 o_v##n;
PASS_OUT(1) PASS_OUT(2) PASS_OUT(3) PASS_OUT(4) PASS_OUT(5) PASS_OUT(6) PASS_OUT(7) PASS_OUT(8) PASS_OUT(9) PASS_OUT(10)
#undef PASS_OUT
#endif

layout(location = 30) in vec4 i_nvr[];   // TerrainDisplace.tesc: x relief weight, y log2 uv spacing, z distance fade, w object blend

out gl_PerVertex {
  vec4 gl_Position;
  float gl_ClipDistance[6];
};

layout(set = 0, binding = 0) uniform sampler s_samplers[];   // DXVK's sampler heap

// The land pixel shader's BaseMap[0..TEX_COUNT-1] (s0-s6): diffuse, height in alpha
layout(set = 1, binding = 0) uniform texture2D t_base0;
#if TEX_COUNT > 1
layout(set = 1, binding = 1) uniform texture2D t_base1;
#endif
#if TEX_COUNT > 2
layout(set = 1, binding = 2) uniform texture2D t_base2;
#endif
#if TEX_COUNT > 3
layout(set = 1, binding = 3) uniform texture2D t_base3;
#endif
#if TEX_COUNT > 4
layout(set = 1, binding = 4) uniform texture2D t_base4;
#endif
#if TEX_COUNT > 5
layout(set = 1, binding = 5) uniform texture2D t_base5;
#endif
#if TEX_COUNT > 6
layout(set = 1, binding = 6) uniform texture2D t_base6;
#endif
#if defined(OBJECT_DEPTH) && TEX_COUNT < 7
// ObjectBlend: the pixel shader's TESR_DisplaceDepth (s6), the scene's depth as the land starts (INTZ: the depth itself, 0-1)
layout(set = 1, binding = 6) uniform texture2D t_depth;
#define HAVE_OBJECT_DEPTH 1
#endif

layout(set = 2, binding = 0, std140) uniform VsConsts { vec4 c[256]; } vsc;          // the vertex shader's float constants (compacted)
layout(set = 2, binding = 1, std140) uniform ClipPlanes { vec4 plane[6]; } clipPlanes;  // D3D9 user clip planes, clip space (0: off)

// The block starts at offset 32: 0-31 are D3D9's shared push data (fog, alpha ref) that every stage sees (bulk round 2: from offset
// 0 the sampler indices were D3D9's fog data, so the heights came from random samplers).
layout(push_constant) uniform Push {
  layout(offset = 32)
  uint  samplerIndex[4];  // heap index of the pixel shader's sampler i: bits 16 * (i & 1) of [i / 2]
  uint  vsIndex;          // byte k: where ModelViewProj row k (c0-c3) sits in VsConsts (0xFF: missing)
  uint  psBits;           // bit i: layer i has a height map (the land pixel shader's LandHeight c34 / c35 > 0, read per draw)
  float strength;         // Strength: game units between the lowest and the highest point the bumps can reach
  float center;           // 16 x Contrast (whole) + 0.99 x Center: contrast() and centerValue() below
  float viewParams;       // 131072 x MapsFull (2026-10-28) + 512 x BumpSize + 256 x ShadowsHold + 128 x FlatNormals + DebugView x 16 + (LodBias + 4). BumpSize
                          // (2026-10-28): the finest bump the height lookup gives, game units (whole, 0: the point spacing alone; LodBias
                          // still adds on top). DebugView: 2 the height as grey,
                          // 3 the split count (blue 1 -> red 16) and the weight (green), 4 the uv (TEXCOORD0) as red / green, 5 the layer
                          // weights (COLOR0 xyz), 6 the normal (TEXCOORD5, bent), 7 the object blend (white: all the bumps, black: none).
                          // LodBias: added to the height lookup's detail level. ShadowsHold (2026-10-27): the sun's shadow maps hold the
                          // displaced land, so projectionPosition follows it all the way (else never below the flat land). FlatNormals
                          // (2026-10-28): BendNormals off (set = off, so an older DLL that never sets it gets the bent frame).
  float sceneScale;       // ObjectBlend: the draw's viewport as a share of the depth copy (RenderScale draws the world into its
                          // top-left part): round(x * 2048) * 4096 + round(y * 2048)
} pc;

#define SMP(i) s_samplers[(pc.samplerIndex[(i) >> 1] >> (16u * uint((i) & 1))) & 0xFFFFu]

float bumpSize() {
  return mod(floor(pc.viewParams / 512.0), 256.0);
}

// 2026-10-28 (user: ObjectBlend "does cause heavy shimmering when other objects cast shadows onto the tessellated geometry"): the sun's
// shadow maps hold the FULL bumps (+ 131072, [Shaders.Terrain.Displacement] ObjectBlendShadows off), not the ObjectBlend share: theirs
// came from last frame's depth copy (jittered, and the far cascades a few frames old), the screen's from this frame's, so near
// objects the two surfaces differed by a different amount every frame and the forward lookup flickered in and out of the land's own
// shadow. The screen then looks its shadow up no lower than the full bumps (projectionPosition below).
float mapsFull() {
  return mod(floor(pc.viewParams / 131072.0), 2.0);
}

float shadowsHold() {
  return mod(floor(pc.viewParams / 256.0), 2.0);
}

bool bendNormals() {
  return mod(floor(pc.viewParams / 128.0), 2.0) < 0.5;
}

float debugView() {
  return mod(floor(pc.viewParams / 16.0), 8.0);
}

// 2026-10-28 (user: "make lod bias accept decimal numbers"): a height read between two detail levels, blended by hand, so a fractional
// level (LodBias 0.5, BumpSize, IgnoreMipBias's share) gives the in-between size whatever mip filter the game's sampler has (a nearest
// mip filter snaps it to whole levels).
#define HEIGHT_AT(T, i, uv, lod) mix(textureLod(sampler2D(T, SMP(i)), uv, floor(lod)).a, \
  textureLod(sampler2D(T, SMP(i)), uv, floor(lod) + 1.0).a, fract(lod))

float lodBias() {
  return mod(pc.viewParams, 16.0) - 4.0;
}

// Contrast (2026-10-27): how strongly a texture's height differences are stretched (1: as the texture has them)
float contrast() {
  return floor(pc.center) / 16.0;
}

// Center: 0.5 = the bumps go up and down round the flat land; lower raises the whole ground by (0.5 - Center) x Strength
float centerValue() {
  return clamp(fract(pc.center) / 0.99, 0.0, 1.0);
}

float g_hsum = 0.0;
float g_wsum = 0.0;
vec2 g_dsum = vec2(0.0);   // BendNormals: the blended slope of the same differences, per uv unit (d/du, d/dv)

// One layer's height, blended as the pixel shader blends the colours (getTerrainHeight in Parallax.hlsl: no relief where a layer has
// no height map). The detail level follows the spacing of the generated points, so the bumps are the height map's broad shapes
// rather than texel noise.
// 2026-10-27 (user: "seems like all tessellation is doing visually is moving the ground up and down"): measured on the user's
// desert and scrub textures, the height (diffuse alpha) only varies by about +-2.5 % round a mean near 0.41, so the bumps were a
// quarter of a game unit while the whole ground rose by Strength x (0.5 - mean). Each layer's height now counts from that texture's
// own average (its smallest mip), so the land stays where it was on average, and landHeight stretches the differences by Contrast.
// 2026-10-28 BendNormals (slopes): also the layer's slope, forward differences one texel of the same detail level along u and along v
// (two more lookups; the spacing of the generated points where that level is above 0).
#define LAYER(i, T, w) \
  if ((w) > 0.0005) { \
    float r = 0.0; \
    vec2 dr = vec2(0.0); \
    if ((pc.psBits & (1u << (i))) != 0u) { \
      ivec2 size = max(textureSize(sampler2D(T, SMP(i)), 0), ivec2(1)); \
      float lod = max(lodUv + log2(float(size.x)) + lodBias(), 0.0); \
      int levels = textureQueryLevels(sampler2D(T, SMP(i))); \
      float mean = levels > 1 ? texelFetch(sampler2D(T, SMP(i)), ivec2(0), levels - 1).a : 0.5; \
      float a = HEIGHT_AT(T, i, uv, lod); \
      r = a - mean; \
      if (slopes) { \
        vec2 e = exp2(lod) / vec2(size); \
        dr = vec2(HEIGHT_AT(T, i, uv + vec2(e.x, 0.0), lod) - a, \
                  HEIGHT_AT(T, i, uv + vec2(0.0, e.y), lod) - a) / e; \
      } \
    } \
    g_hsum += (w) * r; \
    g_dsum += (w) * dr; \
    g_wsum += (w); \
  }

// dh: with slopes, the returned height's slope per uv unit (d/du, d/dv); else 0
float landHeight(vec2 uv, vec4 b0, vec4 b1, float lodUv, bool slopes, out vec2 dh) {
  LAYER(0, t_base0, b0.x)
#if TEX_COUNT > 1
  LAYER(1, t_base1, b0.y)
#endif
#if TEX_COUNT > 2
  LAYER(2, t_base2, b0.z)
#endif
#if TEX_COUNT > 3
  LAYER(3, t_base3, b0.w)
#endif
#if TEX_COUNT > 4
  LAYER(4, t_base4, b1.x)
#endif
#if TEX_COUNT > 5
  LAYER(5, t_base5, b1.y)
#endif
#if TEX_COUNT > 6
  LAYER(6, t_base6, b1.z)
#endif
  // The blended difference from the textures' averages, stretched by Contrast and eased into the Strength range (-0.5 .. 0.5 round
  // the 0.5 that main() takes as the flat land): low-contrast ground gets visible bumps, high-contrast ground rounds off instead of
  // overshooting. Contrast 1 is the texture's own height for small differences.
  float x = 2.0 * contrast() * (g_wsum > 0.0 ? g_hsum / g_wsum : 0.0);
  float s = inversesqrt(1.0 + x * x);
  // The easing's slope, 0.5 / (1 + x^2)^1.5, times the blended slope through the same stretch
  dh = 0.5 * s * s * s * 2.0 * contrast() * (g_wsum > 0.0 ? g_dsum / g_wsum : vec2(0.0));
  return 0.5 + 0.5 * x * s;
}

#ifdef HAVE_OBJECT_DEPTH
// ObjectBlend: the share of the displacement kept at the flat land point p (object space; n its normal, t a tangent): 1 far from
// anything drawn before the land, Keep where something touches it, a smooth fade over Width game units between. Nine points of the
// flat land round p (p itself, four at Width / 2, four at Width) are projected with ModelViewProj; at each, the scene's depth gives
// how far in front of (or behind) that land point the first surface along its view ray is, and the nearest of
// sqrt(offset^2 + gap^2) is the distance to it. Linear filtering and the clamp to the screen keep the result continuous in p, so the
// two patches that share an edge agree on it (no cracks).
const vec2 kBlendTaps[9] = vec2[9](vec2(0.0), vec2(0.5, 0.0), vec2(-0.5, 0.0), vec2(0.0, 0.5), vec2(0.0, -0.5),
  vec2(0.70711, 0.70711), vec2(-0.70711, 0.70711), vec2(0.70711, -0.70711), vec2(-0.70711, -0.70711));

float objectBlend(vec3 p, vec3 n, vec3 tangent, float width, float keep) {
  vec4 r0 = vsc.c[pc.vsIndex & 0xFFu], r1 = vsc.c[(pc.vsIndex >> 8u) & 0xFFu];
  vec4 r2 = vsc.c[(pc.vsIndex >> 16u) & 0xFFu], r3 = vsc.c[pc.vsIndex >> 24u];
  // Depth = A + B / view depth: for any perspective camera ModelViewProj's row 2 is A x row 3 + (0, 0, 0, B), whatever the object's
  // place (row 3 gives the view depth, clip w). Holds for reversed depth too.
  float A = dot(r2.xyz, r3.xyz) / max(dot(r3.xyz, r3.xyz), 1e-20);
  float B = r2.w - A * r3.w;
  float qx = floor(pc.sceneScale / 4096.0);
  vec2 scale = vec2(qx, pc.sceneScale - qx * 4096.0) / 2048.0;

  vec3 t = tangent - n * dot(tangent, n);
  t = dot(t, t) > 1e-12 ? normalize(t) : vec3(1.0, 0.0, 0.0);
  vec3 bt = cross(n, t);

  float nearest = 1e9;
  for (int k = 0; k < 9; k++) {
    vec2 o = kBlendTaps[k] * width;
    vec3 x = p + t * o.x + bt * o.y;
    vec4 c = vec4(dot(r0.xyz, x) + r0.w, dot(r1.xyz, x) + r1.w, dot(r2.xyz, x) + r2.w, dot(r3.xyz, x) + r3.w);
    if (c.w < 1.0)
      continue;   // at or behind the camera
    vec2 uv = clamp(c.xy / c.w * vec2(0.5, -0.5) + 0.5, 0.0, 1.0) * scale;
    float den = textureLod(sampler2D(t_depth, SMP(6)), uv, 0.0).r - A;
    if (abs(den) < 1e-12)
      continue;   // an infinitely far plane: nothing there
    float sceneW = B / den;
    if (!(sceneW > 0.0))
      continue;
    float gap = c.w - sceneW;   // > 0: the surface is in front of this land point, < 0: behind it (a rock's buried foot)
    nearest = min(nearest, sqrt(dot(o, o) + gap * gap));
  }
  return mix(keep, 1.0, smoothstep(0.0, 1.0, nearest / max(width, 1.0)));
}
#endif

void main() {
  vec3 b = gl_TessCoord;

#define INTERP(n) vec4 v##n = b.x * i_v##n[0] + b.y * i_v##n[1] + b.z * i_v##n[2];
  INTERP(1) INTERP(2) INTERP(3) INTERP(4) INTERP(5) INTERP(6) INTERP(7) INTERP(8) INTERP(9) INTERP(10)
#undef INTERP
  vec4 nvr = b.x * i_nvr[0] + b.y * i_nvr[1] + b.z * i_nvr[2];

  float weight = nvr.x;
  float nl = length(v6.xyz);
  vec3 n = nl > 1e-6 ? v6.xyz / nl : vec3(0.0, 0.0, 1.0);

  bool haveMvp = (pc.vsIndex & 0xFFu) != 0xFFu && ((pc.vsIndex >> 8u) & 0xFFu) != 0xFFu
              && ((pc.vsIndex >> 16u) & 0xFFu) != 0xFFu && (pc.vsIndex >> 24u) != 0xFFu;

  // ObjectBlend: the share of the displacement kept here (1: all of it). i_nvr[0].w is the same for every point of the draw.
  float keepShare = 1.0;
#ifdef HAVE_OBJECT_DEPTH
  float blend = i_nvr[0].w;
  if (haveMvp && blend >= 1.0 && nvr.z > 0.0 && pc.strength != 0.0)
    keepShare = objectBlend(v3.xyz, n, v4.xyz, floor(blend), clamp(fract(blend) / 0.99, 0.0, 1.0));
#endif

  // Displacement = Strength x (relief + offset): the relief (height - 0.5) fades with distance and to 0 on quadrant borders (each
  // quadrant is its own draw with its own textures); the offset (0.5 - Center, how far the average height rises) only fades with
  // distance, so a border keeps the same average level as the land on both sides of it (no groove along it). Both fade near objects
  // (ObjectBlend), so there the land is the flat land again.
#ifdef SHADOW_CASTER
  // The shadow map's clip position needs no ModelViewProj from the constants (it comes in TEXCOORD6 and the w of TEXCOORD3/4/5/7);
  // c0-c3 (last frame's main camera) only serve ObjectBlend above.
  const bool canDisplace = true;
#else
  const bool canDisplace = haveMvp;
#endif
#ifdef SHADOW_CASTER
  const bool slopes = false;   // no lighting there
#else
  bool slopes = bendNormals();
#endif
  // This triangle's uv over the land's x / y (uv = J (x, y) + c), from its corners (the shadow casters' vertex shader has the same
  // uv and position slots). Land uv is a plane over x / y, so J is the same for every triangle of a quadrant whatever their slope.
  vec2 e1 = i_v3[1].xy - i_v3[0].xy, e2 = i_v3[2].xy - i_v3[0].xy;
  bool haveJ = abs(e1.x * e2.y - e1.y * e2.x) > 1e-3;
  mat2 J = haveJ ? mat2(i_v1[1].xy - i_v1[0].xy, i_v1[2].xy - i_v1[0].xy) * inverse(mat2(e1, e2)) : mat2(0.0);

  // The height's detail level (log2 of a texel of it in uv): the spacing of the generated points (the TCS), but never finer than
  // BumpSize game units. Rounded to 1/256 of a level, so two triangles whose J differs in the last bit still read the same height
  // along their shared edge.
  float lodUv = nvr.y;
  float uvPerUnit = sqrt(abs(determinant(J)));
  if (bumpSize() > 0.0 && uvPerUnit > 0.0)
    lodUv = max(lodUv, floor(log2(bumpSize() * uvPerUnit) * 256.0 + 0.5) / 256.0);

  float h = 0.5;
  vec2 dh = vec2(0.0);
  if (canDisplace && weight > 0.0 && pc.strength != 0.0 && (keepShare > 0.0 || mapsFull() > 0.5))
    h = landHeight(v1.xy, v9, v10, lodUv, slopes, dh);
  float dispFull = canDisplace ? pc.strength * (weight * (h - 0.5) + nvr.z * (0.5 - centerValue())) : 0.0;
  float disp = keepShare * dispFull;

  // The vertex shader's clip position (interpolated), moved by the displacement through ModelViewProj (row k = c[k] . (v, 1))
  vec4 clipN = vec4(0.0);
#ifdef SHADOW_CASTER
  // The shadow map's ModelViewProj times each corner's normal, interpolated like the normal itself (linear: exact for the normal
  // before it is normalized, hence the same division by its length as n above).
  clipN = vec4(v4.w, v5.w, v6.w, v8.w) / max(nl, 1e-6);
#else
  if (haveMvp) {
    clipN.x = dot(vsc.c[pc.vsIndex & 0xFFu].xyz, n);
    clipN.y = dot(vsc.c[(pc.vsIndex >> 8u) & 0xFFu].xyz, n);
    clipN.z = dot(vsc.c[(pc.vsIndex >> 16u) & 0xFFu].xyz, n);
    clipN.w = dot(vsc.c[pc.vsIndex >> 24u].xyz, n);
  }
#endif
  precise vec4 clip = v7 + disp * clipN;
#ifdef SHADOW_CASTER
  clip.z = max(clip.z, 0.0);   // pancaking, as ShadowMap.vso: casters nearer than the map's near plane are kept, at its depth
#endif

  gl_Position = clip;
  for (int k = 0; k < 6; k++)
    gl_ClipDistance[k] = dot(clip, clipPlanes.plane[k]);

#ifdef SHADOW_CASTER
  o_v7 = clip;
#else
  o_v1 = v1;
  o_v2 = v2;
  o_v3 = vec4(v3.xyz + n * disp, clip.w);   // the object-space position and the view depth, as the vertex shader writes them

  // BendNormals (see the top): the lighting frame of the displaced surface. The slope of the weight and of ObjectBlend's share (the
  // fades) is left out: it is small but differs from one game triangle to the next, and would show their edges in the light.
  vec3 nb = n;
  vec4 t4 = v4, b5 = v5;
  if (dh != vec2(0.0)) {
    // Through J (above: no seams between the game's triangles, and the tangent's sign convention does not matter)
    if (haveJ) {
      // The displacement's gradient over x / y (game units per game unit), then along the surface: n - that is the normal of the
      // surface moved along n
      vec3 g = vec3(pc.strength * keepShare * weight * (dh * J), 0.0);
      g -= n * dot(n, g);
      nb = normalize(n - g);
      vec3 t = v4.xyz - nb * dot(v4.xyz, nb);
      vec3 bt = v5.xyz - nb * dot(v5.xyz, nb);
      if (dot(t, t) > 1e-12) t4.xyz = t * (length(v4.xyz) * inversesqrt(dot(t, t)));
      if (dot(bt, bt) > 1e-12) b5.xyz = bt * (length(v5.xyz) * inversesqrt(dot(bt, bt)));
    }
  }
  o_v4 = t4;
  o_v5 = b5;
  o_v6 = nl > 1e-6 ? vec4(nb * nl, v6.w) : v6;
  // projectionPosition (the pixel shader's forward sun shadows and fog). While the sun's shadow maps hold the displaced land
  // (ShadowsHold: [Shaders.Terrain.Displacement] CastShadows) it is the displaced position itself, below the flat land too.
  // Otherwise it rises with the land but never sinks below the flat land: those maps hold the flat land, so a point under it would
  // shadow itself (bulk round 2: land sunk below the flat land went dark). Until 2026-10-27 it stayed the flat land's everywhere, and
  // the flat point under land raised against a rock lay inside the rock: the black outlines round rocks (see the top). The rasterized
  // position, the depth and the object position (lighting, parallax) follow the bumps.
  // 2026-10-28 mapsFull (see mapsFull): the maps hold the full bumps, so the lookup is never below them (above them is lit land).
  o_v7 = shadowsHold() > 0.5 ? (mapsFull() > 0.5 ? v7 + max(disp, dispFull) * clipN : clip) : v7 + max(disp, 0.0) * clipN;
  o_v8 = v8;
  o_v9 = v9;
  o_v10 = v10;

  // Debug views (they replace the vertex colour, which the land multiplies into its colour). 4-6 prove that the pixel shader's
  // inputs arrive through the tessellation, interpolated: smooth gradients that follow the land, no black, no blocks.
  float dv = debugView();
  if (dv > 1.5 && dv < 2.5) {
    o_v2 = vec4(vec3(h), 1.0);
  } else if (dv > 2.5 && dv < 3.5) {
    float t = clamp((gl_TessLevelInner[0] - 1.0) / 15.0, 0.0, 1.0);
    o_v2 = vec4(t, weight, 1.0 - t, 1.0);
  } else if (dv > 3.5 && dv < 4.5) {
    o_v2 = vec4(fract(v1.xy), 0.0, 1.0);
  } else if (dv > 4.5 && dv < 5.5) {
    o_v2 = vec4(clamp(v9.xyz, 0.0, 1.0), 1.0);
  } else if (dv > 5.5 && dv < 6.5) {
    o_v2 = vec4(nb * 0.5 + 0.5, 1.0);   // the bent normal (BendNormals; the flat one when off)
  } else if (dv > 6.5) {
    o_v2 = vec4(vec3(keepShare), 1.0);   // white: all the bumps; black: none (ObjectBlend); 7-layer land always white
  }
#endif
}