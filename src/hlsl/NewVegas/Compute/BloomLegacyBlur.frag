// BloomLegacy passes 2 and 3 (BlurPass, horizontal then vertical) as Vulkan passes (needs NVR DXVK): Effects/BloomLegacy.fx.hlsl
// line for line. effects/BloomLegacy.cpp ExtRender; the offset mask is (1,0) for the first and (0,1) for the second.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    vec4 reciprocalResolution;  // 0: TESR_ReciprocalResolution
    vec2 offsetMask;            // 16: OffsetMaskH (1, 0) or OffsetMaskV (0, 1)
    uint samplerIndex;          // 24: LINEAR, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const int cKernelSize = 13;

const float blurWeights[cKernelSize] = float[cKernelSize](
    0.002216,
    0.008764,
    0.026995,
    0.064759,
    0.120985,
    0.176033,
    0.199471,
    0.176033,
    0.120985,
    0.064759,
    0.026995,
    0.008764,
    0.002216
);

void main() {
    vec3 color = vec3(0.0);

    for (int i = 0; i < cKernelSize; i++) {
        // BlurOffsets[i] = (i - 6) * TESR_ReciprocalResolution.xy
        vec2 blurOffset = float(i - 6) * pc.reciprocalResolution.xy;
        color += texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv + blurOffset * pc.offsetMask).rgb * blurWeights[i];
    }

    o_color = vec4(color, 1.0);
}
