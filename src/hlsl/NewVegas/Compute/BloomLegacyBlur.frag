// BloomLegacy passes 2 and 3 as a Vulkan pass (needs NVR DXVK): the 13-tap blur of Effects/BloomLegacy.fx.hlsl (BlurLine).
// The direction comes from the push block: (1, 0) then (0, 1).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // the picture (the previous pass's output)

layout(push_constant) uniform Push {
    vec4 reciprocalResolution;  // 0: TESR_ReciprocalResolution
    vec2 offsetMask;            // 16: (1, 0) or (0, 1)
    uint samplerIndex;          // 24: LINEAR, CLAMP
    uint pad;                   // 28
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 step = pc.reciprocalResolution.xy * pc.offsetMask;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = -6; i <= 6; i++) {
        float w = exp(-float(i * i) / 8.82);
        sum += w * texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv + step * float(i)).rgb;
        total += w;
    }
    o_color = vec4(sum / total, 1.0);
}
