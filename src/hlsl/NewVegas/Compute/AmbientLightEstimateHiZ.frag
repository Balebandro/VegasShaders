// UNOFFICIAL 2026-10-26 (NVR DXVK, deep/fork-hiz): AmbientLightEstimate.frag with [Shaders.AmbientLight.Main] HiZ above 0: the far horizon
// taps read the depth from a coarse level of the Hi-Z pyramid (HiZBuild.comp, core/HiZ.cpp; the nearest depth of the cell, the weapon
// left out) instead of the half-resolution prep, as Scalable Ambient Obscurance (McGuire et al. 2012) does: a tap 2^(HiZ + m) or more
// prep texels from the pixel reads level m (m >= 1, from the prep's own resolution), so the far taps of neighbouring pixels hit the same
// few texels in the cache instead of scattered 16-byte prep texels. Near taps (and HiZ 0) read the prep exactly as before.
// Into NVR_AmbientLight0 (half resolution). effects/AmbientLight.cpp ExtDraw.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_prep;       // NVR_ALPrep (POINT)
layout(set = 1, binding = 1) uniform texture2D t_blueNoise;  // TESR_BlueNoiseSampler (POINT, WRAP)
layout(set = 1, binding = 2) uniform texture2D t_hiz;        // NVR_HiZ: r nearest linear depth / farZ; level k = 2^(k+1) full-res pixels

// The view-space depth for a tap at sampleUV (the prep texel's centre), offsetTexels prep texels from the pixel.
float TapViewZ(vec2 sampleUV, float offsetTexels, float prepToHiz) {
    float logQ = float(pc.variant);
    float m = floor(log2(max(offsetTexels, 1.0))) - logQ;   // the prep-resolution level this tap may read
    if (m < 1.0) return pointLod(t_prep, sampleUV).x;
    int level = int(min(m + prepToHiz, 6.0));   // NVR_HiZ level: prep level m is HiZ level m + log2(prep scale) - 1
    ivec2 cell = ivec2(floor(sampleUV / cb.reciprocalResolution.xy)) >> (level + 1);
    float z = texelFetch(sampler2D(t_hiz, POINT_S), cell, level).x;
    return z >= 3.0 ? 1.0e6 : z * farZ;   // a cell that is all weapon: as the prep's weapon (never an occluder)
}

layout(location = 0) out vec4 o_color;

float SliceArc(float horizonCos0, float horizonCos1, float n, float cosNormal, float sinN) {
    float h0 = n + clamp(-acos(clamp(horizonCos1, -1.0, 1.0)) - n, -1.5707963, 1.5707963);
    float h1 = n + clamp(acos(clamp(horizonCos0, -1.0, 1.0)) - n, -1.5707963, 1.5707963);
    return (cosNormal + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) * 0.25 + (cosNormal + 2.0 * h1 * sinN - cos(2.0 * h1 - n)) * 0.25;
}

vec4 Estimate(vec2 uv) {
    vec4 center = pointLod(t_prep, uv);
    vec3 origin = PositionFromViewZ(uv, center.x);
    float fade = smoothstep(NearFade, NearFade * 2.0, origin.z) * (1.0 - smoothstep(DrawDistance * 0.7, DrawDistance, origin.z));
    if (fade <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);

    vec3 normal = OctDecode(Unpack2(center.y));
    vec3 viewVec = normalize(-origin);
    vec3 noise = textureLod(sampler2D(t_blueNoise, NVR_SAMPLER(pc.pointWrap)), uv / (256.0 * cb.alLayout.zw), 0.0).xyz;
    noise.xy = fract(noise.xy + (cb.alTemporal.x > 0.0 ? cb.alTemporal.z : 0.0) * vec2(0.618034, 0.754878));
    vec2 screenRadius = Radius * vec2(cb.projectionTransform[0][0], cb.projectionTransform[1][1]) / (2.0 * origin.z);
    screenRadius *= min(1.0, MaxScreenRadius / screenRadius.x);
    float falloffStart = 0.4 * Radius;

    if (DebugView > 2.5 && DebugView < 3.5) return vec4(normal * 0.5 + 0.5, 1.0);
    if (DebugView > 3.5 && DebugView < 4.5) return vec4(origin.z / 2000.0, fract(origin.z / 100.0), 0.0, 1.0);
    if (DebugView > 4.5 && DebugView < 5.5) return vec4(screenRadius * 10.0, Radius / 1000.0, 1.0);
    float visibility = 0.0, weightSum = 0.0;
    vec3 bounce = vec3(0.0);
    float firstCos = 0.0, firstLow = 0.0;
    float prepToHiz = log2(max(floor(cb.alLayout.z / cb.reciprocalResolution.x + 0.5), 1.0)) - 1.0;   // 0 at half resolution
    for (int slice = 0; slice < SLICES; slice++) {
        float phi = (float(slice) + noise.x) * (3.14159265 / float(SLICES));
        vec2 omega = vec2(cos(phi), -sin(phi));
        vec3 direction = vec3(cos(phi), sin(phi), 0.0);
        vec3 orthoDirection = direction - dot(direction, viewVec) * viewVec;
        vec3 axis = normalize(cross(orthoDirection, viewVec));
        vec3 projectedNormal = normal - axis * dot(normal, axis);
        float projectedLength = length(projectedNormal) + 1e-5;
        float signNormal = dot(orthoDirection, projectedNormal) >= 0.0 ? 1.0 : -1.0;
        float cosNormal = saturate(dot(projectedNormal, viewVec) / projectedLength);
        float n = signNormal * acos(cosNormal);
        float lowCos0 = cos(n + 1.5707963 - cb.alEdges.y), lowCos1 = cos(n - 1.5707963 + cb.alEdges.y);
        float horizonCos0 = lowCos0, horizonCos1 = lowCos1;
        for (int k = 0; k < STEPS; k++) {
            float s = (float(k) + fract(noise.y + float(k) * 0.618034)) / float(STEPS);
            s *= s;
            vec2 offset = omega * screenRadius * max(s, 2.0 * cb.alLayout.z);
            for (int side = 0; side < 2; side++) {
                vec2 sampleUV = uv + (side == 0 ? offset : -offset);
                float onScreen = step(0.0, sampleUV.x) * step(sampleUV.x, 1.0) * step(0.0, sampleUV.y) * step(sampleUV.y, 1.0);
                sampleUV = (floor(clamp(sampleUV, 0.0, 1.0) / cb.alLayout.zw) + 0.5) * cb.alLayout.zw;
                float offsetTexels = length(offset / cb.alLayout.zw);
                vec3 delta = PositionFromViewZ(sampleUV, TapViewZ(sampleUV, offsetTexels, prepToHiz)) - origin;
                float dist = length(delta) + 1e-4;
                vec3 dir = delta / dist;
                float weight = saturate((Radius - dist) / (Radius - falloffStart)) * onScreen * step(NearFade, origin.z + delta.z);
                float lowCos = side == 0 ? lowCos0 : lowCos1;
                float sampleCos = mix(lowCos, dot(dir, viewVec), weight);
                if (side == 0) horizonCos0 = max(horizonCos0, sampleCos); else horizonCos1 = max(horizonCos1, sampleCos);
                if (slice == 0 && side == 0 && k == 1) { firstCos = dot(dir, viewVec); firstLow = weight; }
            }
        }
        float sinN = sin(n);
        visibility += projectedLength * SliceArc(horizonCos0, horizonCos1, n, cosNormal, sinN);
        weightSum += projectedLength * SliceArc(lowCos0, lowCos1, n, cosNormal, sinN);
    }
    if (DebugView > 5.5) return vec4(visibility / max(weightSum, 1e-4), firstCos * 0.5 + 0.5, firstLow, 1.0);
    visibility = pow(saturate(visibility / max(weightSum, 1e-4)), 1.5 * OcclusionStrength);
    visibility = mix(1.0, visibility, fade);
    return vec4(bounce * (BounceScale * fade / float(SLICES * STEPS * 2)), saturate(visibility));
}

void main() { o_color = Estimate(frameUV()); }
