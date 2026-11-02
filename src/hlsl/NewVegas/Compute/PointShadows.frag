// PointShadows as a Vulkan pass (needs NVR DXVK): techniques 0 (merged), 1 (AddShadowedLamps, the outdoor batch) and 3 (LampMaskHalf)
// of Effects/PointShadows.fx.hlsl. The mode is the push block's mode (0, 1, 2 = half-resolution mask), or the compile-time
// POINT_SHADOW_MODE of the mode variants (variants.txt). Test variants (PS_TEST_*) only measure cost.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "PointShadowsCommon.glsl"

layout(set = 1, binding = 0) uniform textureCube t_cube0;
layout(set = 1, binding = 1) uniform textureCube t_cube1;
layout(set = 1, binding = 2) uniform textureCube t_cube2;
layout(set = 1, binding = 3) uniform textureCube t_cube3;
layout(set = 1, binding = 4) uniform textureCube t_cube4;
layout(set = 1, binding = 5) uniform textureCube t_cube5;
layout(set = 1, binding = 6) uniform textureCube t_cube6;
layout(set = 1, binding = 7) uniform textureCube t_cube7;
layout(set = 1, binding = 8) uniform textureCube t_cube8;
layout(set = 1, binding = 9) uniform textureCube t_cube9;
layout(set = 1, binding = 10) uniform textureCube t_cube10;
layout(set = 1, binding = 11) uniform texture2D t_depth;
layout(set = 1, binding = 12) uniform texture2D t_normals;

layout(push_constant) uniform Push {
    uint linearSampler;   // 0: LINEAR, CLAMP (depth, cube maps)
    uint pointSampler;    // 4: POINT, CLAMP (normals)
    uint mode;            // 8: 0 merged, 1 batch, 2 half-resolution mask
    uint pad;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

float depthAt(vec2 p) {
    return textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.linearSampler)), p, 0.0).r * cb.cameraData.y;
}

// The mask at one pixel: the sum of every lamp's share, saturated. batch: cube-mapped slots 0..10 only.
float maskAt(vec2 p, bool batch) {
#ifdef PS_TEST_EMPTY
    return 0.0;
#else
    float depth = depthAt(p);
    vec3 pos = cb.cameraPosition.xyz + toWorldDir(p) * depth;
    vec3 nView = texture(sampler2D(t_normals, NVR_SAMPLER(pc.pointSampler)), p).xyz * 2.0 - 1.0;
    vec3 n = (vec4(nView, 0.0) * cb.viewTransform).xyz;	// D3D mul(M, n)
    n = n * inversesqrt(max(dot(n, n), 1e-8));
    float total = 0.0;
    uint s = pc.linearSampler;
    total += cubeLampShare(t_cube0, s, 0, pos, n);
    total += cubeLampShare(t_cube1, s, 1, pos, n);
    total += cubeLampShare(t_cube2, s, 2, pos, n);
    total += cubeLampShare(t_cube3, s, 3, pos, n);
    total += cubeLampShare(t_cube4, s, 4, pos, n);
    total += cubeLampShare(t_cube5, s, 5, pos, n);
    total += cubeLampShare(t_cube6, s, 6, pos, n);
    total += cubeLampShare(t_cube7, s, 7, pos, n);
    total += cubeLampShare(t_cube8, s, 8, pos, n);
    total += cubeLampShare(t_cube9, s, 9, pos, n);
    total += cubeLampShare(t_cube10, s, 10, pos, n);
    if (batch) return clamp(total, 0.0, 1.0);
#ifndef PS_TEST_NO_LAMPS
    {
        vec4 lp = cb.shadowLightPosition[11];
        vec4 col = cb.lightColor[11];
        total += clamp(lampFactor(pos, n, lp.xyz, lp.w) * lumaOf(col.rgb) * col.w, 0.0, 1.0);
    }
    for (int i = 0; i < 12; i++) {
        vec4 lp = cb.lightPosition[i];
        if (lp.w <= 0.0) break;
        vec4 col = cb.lightColor[i + 12];
        total += clamp(lampFactor(pos, n, lp.xyz, lp.w) * lumaOf(col.rgb) * col.w, 0.0, 1.0);
    }
    total += spotShare(pos, n);
    for (int e = 0; e < 16; e++) {
        if (total >= 1.0) break;
        vec4 x = cb.extraLights[e];
        if (x.w <= 0.0) break;
        float radius = floor(x.w / 8.0);
        float weight = x.w - 8.0 * radius;
        total += clamp(lampFactor(pos, n, x.xyz, radius) * weight, 0.0, 1.0);
    }
#endif
    return clamp(total, 0.0, 1.0);
#endif
}

void main() {
#ifdef POINT_SHADOW_MODE
    const uint mode = uint(POINT_SHADOW_MODE);
#else
    const uint mode = pc.mode;
#endif
    if (mode == 2u) {
        // Half-resolution mask: the mask at the top-left full-resolution texel of each half pixel; G = that texel's stored depth.
        vec2 cell = floor(uv / (2.0 * cb.reciprocalResolution.xy) + 0.25);
        vec2 uvFull = (cell * 2.0 + 0.5) * cb.reciprocalResolution.xy;
        float v = maskAt(uvFull, false);
        float d = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.linearSampler)), uvFull, 0.0).r;
        o_color = vec4(v, d, 0.0, 1.0);
        return;
    }
    float v = maskAt(uv, mode == 1u);
    o_color = vec4(v, v, v, 1.0);
}
