#version 450
#extension GL_EXT_nonuniform_qualifier : enable   // a runtime-sized sampler heap (the index is uniform for a draw)
// UNOFFICIAL 2026-10-28 NVR DXVK tessellation displacement of objects (fork patch 0021, extension 1.7-displace; docs/next/FORK2-DISPLACE.md
// "Objects"). [Main.Main.Performance] NvrDxvkDisplacement, [Shaders.POM.Displacement].
//
// Tessellation evaluation shader of the parallax objects (ParallaxTemplate.hlsl compiled with NVR_DISPLACE_OBJECTS): every generated point
// gets the vertex shader's outputs interpolated from its triangle's corners and is moved along the object's normal by the object's own
// height map (the one its parallax reads: HeightMap, s3 in the main pass, s2 in the lamp, highlight and texture-only passes; HEIGHT_SLOT).
// The pixel shader then runs unchanged on the moved surface. Compiled per height slot, per vertex output set (SKIP_COLOR0: the DIFFUSE lamp
// passes, see ObjectDisplace.tesc) and per winding (WINDING_CCW the test variant).
//
// Seams (2026-10-28): a mesh's vertices are split where its uv or its normals jump (uv seams, hard edges); the two triangles on either side
// of such an edge then read different heights or move along different normals, and a crack would open along the edge. A shader only sees
// its own triangle, so it cannot tell such an edge from a smooth one. The displacement is therefore 0 on every edge of the game's own
// triangles and fades in over EdgeFade generated steps from them: no edge ever moves, so no crack can open anywhere, whatever the mesh's
// seams. The bumps sit inside the game's triangles (rock meshes have big ones). EdgeFade 0 = no pinning (test only: cracks at seams).
//
// Every pass of an object lands on the same depth: the same corners, the same split (screen space, from the clip positions only), the same
// height (texelFetch, so the passes' sampler states do not matter) and a precise, invariant position.
// The lighting vectors (light directions, the view vector, the shadow lookups) are interpolated, not moved: the bumps are a few game units
// and the lights hundreds away; the forward sun shadow lookup stays at the flat surface on purpose (the sun's shadow maps hold the flat
// object, so a point below it would shadow itself).
#ifndef HEIGHT_SLOT
  #define HEIGHT_SLOT 3
#endif

#ifdef WINDING_CCW
layout(triangles, fractional_odd_spacing, ccw) in;
#else
layout(triangles, fractional_odd_spacing, cw) in;
#endif

#define PASS(n) layout(location = n) in vec4 i_v##n[]; layout(location = n) out vec4 o_v##n;
#ifdef SKIP_COLOR0
PASS(0) PASS(1) PASS(2) PASS(3) PASS(4) PASS(5) PASS(6) PASS(7) PASS(8) PASS(10)
#else
PASS(0) PASS(1) PASS(2) PASS(4) PASS(5) PASS(6) PASS(7) PASS(8) PASS(9) PASS(10)
#endif
#undef PASS

layout(location = 30) in vec4 i_nvr[];   // ObjectDisplace.tesc: x distance fade, y log2 uv step (the patch's)

out gl_PerVertex {
  vec4 gl_Position;
  float gl_ClipDistance[6];
};
invariant gl_Position;

layout(set = 0, binding = 0) uniform sampler s_samplers[];   // DXVK's sampler heap

// The pixel shader's HeightMap (height in red, as Parallax.hlsl reads it)
layout(set = 1, binding = HEIGHT_SLOT) uniform texture2D t_height;

layout(set = 2, binding = 0, std140) uniform VsConsts { vec4 c[256]; } vsc;          // the vertex shader's float constants (compacted)
layout(set = 2, binding = 1, std140) uniform ClipPlanes { vec4 plane[6]; } clipPlanes;  // D3D9 user clip planes, clip space (0: off)

layout(push_constant) uniform Push {
  layout(offset = 32)
  uint  samplerIndex[4];  // heap index of the pixel shader's sampler i: bits 16 * (i & 1) of [i / 2]
  uint  vsIndex;          // byte k: where ModelViewProj row k (c0-c3) sits in VsConsts (0xFF: missing)
  uint  psBits;           // (unused: every parallax object has its height map)
  float strength;         // Strength: game units between the lowest and the highest point the bumps can reach
  float center;           // 16 x Contrast (whole) + 0.99 x Center
  float lodBias;          // LodBias + 4
  float edgeFade;         // EdgeFade: generated steps over which the bumps fade in from the game's triangle edges (0: no pinning)
} pc;

#define SMP(i) s_samplers[(pc.samplerIndex[(i) >> 1] >> (16u * uint((i) & 1))) & 0xFFFFu]
#define HEIGHT_TEX sampler2D(t_height, SMP(HEIGHT_SLOT))

float contrast() {
  return floor(pc.center) / 16.0;
}

float centerValue() {
  return clamp(fract(pc.center) / 0.99, 0.0, 1.0);
}

vec3 decodeNormal(vec2 e) {   // ShadowEncodeNormal's octahedral pair back to a vector (Includes/Shadow.hlsl ShadowDecodeNormal)
  vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
  float t = clamp(-n.z, 0.0, 1.0);
  n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
  return n;
}

// Bilinear red at one mip level, wrapping (the objects' textures tile), from texelFetch: no sampler state takes part
float heightAtLevel(vec2 uv, int level) {
  ivec2 size = max(textureSize(HEIGHT_TEX, level), ivec2(1));
  vec2 p = uv * vec2(size) - 0.5;
  vec2 f = fract(p);
  vec2 base = floor(p);
  ivec2 a = ivec2(mod(base, vec2(size)));
  ivec2 b = ivec2(mod(base + 1.0, vec2(size)));
  float h00 = texelFetch(HEIGHT_TEX, ivec2(a.x, a.y), level).r;
  float h10 = texelFetch(HEIGHT_TEX, ivec2(b.x, a.y), level).r;
  float h01 = texelFetch(HEIGHT_TEX, ivec2(a.x, b.y), level).r;
  float h11 = texelFetch(HEIGHT_TEX, ivec2(b.x, b.y), level).r;
  return mix(mix(h00, h10, f.x), mix(h01, h11, f.x), f.y);
}

// The height round the texture's own average (its smallest mip), stretched by Contrast and eased into 0..1 round 0.5 (as the land's).
// The detail level follows the spacing of the generated points, so the bumps are the height map's broad shapes, not texel noise.
float objectHeight(vec2 uv, float lodUv) {
  int levels = textureQueryLevels(HEIGHT_TEX);
  float mean = levels > 1 ? texelFetch(HEIGHT_TEX, ivec2(0), levels - 1).r : 0.5;
  float lod = clamp(lodUv + log2(float(max(textureSize(HEIGHT_TEX, 0).x, 1))) + pc.lodBias - 4.0, 0.0, float(levels - 1));
  int l0 = int(floor(lod));
  int l1 = min(l0 + 1, levels - 1);
  float raw = mix(heightAtLevel(uv, l0), heightAtLevel(uv, l1), lod - float(l0));
  float x = 2.0 * contrast() * (raw - mean);
  return 0.5 + 0.5 * x * inversesqrt(1.0 + x * x);
}

void main() {
  vec3 b = gl_TessCoord;

#define INTERP(n) vec4 v##n = b.x * i_v##n[0] + b.y * i_v##n[1] + b.z * i_v##n[2];
#ifdef SKIP_COLOR0
  INTERP(1) INTERP(2) INTERP(3) INTERP(4) INTERP(5) INTERP(6) INTERP(7) INTERP(8) INTERP(10)
#else
  INTERP(1) INTERP(2) INTERP(4) INTERP(5) INTERP(6) INTERP(7) INTERP(8) INTERP(9) INTERP(10)
#endif
#undef INTERP
  precise vec4 v0 = b.x * i_v0[0] + b.y * i_v0[1] + b.z * i_v0[2];   // the clip position

  // The normal: each corner's decoded, then interpolated (an octahedral pair does not interpolate across its folds)
  vec3 n = b.x * decodeNormal(i_v1[0].zw) + b.y * decodeNormal(i_v1[1].zw) + b.z * decodeNormal(i_v1[2].zw);
  float nl = length(n);
  n = nl > 1e-6 ? n / nl : vec3(0.0, 0.0, 1.0);

  bool haveMvp = (pc.vsIndex & 0xFFu) != 0xFFu && ((pc.vsIndex >> 8u) & 0xFFu) != 0xFFu
              && ((pc.vsIndex >> 16u) & 0xFFu) != 0xFFu && (pc.vsIndex >> 24u) != 0xFFu;

  // Pinned to the game's own triangle edges (see the top): 0 on them, all of it EdgeFade generated steps in. Outer level k is the edge
  // opposite corner k, where gl_TessCoord[k] is 0.
  float pin = 1.0;
  if (pc.edgeFade > 0.0) {
    for (int k = 0; k < 3; k++)
      pin = min(pin, clamp(b[k] * gl_TessLevelOuter[k] / pc.edgeFade, 0.0, 1.0));
    pin = pin * pin * (3.0 - 2.0 * pin);
  }
  float fade = b.x * i_nvr[0].x + b.y * i_nvr[1].x + b.z * i_nvr[2].x;
  float weight = fade * pin;

  float h = centerValue();
  if (haveMvp && weight > 0.0 && pc.strength != 0.0)
    h = objectHeight(v1.xy, i_nvr[0].y);

  // ModelViewProj's rows (row k = c[k] . (v, 1)). Row 3 gives the view depth; its length is the object's scale (rotation rows are unit
  // vectors), so Strength stays in game units on scaled rocks.
  vec4 r0 = vec4(0.0), r1 = vec4(0.0), r2 = vec4(0.0), r3 = vec4(0.0);
  if (haveMvp) {
    r0 = vsc.c[pc.vsIndex & 0xFFu]; r1 = vsc.c[(pc.vsIndex >> 8u) & 0xFFu];
    r2 = vsc.c[(pc.vsIndex >> 16u) & 0xFFu]; r3 = vsc.c[pc.vsIndex >> 24u];
  }
  float scale = length(r3.xyz);
  float disp = haveMvp && scale > 1e-6 ? pc.strength * weight * (h - centerValue()) / scale : 0.0;
  precise vec4 clip = v0 + disp * vec4(dot(r0.xyz, n), dot(r1.xyz, n), dot(r2.xyz, n), dot(r3.xyz, n));

  gl_Position = clip;
  for (int k = 0; k < 6; k++)
    gl_ClipDistance[k] = dot(clip, clipPlanes.plane[k]);

  o_v0 = clip;
  o_v1 = v1;
  o_v2 = v2;
#ifdef SKIP_COLOR0
  o_v3 = v3;
#endif
  o_v4 = v4;
  o_v5 = v5;
  o_v6 = v6;
  o_v7 = v7;
  o_v8 = v8;
#ifndef SKIP_COLOR0
  o_v9 = v9;
#endif
  o_v10 = v10;
}
