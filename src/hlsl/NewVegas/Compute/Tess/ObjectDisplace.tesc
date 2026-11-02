#version 450
// UNOFFICIAL 2026-10-28 NVR DXVK tessellation displacement of objects (fork patch 0021, extension 1.7-displace; docs/next/FORK2-DISPLACE.md
// "Objects"). [Main.Main.Performance] NvrDxvkDisplacement, [Shaders.POM.Displacement]. Needs the NVR-built DXVK d3d9.dll.
//
// Tessellation control shader of the parallax objects (ParallaxTemplate.hlsl, vertex shaders PAR2000-2019.vso compiled with
// NVR_DISPLACE_OBJECTS): rocks, cliffs, cave and wall pieces with a height map. It hands the vertex shader's outputs on unchanged and
// sets how finely each triangle is split. Inputs = the vertex shader's outputs at their D3D9 semantic locations (vec4 each):
//   0 NORMAL0 the clip position (a copy of POSITION)   1 TEXCOORD0 xy uv, zw the object normal (octahedral)
//   2-8 TEXCOORD1-7   9 / 10 COLOR0 / COLOR1   (what each holds depends on the pass: lights, view vector, shadow lookups)
// The vertex shaders write 10 of those 11 (vs_3_0's limit): all but TEXCOORD2, or with SKIP_COLOR0 (the DIFFUSE lamp passes, whose
// TEXCOORD2 is their light's attenuation) all but COLOR0. This shader reads exactly that set, as DXVK's interface check requires.
//
// The split is set in screen space (EdgePixels: the pixels between the new points), from the clip positions alone, so it is the same
// for every pass of an object (lamp and highlight passes draw the same triangles again and must land on the same depth) and for both
// triangles that share an edge: an edge's level depends only on its two end points. The displacement fades out with the view depth
// between FadeStart and FadeEnd.
layout(vertices = 3) out;

#define PASS(n) layout(location = n) in vec4 i_v##n[]; layout(location = n) out vec4 o_v##n[];
#ifdef SKIP_COLOR0
PASS(0) PASS(1) PASS(2) PASS(3) PASS(4) PASS(5) PASS(6) PASS(7) PASS(8) PASS(10)
#else
PASS(0) PASS(1) PASS(2) PASS(4) PASS(5) PASS(6) PASS(7) PASS(8) PASS(9) PASS(10)
#endif
#undef PASS

// Per control point, for ObjectDisplace.tese: x the distance fade at this point, y log2 of the uv distance between the generated points
// (the patch's, the same for its three points: the height lookup's detail level before the texture's size), zw 0.
layout(location = 30) out vec4 o_nvr[];

// From offset 32: 0-31 are D3D9's shared push data (fog, alpha ref), which every stage sees.
layout(push_constant) uniform Push {
  layout(offset = 32)
  float factors;      // MaxFactor [1-64] + 128 x FixedFactor [0-64] (> 0: every edge split that many times, a test)
  float edgePixels;   // the pixels aimed for between the new points (EdgePixels)
  float halfWidth;    // half the screen's width in pixels
  float fadeStart;    // the displacement fades out from this view depth ... (FadeStart)
  float fadeEnd;      // ... to nothing here, game units; the splitting ends there too (FadeEnd)
  float halfHeight;   // half the screen's height in pixels
  float cullMargin;   // clip-space margin of the view test (a few times Strength: the most the surface can move); < 0: no culling
  float stripWindow;  // DXVK's: -1 a triangle list, 0 / 1 / 2 the index window of a strip
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

vec2 toPixels(vec4 c) {
  return c.xy / max(c.w, 1.0) * vec2(pc.halfWidth, pc.halfHeight);
}

float edgeLevel(vec4 a, vec4 b) {
  float d = 0.5 * (a.w + b.w);
  float f = fixedFactor() > 0.0 ? fixedFactor() : distance(toPixels(a), toPixels(b)) / max(pc.edgePixels, 1.0);
  f = mix(1.0, f, fadeAt(d));   // full detail up to FadeStart, back to the plain triangle by FadeEnd
  return clamp(f, 1.0, max(maxFactor(), 1.0));
}

// All three corners beyond the same side of the view (with the margin the displacement could move them back in)
bool outside(vec4 a, vec4 b, vec4 c) {
  float m = pc.cullMargin;
  if (m < 0.0) return false;
  return (a.x > a.w + m && b.x > b.w + m && c.x > c.w + m) || (a.x < -a.w - m && b.x < -b.w - m && c.x < -c.w - m)
      || (a.y > a.w + m && b.y > b.w + m && c.y > c.w + m) || (a.y < -a.w - m && b.y < -b.w - m && c.y < -c.w - m)
      || (a.w < -m && b.w < -m && c.w < -m);
}

void main() {
  uint i = gl_InvocationID;

  // Which input point is this patch's control point j (strips: the odd triangles' first two swapped, as the land's TCS)
  bool odd = pc.stripWindow >= 0.0 && ((uint(pc.stripWindow) + uint(gl_PrimitiveID)) & 1u) != 0u;
  uint s0 = odd ? 1u : 0u, s1 = odd ? 0u : 1u, s2 = 2u;
  uint si = i == 0u ? s0 : (i == 1u ? s1 : s2);

#define COPY(n) o_v##n[gl_InvocationID] = i_v##n[si];
#ifdef SKIP_COLOR0
  COPY(0) COPY(1) COPY(2) COPY(3) COPY(4) COPY(5) COPY(6) COPY(7) COPY(8) COPY(10)
#else
  COPY(0) COPY(1) COPY(2) COPY(4) COPY(5) COPY(6) COPY(7) COPY(8) COPY(9) COPY(10)
#endif
#undef COPY

  vec4 c0 = i_v0[s0], c1 = i_v0[s1], c2 = i_v0[s2];

  // Vulkan triangle domain: outer level 0 is the edge opposite point 0, 1 opposite point 1, 2 opposite point 2.
  float e0 = edgeLevel(c1, c2);
  float e1 = edgeLevel(c2, c0);
  float e2 = edgeLevel(c0, c1);
  float inner = max(max(e0, e1), e2);

  // The uv distance between generated points: each edge's uv length over its split count, the largest (one value for the patch: the
  // TES pins the displacement to 0 on the patch's edges, so the two patches of an edge never need the same detail level)
  vec2 u0 = i_v1[s0].xy, u1 = i_v1[s1].xy, u2 = i_v1[s2].xy;
  float uvStep = max(max(distance(u1, u2) / e0, distance(u2, u0) / e1), distance(u0, u1) / e2);
  o_nvr[gl_InvocationID] = vec4(fadeAt(i_v0[si].w), log2(max(uvStep, 1e-8)), 0.0, 0.0);

  if (i == 0u) {
    // A degenerate triangle (a strip's join) draws nothing in D3D9; a patch wholly off screen needs no splitting: both dropped.
    if (c0 == c1 || c1 == c2 || c2 == c0 || outside(c0, c1, c2)) {
      e0 = 0.0; e1 = 0.0; e2 = 0.0; inner = 0.0;
    }

    gl_TessLevelOuter[0] = e0;
    gl_TessLevelOuter[1] = e1;
    gl_TessLevelOuter[2] = e2;
    gl_TessLevelInner[0] = inner;
  }
}
