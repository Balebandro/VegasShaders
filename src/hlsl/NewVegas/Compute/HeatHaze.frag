// HeatHaze as a Vulkan pass (needs NVR DXVK): Effects/HeatHaze.fx.hlsl line for line. effects/HeatHaze.cpp ExtRender.
// t_depth is TESR_DepthBuffer (CombineDepth's linear depth / far, one R32F channel; POINT, CLAMP), t_rendered is
// TESR_RenderedBuffer (LINEAR, CLAMP). The push block holds the TESR_ values the technique reads (the matrices reduced to the
// terms toWorld uses), 104 bytes.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer

layout(push_constant) uniform Push {
    vec4 heatHazeData;     // 0: TESR_HeatHazeData (x offset at full haze, y start distance, z full distance, w horizon band)
    vec4 heatHazeNoise;    // 16: TESR_HeatHazeNoise (x noise scale, y speed, z time, w heat)
    vec4 misc;             // 32: x TESR_ProjectionTransform[0][0], y [1][1], z TESR_CameraData.y (far Z), w TESR_ReciprocalResolution.z
    vec4 viewCol0;         // 48: TESR_ViewTransform[0..2][0]
    vec4 viewCol1;         // 64: TESR_ViewTransform[0..2][1]
    vec4 viewCol2;         // 80: TESR_ViewTransform[0..2][2]
    uint linearIndex;      // 96: LINEAR, CLAMP (the sampler heap index, written by NVR)
    uint pointIndex;       // 100: POINT, CLAMP (the sampler heap index, written by NVR)
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const float PI = 3.14159265;

// Depth.hlsl readDepth
float readDepth(vec2 coord) {
    return texture(sampler2D(t_depth, NVR_SAMPLER(pc.pointIndex)), coord).x * pc.misc.z;
}

// Depth.hlsl toWorld
vec3 toWorld(vec2 tex) {
    vec3 v = pc.viewCol2.xyz;
    v += (1.0 / pc.misc.x * (2.0 * tex.x - 1.0)) * pc.viewCol0.xyz;
    v += (-1.0 / pc.misc.y * (2.0 * tex.y - 1.0)) * pc.viewCol1.xyz;
    return v;
}

// HLSL fmod: x - y * trunc(x / y)
float hfmod(float x, float y) { return x - y * trunc(x / y); }

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// Value noise that wraps every `period` cells in x, so the full turn around the camera has no seam.
float valueNoise(vec2 p, float period, float seed) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float x0 = hfmod(i.x, period);
    float x1 = hfmod(i.x + 1.0, period);
    float a = hash(vec2(x0, i.y) + seed);
    float b = hash(vec2(x1, i.y) + seed);
    float c = hash(vec2(x0, i.y + 1.0) + seed);
    float d = hash(vec2(x1, i.y + 1.0) + seed);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Two octaves, -1 to 1.
float hazeNoise(vec2 p, float period, float seed) {
    float n = valueNoise(p, period, seed) * 0.65 + valueNoise(p * 2.0, period * 2.0, seed + 17.0) * 0.35;
    return n * 2.0 - 1.0;
}

void main() {
    vec3 color = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.linearIndex)), uv).rgb;

    float startDistance = pc.heatHazeData.y;
    float fullDistance = pc.heatHazeData.z;

    float depth = readDepth(uv);
    float distanceFactor = smoothstep(startDistance, fullDistance, depth);
    if (distanceFactor <= 0.0) {
        o_color = vec4(color, 1.0);
        return;
    }

    // Strongest along the horizon, fading above and below it.
    vec3 dir = normalize(toWorld(uv));
    float horizon = 1.0 - smoothstep(0.0, pc.heatHazeData.w, abs(dir.z));
    float amount = pc.heatHazeData.x * distanceFactor * horizon;
    if (amount <= 1e-6) {
        o_color = vec4(color, 1.0);
        return;
    }

    // Noise coordinates from the view direction (see the .fx.hlsl), drifting upward over time.
    float period = max(1.0, floor(pc.heatHazeNoise.x * 2.0 * PI));
    float around = (atan(dir.y, dir.x) / (2.0 * PI) + 0.5) * period;
    float up = dir.z * pc.heatHazeNoise.x * 3.0 - pc.heatHazeNoise.z * pc.heatHazeNoise.y;
    vec2 p = vec2(around, up);

    // Mostly vertical waver, some sideways.
    vec2 offset = vec2(hazeNoise(p, period, 3.0) * 0.5, hazeNoise(p, period, 41.0)) * amount;
    offset.x /= pc.misc.w;   // the same distance in pixels both ways

    // Refuse a fetch that lands on something near: it would smear that object into the haze.
    vec2 sampleUV = uv + offset;
    if (readDepth(sampleUV) < startDistance) {
        o_color = vec4(color, 1.0);
        return;
    }

    o_color = vec4(texture(sampler2D(t_rendered, NVR_SAMPLER(pc.linearIndex)), sampleUV).rgb, 1.0);
}
