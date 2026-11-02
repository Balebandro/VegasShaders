// Underwater passes 1 and 2, blur of the ray image (NVR DXVK). Effects/Underwater.fx.hlsl Blur: OffsetMask (0,1) vertical, (1,0) horizontal.
// Push: OffsetMask at 0, LINEAR CLAMP sampler index at 8.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer

layout(push_constant) uniform Push {
    vec2 offsetMask;      // 0
    uint samplerIndex;    // 8
    uint pad;             // 12
} pc;

#include "Underwater.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 current = sampleLod(t_rendered, pc.samplerIndex, uv);
    if (!inQuarter(uv)) {
        o_color = current;
        return;
    }
    vec2 stepUV = pc.offsetMask * 2.0 * cb.reciprocalResolution.xy;
    vec4 acc = current * 0.115;
    float wsum = 0.115;
    for (int i = 1; i <= 6; i++) {
        float w = tapWeight(i);
        vec2 a = uv + stepUV * float(i);
        vec2 b = uv - stepUV * float(i);
        if (inQuarter(a)) {
            acc += sampleLod(t_rendered, pc.samplerIndex, a) * w;
            wsum += w;
        }
        if (inQuarter(b)) {
            acc += sampleLod(t_rendered, pc.samplerIndex, b) * w;
            wsum += w;
        }
    }
    o_color = acc / wsum;
}
