// VolumetricSmoke technique VolumetricSmokeHaze (every blob at quarter resolution, into TESR_SmokeHaze and TESR_SmokeCut) as a
// Vulkan pass (needs NVR DXVK): Effects/VolumetricSmoke.fx.hlsl HazePS and SegmentCut line for line.
// effects/VolumetricSmoke.cpp ExtHaze (two targets, no blending: the whole target is written).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_smoke;      // TESR_SmokeBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_low;        // TESR_SmokeBufferLow (LINEAR, CLAMP)
layout(set = 1, binding = 3) uniform texture3D t_noise;      // TESR_NoiseVolume (LINEAR, WRAP, mips)

#define VS_DEPTH
#define VS_SMOKE
#define VS_NOISE
#include "VolumetricSmoke.glsl"

layout(location = 0) out vec4 o_haze;   // TESR_SmokeHaze: optical depth, light share, flash; alpha: distance to the surface
layout(location = 1) out vec4 o_cut;    // TESR_SmokeCut: the bullets' cut of the segment smoke

// VolumetricSmoke.fx.hlsl SegmentCut: how much of the segment smoke the bullets cut along this ray (0 none .. 1 all).
float SegmentCut(vec2 uv, vec3 cam, vec3 dir, float surface) {
    float cut = 0.0;
    vec4 smoke = SmokeAtLod(uv);
    if (smoke.r >= 0.0005 && HazeInfo.y > 0.5) {
        float smokeDist = smoke.a / max(smoke.r, 0.000001);
        float spread = 6.0 + 0.15 * smokeDist;
        float total = 0.0, covered = 0.0, strongest = 0.0;
        for (int j = 0; j < 16; j++) {
            if (float(j) >= HazeInfo.y) break;
            float t0, t1, tPuff;
            float c = BulletHole(j, cam, dir, surface, t0, t1, tPuff);
            if (c > 0.0) {
                total += c;
                covered += c * saturate((min(t1, smokeDist + spread) - max(t0, smokeDist - spread)) / (2.0 * spread));
                strongest = max(strongest, c);
            }
        }
        cut = total > 0.0 ? smoothstep(0.07, 0.375, total) * saturate(covered / strongest) : 0.0;
    }
    return cut;
}

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    float scale = max(SmokeLook.z, 1.0);
    vec2 uv = (vpos + 0.5) * TESR_ReciprocalResolution.xy * scale;
    float viewDepth;
    vec3 world = reconstructWorldPosition(uv, viewDepth).xyz;
    vec3 cam = (TESR_InvViewTransform * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    vec3 toPixel = world - cam;
    float sceneDist = length(toPixel);
    vec3 dir = toPixel / max(sceneDist, 0.0001);
    const vec2 corners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    vec2 reach = (0.5 * scale - 0.5) * TESR_ReciprocalResolution.xy;
    for (int k = 0; k < 4; k++) {
        float cornerDepth;
        vec3 corner = reconstructWorldPosition(uv + corners[k] * reach, cornerDepth).xyz;
        sceneDist = min(sceneDist, length(corner - cam));
    }
    vec3 L = LightDirection();
    float flashReach2 = max(SmokeFlashColor.w * SmokeFlashColor.w, 1.0);
    vec3 sum = vec3(0.0);
    float tNear = 1e9, tFar = 0.0;
    for (int i = 0; i < 40; i++) {
        if (float(i) >= HazeInfo.x) break;
        vec4 shape = HazeShape[i];
        vec3 oc = shape.xyz - cam;
        float R2 = shape.w * shape.w;
        float tc = dot(oc, dir);
        float d2 = dot(oc, oc) - tc * tc;
        if (d2 < R2) {
            float h = sqrt(R2 - d2);
            float s0 = max(-h, 6.0 - tc), s1 = min(h, sceneDist - tc);
            if (s1 > s0) {
                // Integral of (kk - s^2/R^2)^2 ds = kk^2 s - 2 kk s^3 / (3 R^2) + s^5 / (5 R^4); through the middle it comes to
                // 16 R / 15, so rho0 = (optical depth through the middle) * 15 / (16 R).
                float iR2 = 1.0 / R2, kk = 1.0 - d2 * iR2;
                float F1 = s1 * (kk * kk - s1 * s1 * iR2 * (0.6666667 * kk - 0.2 * s1 * s1 * iR2));
                float F0 = s0 * (kk * kk - s0 * s0 * iR2 * (0.6666667 * kk - 0.2 * s0 * s0 * iR2));
                float rho = HazeLook[i].x * 0.9375 / shape.w;
                float tau = rho * (F1 - F0);
                tNear = min(tNear, tc + s0);
                tFar = max(tFar, tc + s1);
                float sm = 0.5 * (s0 + s1);
                vec3 rel = (dir * (tc + sm) - oc) / shape.w;     // the middle of the ray's stretch, from the centre, in radii
                float lit = 0.75 + 0.25 * dot(rel, L);
                vec3 toFlash = SmokeFlash.xyz - shape.xyz;
                float flash = SmokeFlash.w * exp(-dot(toFlash, toFlash) / (flashReach2 + 0.25 * R2));
                sum += tau * vec3(1.0, lit, flash);
            }
        }
    }
    // Bullets clear tunnels through the haze (BulletField): the haze in the merged holes' stretch along the ray goes.
    if (sum.x > 0.00001 && HazeInfo.y > 0.5) {
        float h0, h1, hp;
        float k = BulletField(cam, dir, sceneDist, h0, h1, hp);
        if (k > 0.0 && h1 > h0) {
            float inside = 0.0;
            for (int i = 0; i < 40; i++) {
                if (float(i) >= HazeInfo.x) break;
                vec4 shape = HazeShape[i];
                vec3 oc = shape.xyz - cam;
                float R2 = shape.w * shape.w;
                float tc = dot(oc, dir);
                float d2 = dot(oc, oc) - tc * tc;
                if (d2 < R2) {
                    float h = sqrt(R2 - d2);
                    float iR2 = 1.0 / R2, kk = 1.0 - d2 * iR2;
                    float s0 = max(-h, h0 - tc), s1 = min(h, h1 - tc);
                    if (s1 > s0) inside += HazeLook[i].x * 0.9375 / shape.w * (BlobStretch(s1, kk, iR2) - BlobStretch(s0, kk, iR2));
                }
            }
            // Softly by how see-through it looks, not by how much haze goes (see the same in CompositePS).
            float seen = mix(exp(-sum.x), exp(-max(sum.x - inside, 0.0)), k);
            sum *= saturate(-log(max(seen, 0.000001)) / sum.x);
        }
    }
    if (sum.x > 0.00001) {
        // The noise is read where this ray crosses planes fixed in the world (across the axis the ray runs most along, every D
        // units, D doubling for long stretches so there are at most 8), inside the haze's own stretch along the ray. It drifts
        // all the time (a slow breeze, rising a little).
        float pixelAngle = 2.0 * scale * TESR_ReciprocalResolution.y / TESR_ProjectionTransform[1][1];
        vec3 drift = vec3(0.35, 0.2, 0.25) * SmokeParams.w * 0.05;
        vec3 ad = abs(dir);
        vec3 axis = ad.x > ad.y && ad.x > ad.z ? vec3(1.0, 0.0, 0.0) : (ad.y > ad.z ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
        float dirA = dot(dir, axis);
        float camA = dot(TESR_CameraPosition.xyz, axis);
        float extent = max(tFar - tNear, 0.0);
        float D = 30.0 * exp2(ceil(log2(max(extent / 210.0, 1.0))));
        float side = dirA >= 0.0 ? 1.0 : -1.0;
        float atNear = camA + dirA * tNear;
        float first = (side > 0.0 ? ceil(atNear / D) : floor(atNear / D)) * D;
        float total = 0.0, total2 = 0.0, acc = 0.0;
        for (int k = 0; k < 8; k++) {
            float t = (first + side * float(k) * D - camA) / (abs(dirA) > 0.0001 ? dirA : 0.0001);
            if (t > tFar) break;
            float w = saturate((t - tNear) / (0.5 * D)) * saturate((tFar - t) / (0.5 * D));
            if (w > 0.0) {
                vec3 p = (dir * t + TESR_CameraPosition.xyz) / 45.0 + drift;
                acc += w * noiseFbmLod(p, log2(max(t * pixelAngle / 2.8125, 1.0)));
                total += w;
                total2 += w * w;
            }
        }
        // (A stretch thinner than the planes' spacing: its middle, once.)
        if (total < 0.001) {
            float t = 0.5 * (tNear + tFar);
            acc = noiseFbmLod((dir * t + TESR_CameraPosition.xyz) / 45.0 + drift, log2(max(t * pixelAngle / 2.8125, 1.0)));
            total = 1.0;
            total2 = 1.0;
        }
        // Averaging several reads flattens the contrast by about the square root of how many count: restored by that.
        float spread = clamp(sqrt(total * total / max(total2, 0.000001)), 1.0, 2.5);
        float n = saturate((acc / total - 0.5) * 1.6 * spread + 0.5);
        sum *= mix(0.25, 1.7, n);
    }
    o_haze = vec4(sum.xyz, sceneDist);
    o_cut = vec4(SegmentCut(uv, cam, dir, sceneDist), 0.0, 0.0, sceneDist);
}
