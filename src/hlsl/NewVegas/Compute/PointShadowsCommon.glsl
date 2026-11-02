// PointShadows as Vulkan passes (needs NVR DXVK). The constants block is the one effects/PointShadows.cpp fills (PointShadowsUniforms,
// std140, the same offsets; its preshader fields are not read: the maths divides as Effects/PointShadows.fx.hlsl does). The lamp helpers
// are that file's, line for line. Matrices: a GLSL mat4 element [c][r] is the D3D element [c][r] of the stored matrix, so the D3D
// mul(v, M) is M * v here and the D3D mul(M, v) is v * M.
#include "Common.glsl"

layout(set = 2, binding = 0, std140) uniform PointShadowsBlock {
    vec4 shadowLightPosition[12];   // 0: xyz lamp, w radius (slot 11 has no cube map)
    vec4 lightPosition[12];         // 192: tracked lamps (w 0 ends the list)
    vec4 lightColor[24];            // 384: 0..11 shadow slots, 12..23 tracked lamps
    vec4 extraLights[16];           // 768: packed radius and weight (w 0 ends the list)
    vec4 shadowFade;                // 1024: z cube-map switch
    vec4 pointShadowNear;           // 1040: x NearFade, y EdgeFade, z reach value, w MaskSkipFar
    vec4 spotLightPosition;         // 1056
    vec4 spotLightDirection;        // 1072: w full cone angle, degrees
    vec4 spotLightColor;            // 1088
    vec4 lampContact;               // 1104
    vec4 lampContactFade;           // 1120
    vec4 lampContactBias;           // 1136
    vec4 lampContactNoise;          // 1152
    vec4 reciprocalResolution;      // 1168
    vec4 cameraPosition;            // 1184
    vec4 cameraData;                // 1200: y far distance
    mat4 viewTransform;             // 1216
    mat4 projectionTransform;       // 1280
    vec4 shadowLightPre[12];        // 1344 (preshader, not read here)
    vec4 lightPre[12];              // 1536 (preshader, not read here)
    vec4 extraPre[16];              // 1728 (preshader, not read here)
    vec4 nearPre;                   // 1984 (preshader, not read here)
    vec4 spotPre;                   // 2000 (preshader, not read here)
} cb;

float lumaOf(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Depth.hlsl toWorld(): camera-relative world direction through this pixel (view depth 1 along forward).
vec3 toWorldDir(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// LampFactor of the .fx: 0 outside the radius; normal, reach or forward mode 2.
float lampFactor(vec3 pos, vec3 n, vec3 lamp, float radius) {
    vec3 toLamp = lamp - pos;
    float d = length(toLamp) / radius;
    if (!(d < 1.0)) return 0.0;
    float reach = cb.pointShadowNear.z;
    float f;
    if (reach > 0.0) {
        f = 1.0 - smoothstep(reach, 1.0, d);
    } else {
        float s = d * d;
        f = (1.0 - s) * (1.0 - s) / (1.0 + 5.0 * s);
        if (reach == 0.0) {
            vec3 dir = toLamp / max(length(toLamp), 1e-6);
            f *= mix(1.0, dot(dir, n), smoothstep(0.0, 0.2, d));
        }
    }
    float edge = cb.pointShadowNear.y;
    if (edge > 0.0) f *= 1.0 - smoothstep(1.0 - edge, 1.0, d);
    return clamp(f, 0.0, 1.0);
}

// LampLit of the .fx: 1 lit, 0 shadowed (cube map slot), blended near the lamp by NearFade.
float lampLit(textureCube cube, uint samplerIdx, vec3 pos, vec3 lamp, float radius) {
#ifdef PS_TEST_NO_CUBES
    return 1.0;
#else
    if (cb.shadowFade.z < 0.5) return 1.0;
    vec3 toLamp = lamp - pos;
    float d = length(toLamp) / radius;
    vec3 dir = vec3(-toLamp.x, -toLamp.y, toLamp.z);
    float stored = textureLod(samplerCube(cube, NVR_SAMPLER(samplerIdx)), dir, 0.0).r;
    float lit = 1.0;
    if (stored > 0.0 && stored < 1.0) lit = (stored + 0.018 * d > d) ? 1.0 : 0.0;
    if (cb.pointShadowNear.x > 0.0) lit = mix(1.0, lit, smoothstep(0.0, cb.pointShadowNear.x / radius, stored));
    return lit;
#endif
}

// cubeLampShare of the .fx: one cube-mapped slot's share of the mask.
float cubeLampShare(textureCube cube, uint samplerIdx, int slot, vec3 pos, vec3 n) {
#ifdef PS_TEST_NO_SHADOWED
    return 0.0;
#else
    vec4 lp = cb.shadowLightPosition[slot];
    vec4 col = cb.lightColor[slot];
    float f = lampFactor(pos, n, lp.xyz, lp.w);
    if (f <= 0.0) return 0.0;
    return clamp(f * lampLit(cube, samplerIdx, pos, lp.xyz, lp.w) * lumaOf(col.rgb) * col.w, 0.0, 1.0);
#endif
}

// SpotShare of the .fx.
float spotShare(vec3 pos, vec3 n) {
    float radius = cb.spotLightPosition.w;
    if (!(radius > 0.0)) return 0.0;
    vec3 toSpot = cb.spotLightPosition.xyz - pos;
    float len = max(length(toSpot), 1e-6);
    float d = len / radius;
    if (!(d < 1.0)) return 0.0;
    float s = d * d;
    float atten = clamp((1.0 - s) * (1.0 - s) / (1.0 + 5.0 * s), 0.0, 1.0);
    float cosFull = cos(radians(cb.spotLightDirection.w));
    float cosHalf = cos(radians(cb.spotLightDirection.w * 0.5));
    float cosAlong = clamp(dot(cb.spotLightDirection.xyz, -toSpot / len), 0.0, 1.0);
    float cone = clamp((cosAlong - cosFull) / max(cosHalf - cosFull, 1e-4), 0.0, 1.0);
    cone *= cone;
    float diffuse = max(dot(toSpot / len, n), 0.0);
    vec4 col = cb.spotLightColor;
    return clamp(atten * cone * diffuse * lumaOf(col.rgb) * col.w, 0.0, 1.0);
}
