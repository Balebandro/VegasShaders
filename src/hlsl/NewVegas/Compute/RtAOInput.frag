// UNOFFICIAL 2026-10-28 (NVR DXVK fork, hybrid RT item 2; docs/next/FORK-RT-AO.md) [Shaders.AmbientOcclusion.RT]: what the RT helper's
// trace pass (RtAOTrace.comp) starts its rays from, per half-resolution texel: x the view distance (0 past the AO's far end: no rays),
// y the world normal (camera-relative world, OctEncode + Pack2 as AmbientLight's prep). Into NVR_RTAOInput (G32R32F), shared with the
// helper by core/RtAmbient.h. uv is the target's texel centre. effects/AmbientOcclusion.cpp RtRaw.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientOcclusion.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_normals;    // TESR_NormalsBuffer (NONE = POINT, CLAMP)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

vec3 reconstructPosition(vec2 uv) {
    float z = projectedDepthFromLinear(LINEAR_LOD(t_depth, uv).x);
    vec4 viewSpace = cb.invProjectionTransform * vec4(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, z, 1.0);
    return viewSpace.xyz / viewSpace.w;
}

// AmbientLight.glsl's
float Pack2(vec2 v) { v = floor(clamp(v, 0.0, 1.0) * 4095.0 + 0.5); return v.x * 4096.0 + v.y; }
vec2 OctEncode(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * s;
    return e * 0.5 + 0.5;
}

void main() {
    vec3 P = reconstructPosition(uv);
    if (!(P.z < endFade) || P.z <= 0.0) { o_color = vec4(0.0); return; }
    vec3 N = POINT_LOD(t_normals, uv).xyz * 2.0 - 1.0;
    N = dot(N, -P) < 0.0 ? -N : N;
    vec3 n = normalize((cb.invViewTransform * vec4(N, 0.0)).xyz);
    o_color = vec4(P.z, Pack2(OctEncode(n)), 0.0, 0.0);
}
