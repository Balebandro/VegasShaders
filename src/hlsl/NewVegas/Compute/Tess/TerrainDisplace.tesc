#version 450
// UNOFFICIAL 2026-10-27 NVR DXVK tessellation displacement (fork patch 0021, extension 1.7-displace; docs/next/FORK2-DISPLACE.md).
// [Main.Main.Performance] NvrDxvkDisplacement, [Shaders.Terrain.Displacement]. Needs the NVR-built DXVK d3d9.dll; nothing loads this
// on native D3D9 or stock DXVK.
//
// Tessellation control shader of the land (TerrainTemplate.hlsl, vertex shader SLS2100.vso): it hands the vertex shader's outputs
// on unchanged and sets how finely each land triangle is split. DXVK runs it between the game's vertex shader (DXVK's conversion of
// SLS2100.vso) and TerrainDisplace.tese for the land draws only.
//
// Inputs = the vertex shader's outputs at their D3D9 semantic locations (vec4 each):
//   1 uv (TEXCOORD0)   2 vertex colour   3 position, object space (w: clip w)   4 tangent   5 binormal   6 normal, object space
//   7 clip position (TEXCOORD6)   8 the eye, object space (TEXCOORD7)   9 / 10 the layer blend weights (COLOR0 / COLOR1)
//
// Crack-free by construction: an edge's level depends only on its two end points and the eye (the same in both triangles that share
// it), and the per-point weight (location 30) depends only on that point.
//
// 2026-10-27 (bulk test: the land comes as triangle STRIPS): DXVK draws a strip as three patch lists over its index windows from the
// first, second and third index (stripWindow 0, 1, 2; -1 = a real triangle list). Patch p of window k is strip triangle k + 3p; an odd
// strip triangle has the reverse winding, so its first two control points are swapped here (the TES and the culling then see the
// order D3D9 draws it in). Degenerate triangles (strip joins: two equal corners) are dropped (tessellation level 0).
layout(vertices = 3) out;

#define PASS(n) layout(location = n) in vec4 i_v##n[]; layout(location = n) out vec4 o_v##n[];
PASS(1) PASS(2) PASS(3) PASS(4) PASS(5) PASS(6) PASS(7) PASS(8) PASS(9) PASS(10)
#undef PASS

// Per control point, for TerrainDisplace.tese: x the relief weight (distance fade x quadrant border), y log2 of the spacing between
// generated points in uv units (the height lookup's detail level before the texture's size), z the distance fade alone (for the
// constant part of the displacement, Center: the same on both sides of a quadrant border, so it needs no border rule), w the object
// blend as NVR sends it (objectBlend below: the same for every point of a draw; the TES has no push room left for it).
layout(location = 30) out vec4 o_nvr[];

// The block starts at offset 32: 0-31 are D3D9's shared push data (fog, alpha ref) that every stage sees (bulk round 2: from offset
// 0 the first four values were D3D9's fog data, so nothing was split).
layout(push_constant) uniform Push {
  layout(offset = 32)
  float factors;      // MaxFactor [1-64] + 128 x FixedFactor [0-64] (> 0: every edge split that many times, a test)
  float edgeUnits;    // the spacing aimed for next to the camera, game units (EdgeLength)
  float edgeGrowth;   // spacing added per game unit of distance (EdgeGrowth)
  float fadeStart;    // the displacement fades out from here ... (FadeStart)
  float fadeEnd;      // ... to nothing here, game units from the camera; the splitting ends there too (FadeEnd)
  float borderGrid;   // land quadrant size, 2048: no displacement on quadrant borders (each quadrant is its own draw); 0 off
  float objectBlend;  // 2026-10-27 ObjectBlend: floor = ObjectBlendWidth (game units), fraction / 0.99 = ObjectBlendKeep; 0 = off
                      // (the setting, or a draw outside the world scene, whose depth copy is not this view's)
  float stripWindow;  // DXVK's: -1 a triangle list, 0 / 1 / 2 the index window of a strip (see above)
} pc;

float maxFactor() {
  return mod(pc.factors, 128.0);
}

float fixedFactor() {
  return floor(pc.factors / 128.0);
}

float fadeAt(float d) {
  return 1.0 - smoothstep(pc.fadeStart, max(pc.fadeEnd, pc.fadeStart + 1.0), d);
}

float spacingAt(float d) {
  return max(pc.edgeUnits + pc.edgeGrowth * d, 0.25);
}

float edgeLevel(vec3 a, vec3 b, vec3 eye) {
  float len = distance(a, b);
  float d = distance((a + b) * 0.5, eye);
  float f = fixedFactor() > 0.0 ? fixedFactor() : len / spacingAt(d);
  f = mix(1.0, f, fadeAt(d));   // full detail up to FadeStart, back to the plain triangle by FadeEnd
  return clamp(f, 1.0, max(maxFactor(), 1.0));
}

void main() {
  uint i = gl_InvocationID;

  // Which input point is this patch's control point j (strips: the odd triangles' first two swapped)
  bool odd = pc.stripWindow >= 0.0 && ((uint(pc.stripWindow) + uint(gl_PrimitiveID)) & 1u) != 0u;
  uint s0 = odd ? 1u : 0u, s1 = odd ? 0u : 1u, s2 = 2u;
  uint si = i == 0u ? s0 : (i == 1u ? s1 : s2);

#define COPY(n) o_v##n[gl_InvocationID] = i_v##n[si];
  COPY(1) COPY(2) COPY(3) COPY(4) COPY(5) COPY(6) COPY(7) COPY(8) COPY(9) COPY(10)
#undef COPY

  vec3 p0 = i_v3[s0].xyz, p1 = i_v3[s1].xyz, p2 = i_v3[s2].xyz;
  vec3 eye = i_v8[0].xyz;   // EyePosition: one value for the whole draw

  // This point's weight: the distance fade, and none on a land quadrant's border (its neighbour is another draw with other
  // textures: its height there would differ, the edge would crack).
  vec3 p = i_v3[si].xyz;
  float d = distance(p, eye);
  float fade = fadeAt(d);
  float weight = fade;
  if (pc.borderGrid > 0.0) {
    vec2 m = abs(fract(p.xy / pc.borderGrid + 0.5) - 0.5) * pc.borderGrid;
    if (min(m.x, m.y) < 0.5)
      weight = 0.0;
  }

  // Spacing of the generated points in uv units: the aimed-for spacing, or the edge length over MaxFactor where that is coarser
  // (land uv scale is the same for every triangle, so shared points get the same value from either triangle).
  vec2 u0 = i_v1[s0].xy, u1 = i_v1[s1].xy, u2 = i_v1[s2].xy;
  float l01 = distance(p0, p1), l12 = distance(p1, p2), l20 = distance(p2, p0);
  float uvPerUnit = max(max(distance(u0, u1) / max(l01, 1e-3), distance(u1, u2) / max(l12, 1e-3)), distance(u2, u0) / max(l20, 1e-3));
  float spacing = max(spacingAt(d), max(max(l01, l12), l20) / max(maxFactor(), 1.0));
  o_nvr[gl_InvocationID] = vec4(weight, log2(max(spacing * uvPerUnit, 1e-8)), fade, pc.objectBlend);

  if (i == 0u) {
    // Vulkan triangle domain: outer level 0 is the edge opposite point 0, 1 opposite point 1, 2 opposite point 2.
    float e0 = edgeLevel(p1, p2, eye);
    float e1 = edgeLevel(p2, p0, eye);
    float e2 = edgeLevel(p0, p1, eye);
    float inner = max(max(e0, e1), e2);

    // A degenerate triangle (a strip's join: two corners the same) draws nothing in D3D9: dropped here, as displaced its
    // generated points would no longer lie on one line.
    if (p0 == p1 || p1 == p2 || p2 == p0) {
      e0 = 0.0; e1 = 0.0; e2 = 0.0; inner = 0.0;
    }

    gl_TessLevelOuter[0] = e0;
    gl_TessLevelOuter[1] = e1;
    gl_TessLevelOuter[2] = e2;
    gl_TessLevelInner[0] = inner;
  }
}