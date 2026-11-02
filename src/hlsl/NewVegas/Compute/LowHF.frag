// LowHF as a Vulkan pass (NVR DXVK). Effects/LowHF.fx.hlsl, line for line.
// Textures: set 1 binding 0 = the rendered frame. Sampler: LINEAR CLAMP at push offset 16.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer

layout(push_constant) uniform Push {
    vec4 lowHFData;       // 0   TESR_LowHFData (x desaturation, y blur, z vignette exponent, w vignette floor)
    uint samplerIndex;    // 16  LINEAR CLAMP
    uint pad0;            // 20
    uint pad1;            // 24
    uint pad2;            // 28
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

vec4 sampleLod(texture2D t, uint samplerIndex, vec2 at) {
    return textureLod(sampler2D(t, NVR_SAMPLER(samplerIndex)), at, 0.0);
}

void main() {
    float desaturation = pc.lowHFData.x;
    float blur = pc.lowHFData.y;
    float vignetteExponent = pc.lowHFData.z;
    float vignetteFloor = pc.lowHFData.w;

    vec3 color = sampleLod(t_rendered, pc.samplerIndex, uv).rgb;
    if (blur > 0.0) {
        vec3 sum = color * 0.2;
        sum += sampleLod(t_rendered, pc.samplerIndex, uv + vec2(blur, 0.0)).rgb * 0.15;
        sum += sampleLod(t_rendered, pc.samplerIndex, uv - vec2(blur, 0.0)).rgb * 0.15;
        sum += sampleLod(t_rendered, pc.samplerIndex, uv + vec2(0.0, blur)).rgb * 0.15;
        sum += sampleLod(t_rendered, pc.samplerIndex, uv - vec2(0.0, blur)).rgb * 0.15;
        sum += sampleLod(t_rendered, pc.samplerIndex, uv + vec2(2.0 * blur, 0.0)).rgb * 0.05;
        sum += sampleLod(t_rendered, pc.samplerIndex, uv - vec2(2.0 * blur, 0.0)).rgb * 0.05;
        sum += sampleLod(t_rendered, pc.samplerIndex, uv + vec2(0.0, 2.0 * blur)).rgb * 0.05;
        sum += sampleLod(t_rendered, pc.samplerIndex, uv - vec2(0.0, 2.0 * blur)).rgb * 0.05;
        color = sum;
    }

    if (desaturation > 0.0) {
        float luma = dot(color, vec3(0.2125, 0.7154, 0.0721));
        color = mix(color, vec3(luma), desaturation);
    }

    if (vignetteExponent > 0.0) {
        vec2 d = uv - 0.5;
        float factor = 1.0 - dot(d, d);
        float multiplier = clamp(pow(abs(factor), abs(vignetteExponent)) + vignetteFloor, 0.0, 1.0);
        color *= multiplier;
    }

    o_color = vec4(color, 1.0);
}
