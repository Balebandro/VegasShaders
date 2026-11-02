// VolumetricClouds technique Temporal (2) as a Vulkan pass (needs NVR DXVK): Effects/VolumetricClouds.fx.hlsl TemporalPS,
// SharpHistory and CatmullRom line for line, quarter resolution, into the history being written. effects/VolumetricClouds.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_current;    // TESR_CloudCurrent (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_history;    // TESR_CloudHistory (LINEAR, CLAMP)

#define CLOUDS_HISTORY
#include "VolumetricClouds.glsl"

vec4 SharpHistory(vec2 coord) {
    vec2 size = 1.0 / LowTexel;
    vec2 corner = (floor(coord * size - 0.5) + 0.5) * LowTexel;
    vec4 a = LINEAR_LOD(t_history, corner);
    vec4 b = LINEAR_LOD(t_history, corner + vec2(LowTexel.x, 0.0));
    vec4 c = LINEAR_LOD(t_history, corner + vec2(0.0, LowTexel.y));
    vec4 d = LINEAR_LOD(t_history, corner + LowTexel);
    return clamp(CatmullRom(coord), min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
}

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 coord = (vpos + 0.5) * LowTexel;
    vec2 block = floor(vpos * 0.5);
    vec2 blockUV = (block + 0.5) * cb.blocks.xy;
    vec4 current = POINT_LOD(t_current, blockUV);
    vec4 lo = current, hi = current;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec4 n = POINT_LOD(t_current, blockUV + vec2(x, y) * cb.blocks.xy);
            lo = min(lo, n); hi = max(hi, n);
        }
    }
    bool fresh = all(equal(vpos - block * 2.0, cb.blocks.zw));
    vec3 dir = normalize(toWorld(coord));
    vec3 previousDir = dir;
    if (Sharpness > 0.0) {
        vec3 camera = cb.cameraPosition.xyz * MetresPerUnit;
        float reach = ShellDistance(dir.z, max(Base + 0.4 * BaseThickness - camera.z, 50.0));
        previousDir = dir * reach + vec3(cb.motion.xy, 0.0);
    }
    vec4 clip = cb.prevViewProj * vec4(previousDir, 0.0);     // HLSL mul(float4(previousDir, 0.0), TESR_CloudsPrevViewProj)
    float weight = cb.cloudLayout.z;
    if (clip.w <= 0.0 || weight <= 0.0) { o_color = current; return; }
    vec2 previous = clip.xy / clip.w * vec2(0.5, -0.5) + 0.5;
    if (any(lessThan(previous, vec2(0.0))) || any(greaterThan(previous, vec2(1.0)))) { o_color = current; return; }
    vec4 history = clamp(Sharpness > 0.0 ? SharpHistory(previous) : LINEAR_LOD(t_history, previous), lo, hi);
    o_color = fresh ? mix(current, history, weight) : history;
}
