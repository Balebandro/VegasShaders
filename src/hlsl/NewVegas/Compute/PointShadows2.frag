// PointShadows2 as a Vulkan pass (needs NVR DXVK): the lamp light of lamps 6 to 11 added to the point-shadow buffer, as
// Effects/PointShadows2.fx.hlsl. Reads the buffer's value before the pass from a copy (binding 2), so the pass never reads the target it writes.
// Bindings (set 1): 0 depth, 1 normals, 2 the buffer copy, 3 to 7 the cube maps of slots 6 to 10. Set 2 binding 0: the 752-byte block below.
// Push: linearSampler (offset 0, LINEAR CLAMP: depth, copy, cube maps), pointSampler (offset 4, POINT CLAMP: normals).
// Matrices: a GLSL mat4 element [c][r] is the D3D element [c][r]; the D3D mul(v, M) is M * v, and mul(M, v) is v * M.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;
layout(set = 1, binding = 1) uniform texture2D t_normals;
layout(set = 1, binding = 2) uniform texture2D t_copy;
layout(set = 1, binding = 3) uniform textureCube t_cube6;
layout(set = 1, binding = 4) uniform textureCube t_cube7;
layout(set = 1, binding = 5) uniform textureCube t_cube8;
layout(set = 1, binding = 6) uniform textureCube t_cube9;
layout(set = 1, binding = 7) uniform textureCube t_cube10;

layout(set = 2, binding = 0, std140) uniform PointShadows2Block {
    vec4 cameraPosition;              // 0
    vec4 cameraData;                  // 16: y far distance
    mat4 viewTransform;               // 32
    mat4 projectionTransform;         // 96
    vec4 shadowFade;                  // 160: z point shadows on
    vec4 shadowLightPosition[12];     // 176: xyz position, w radius
    vec4 lightColor[24];              // 368: rgb colour, w dimmer
} cb;

layout(push_constant) uniform Push {
    uint linearSampler;   // 0
    uint pointSampler;    // 4
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Depth.hlsl toWorld(): camera-relative world direction through this pixel (view depth 1 along forward).
vec3 toWorldDir(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

float lumaOfLampColour(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// The falloff and the facing term of a lamp (not normalised normal, as the .fx keeps it); unshadowed.
float lampFalloffFacing(vec3 toLamp, float len, float d, vec3 n) {
    float s = clamp(d * d, 0.0, 1.0);
    float falloff = (1.0 - s) * (1.0 - s) / (1.0 + 5.0 * s);
    float facing = dot(toLamp / max(len, 1e-6), n);
    facing = mix(1.0, facing, smoothstep(0.0, 0.2, d));
    return clamp(clamp(facing, 0.0, 1.0) * falloff, 0.0, 1.0);
}

// One cube-mapped lamp's share: amount (shadow test x falloff x facing) times luma x dimmer. An empty slot (w 0) or a NaN gives 0.
float shadowedLampShare(textureCube cube, vec4 lp, vec4 col, vec3 pos, vec3 n) {
    if (!(lp.w > 0.0)) return 0.0;
    vec3 toLamp = lp.xyz - pos;
    float len = length(toLamp);
    float d = len / lp.w;
    float lit = 1.0;
    if (cb.shadowFade.z > 0.5) {
        vec3 dir = vec3(-toLamp.x, -toLamp.y, toLamp.z);
        float stored = textureLod(samplerCube(cube, NVR_SAMPLER(pc.linearSampler)), dir, 0.0).r;
        if (stored > 0.0 && stored < 1.0) lit = (stored + 0.018 * d > d) ? 1.0 : 0.0;
    }
    float amount = clamp(lampFalloffFacing(toLamp, len, d, n) * lit, 0.0, 1.0);
    return amount * lumaOfLampColour(col.rgb) * col.w;
}

// Lamp 11: no shadow test and no colour weight.
float unshadowedLampShare(vec4 lp, vec3 pos, vec3 n) {
    if (!(lp.w > 0.0)) return 0.0;
    vec3 toLamp = lp.xyz - pos;
    float len = length(toLamp);
    return lampFalloffFacing(toLamp, len, len / lp.w, n);
}

void main() {
    float depth = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.linearSampler)), uv, 0.0).r * cb.cameraData.y;
    vec3 pos = cb.cameraPosition.xyz + toWorldDir(uv) * depth;
    vec3 nView = textureLod(sampler2D(t_normals, NVR_SAMPLER(pc.pointSampler)), uv, 0.0).xyz * 2.0 - 1.0;
    vec3 n = (vec4(nView, 1.0) * cb.viewTransform).xyz;	// D3D mul(M, n) with w = 1, as the .fx's normal helper

    float total = 0.0;
    total += shadowedLampShare(t_cube6, cb.shadowLightPosition[6], cb.lightColor[6], pos, n);
    total += shadowedLampShare(t_cube7, cb.shadowLightPosition[7], cb.lightColor[7], pos, n);
    total += shadowedLampShare(t_cube8, cb.shadowLightPosition[8], cb.lightColor[8], pos, n);
    total += shadowedLampShare(t_cube9, cb.shadowLightPosition[9], cb.lightColor[9], pos, n);
    total += shadowedLampShare(t_cube10, cb.shadowLightPosition[10], cb.lightColor[10], pos, n);
    total += unshadowedLampShare(cb.shadowLightPosition[11], pos, n);

    float prev = textureLod(sampler2D(t_copy, NVR_SAMPLER(pc.linearSampler)), uv, 0.0).r;
    float v = clamp(prev + total, 0.0, 1.0);
    o_color = vec4(v, v, v, 1.0);
}
