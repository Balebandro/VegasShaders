// BloomLegacy pass 1 as a Vulkan pass (needs NVR DXVK): the bright pass of Effects/BloomLegacy.fx.hlsl (BrightPS).
// Push block: TESR_BloomLegacyData, the sampler's heap index.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    vec4 bloomData;     // 0: TESR_BloomLegacyData (x Luminance, y MiddleGray, z WhiteCutOff)
    uint samplerIndex;  // 16: LINEAR, CLAMP
    uint pad[3];        // 20..31
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec3 c = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv).rgb;
    float scale = pc.bloomData.y / (pc.bloomData.x + 0.001);
    float cut = max(pc.bloomData.z, 0.001);
    vec3 v = c * scale;
    vec3 boosted = v * (1.0 + v / (cut * cut));
    o_color = vec4((boosted - 5.0) / (10.0 + max(boosted, vec3(0.0))), 1.0);
}
