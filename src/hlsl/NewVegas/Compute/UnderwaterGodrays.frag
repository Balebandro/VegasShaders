// Underwater pass 0, god rays (NVR DXVK). Effects/Underwater.fx.hlsl Godrays, line for line. effects/Underwater.cpp ExtRender.
// Samplers: LINEAR CLAMP at push offset 0, POINT WRAP (blue noise) at 4, ANISOTROPIC WRAP (caustics) at 8.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer
layout(set = 1, binding = 2) uniform texture2D t_noise;      // TESR_BlueNoiseSampler
layout(set = 1, binding = 3) uniform texture2D t_caustics;   // TESR_CausticsSampler

layout(push_constant) uniform Push {
    uint linearIndex;     // 0
    uint noiseIndex;      // 4
    uint causticsIndex;   // 8
    uint pad;             // 12
} pc;

#include "Underwater.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 current = sampleLod(t_rendered, pc.linearIndex, uv);
    if (!inQuarter(uv)) {
        o_color = current;
        return;
    }
    vec2 local = uv * 2.0;
    vec3 eye = cb.cameraPosition.xyz;
    float W = cb.waterSettings.x;
    vec3 dirV = toWorld(local);
    float viewZ = readDepthAt(t_depth, pc.linearIndex, local);
    float dist = length(dirV) * viewZ;
    vec3 dir = dirV / max(length(dirV), 1.0e-6);
    float maxT = min(dist, 4000.0);
    float stepLen = maxT / 49.0;

    vec3 sun = sunDirection();
    float sunZ = max(sun.z, 0.05);
    float sunUp = clamp(sun.z * 6.0, 0.0, 1.0);
    float drift = cb.gameTime.x * 0.001 * cb.waveParams.z * 0.02;

    vec2 pixel = gl_FragCoord.xy;
    float acc = 0.0;
    for (int i = 0; i < 49; i++) {
        float noise = sampleLod(t_noise, pc.noiseIndex, (pixel + vec2(17.0, 31.0) * float(i)) / 256.0).r;
        float t = (float(i) + noise) * stepLen;
        vec3 p = eye + dir * t;
        float under = step(p.z, W);
        vec3 q = p + sun * (max(W - p.z, 0.0) / sunZ);
        float c1 = sampleLod(t_caustics, pc.causticsIndex, q.xy / 600.0 + vec2(drift, drift * 0.7)).r;
        float c2 = sampleLod(t_caustics, pc.causticsIndex, q.xy / 720.0 - vec2(drift * 0.8, drift)).r;
        acc += min(c1, c2) * under * exp(-t * 0.0008);
    }
    float ray = 2.0 * acc / 49.0 * sunUp;
    o_color = vec4(ray, 0.0, 0.0, 1.0);
}
