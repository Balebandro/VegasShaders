// BloomLegacy pass 1 (BloomPass) as a Vulkan pass (needs NVR DXVK): Effects/BloomLegacy.fx.hlsl line for line. effects/BloomLegacy.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    vec4 bloomData;       // 0: TESR_BloomLegacyData (x luminance, y middle gray, z white cut-off)
    uint samplerIndex;    // 16: LINEAR, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec3 color = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv).rgb;

    color *= pc.bloomData.y / (pc.bloomData.x + 0.001);
    color *= (1.0 + (color / (pc.bloomData.z * pc.bloomData.z)));
    color -= 5.0;
    color /= (10.0 + max(color, vec3(0.0)));

    o_color = vec4(color, 1.0);
}
