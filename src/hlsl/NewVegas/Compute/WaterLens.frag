// WaterLens as a Vulkan pass (NVR DXVK). Effects/WaterLens.fx.hlsl, line for line.
// Textures: set 1 binding 0 = the rendered frame, binding 1 = the lens noise map (Effects\water_NRM_lens.dds).
// Samplers: LINEAR CLAMP at push offset 32 (frame), LINEAR WRAP at 36 (noise map).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_noise;      // TESR_waterlensSampler

layout(push_constant) uniform Push {
    vec4 gameTime;        // 0   TESR_GameTime (z: real time)
    vec4 waterLensData;   // 16  TESR_WaterLensData (x TimeMultA, y TimeMultB, z Viscosity, w strength)
    uint samplerIndex;    // 32  LINEAR CLAMP
    uint wrapIndex;       // 36  LINEAR WRAP
    uint pad0;            // 40
    uint pad1;            // 44
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

vec4 sampleLod(texture2D t, uint samplerIndex, vec2 at) {
    return textureLod(sampler2D(t, NVR_SAMPLER(samplerIndex)), at, 0.0);
}

void main() {
    float realTime = pc.gameTime.z;
    float timeA = realTime * pc.waterLensData.x;
    float timeB = realTime * pc.waterLensData.y;

    vec2 uvA = uv * 0.5 + vec2(0.05 * sin(timeA), 0.05 * timeA);
    vec2 uvB = uv + vec2(0.0, timeB);
    vec4 noiseA = sampleLod(t_noise, pc.wrapIndex, uvA) * 2.0 - 1.0;
    vec4 noiseB = sampleLod(t_noise, pc.wrapIndex, uvB) * 2.0 - 1.0;
    noiseA.b += noiseB.a;

    vec3 dir = vec3(noiseA.x * pc.waterLensData.z, noiseA.y * pc.waterLensData.z, noiseA.z);
    dir /= max(length(dir), 1.0e-6);

    float strength = pc.waterLensData.w;
    vec3 scene = sampleLod(t_rendered, pc.samplerIndex, uv + dir.xy * (0.2 * strength)).rgb;

    vec2 bend = dir.xy * strength;
    vec2 bend2 = bend * bend;
    vec2 bend6 = bend2 * bend2 * bend2;
    float glint = clamp(bend6.x, 0.0, 1.0) + clamp(bend6.y, 0.0, 1.0);

    o_color = vec4(scene + glint * vec3(0.85, 0.85, 1.0), 1.0);
}
