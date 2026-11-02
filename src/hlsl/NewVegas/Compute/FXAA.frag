// FXAA as a Vulkan pass (needs NVR DXVK): Effects/FXAA.fx.hlsl line for line. effects/FXAA.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP, no mips)

layout(push_constant) uniform Push {
    vec4 reciprocalResolution;  // 0: TESR_ReciprocalResolution
    uint samplerIndex;          // 16: LINEAR, CLAMP
} pc;

layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 o_color;

const float EdgeThreshold = 0.125;
const float EdgeThresholdMin = 0.05;
const float EdgeSharpness = 8.0;

float Luma(vec3 color) { return dot(color, vec3(0.299, 0.587, 0.114)); }
vec3 Fetch(vec2 uv) { return textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).rgb; }

void main() {
    vec2 uv = uv_in;
    vec2 texel = pc.reciprocalResolution.xy;
    vec3 center = Fetch(uv);
    float lumaM = Luma(center);
    float lumaNW = Luma(Fetch(uv + vec2(-0.5, -0.5) * texel));
    float lumaNE = Luma(Fetch(uv + vec2( 0.5, -0.5) * texel)) + 1.0 / 384.0;
    float lumaSW = Luma(Fetch(uv + vec2(-0.5,  0.5) * texel));
    float lumaSE = Luma(Fetch(uv + vec2( 0.5,  0.5) * texel));
    float lumaMin = min(min(lumaNW, lumaNE), min(lumaSW, lumaSE));
    float lumaMax = max(max(lumaNW, lumaNE), max(lumaSW, lumaSE));
    float range = max(lumaMax, lumaM) - min(lumaMin, lumaM);
    if (range < max(EdgeThresholdMin, lumaMax * EdgeThreshold)) { o_color = vec4(center, 1.0); return; }
    float swMinusNE = lumaSW - lumaNE;
    float seMinusNW = lumaSE - lumaNW;
    vec2 dir = vec2(swMinusNE + seMinusNW, swMinusNE - seMinusNW);
    vec2 dir1 = dir * inversesqrt(max(dot(dir, dir), 1.0e-8));
    vec3 rgbA = Fetch(uv - dir1 * 0.5 * texel) + Fetch(uv + dir1 * 0.5 * texel);
    vec2 dir2 = clamp(dir1 / max(min(abs(dir1.x), abs(dir1.y)) * EdgeSharpness, 1.0e-4), -2.0, 2.0);
    vec3 rgbB = (Fetch(uv - dir2 * 2.0 * texel) + Fetch(uv + dir2 * 2.0 * texel)) * 0.25 + rgbA * 0.25;
    float lumaB = Luma(rgbB);
    vec3 result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA * 0.5 : rgbB;
    o_color = vec4(result, 1.0);
}
