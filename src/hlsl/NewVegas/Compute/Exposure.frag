// Exposure's pass as a Vulkan pass (needs NVR DXVK): Effects/Exposure.fx.hlsl technique 0 (ExposurePS), line for line.
// Output: the frame's colour corrected, delinearised, alpha 1. effects/Exposure.cpp ExtRender fills the push block.
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_samplerless_texture_functions : enable
#include "Common.glsl"

layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform texture2D t_rendered;	// TESR_RenderedBuffer (the frame before exposure)
layout(set = 1, binding = 1) uniform texture2D t_avgLuma;	// TESR_AvgLumaBuffer (1 x 1)

layout(push_constant) uniform Push {
    vec4 exposureData;	// 0: TESR_ExposureData (x pivot, y strength)
    uint samplerIndex;	// 16: LINEAR, CLAMP
    uint pad[3];
} pc;

void main() {
    const float floorValue = 1.0e-6;
    vec2 size = vec2(textureSize(t_rendered, 0));
    vec2 uv = gl_FragCoord.xy / size;

    float average = max(textureLod(sampler2D(t_avgLuma, NVR_SAMPLER(pc.samplerIndex)), vec2(0.5), 0.0).g, floorValue);
    float pivot = max(pc.exposureData.x, floorValue);
    float strength = pc.exposureData.y;

    float up = max(log(average / pivot), 0.0) + 1.0;
    float down = max(log(pivot / average), 0.0) + 1.0;
    float factor = up / down;

    vec3 linearColour = linearize(textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).rgb);
    vec3 corrected = mix(linearColour, linearColour / factor, strength);
    outColor = vec4(delinearize(corrected), 1.0);
}
