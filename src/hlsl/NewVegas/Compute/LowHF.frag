// LowHF as a Vulkan pass (needs NVR DXVK): Effects/LowHF.fx.hlsl line for line. effects/LowHF.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer

layout(push_constant) uniform Push {
    vec4 lowHFData;      // 0: TESR_LowHFData
    uint samplerIndex;   // 16: LINEAR, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const vec3 LuminanceConv = vec3(0.2125, 0.7154, 0.0721);

vec3 Fetch(vec2 at) { return texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), at).rgb; }

void main() {
    vec3 Color = Fetch(uv);
    if (pc.lowHFData.y > 0.0) {
        Color += Fetch(uv + pc.lowHFData.y);
        Color += Fetch(uv + pc.lowHFData.y * 2.0);
        Color += Fetch(uv - pc.lowHFData.y);
        Color += Fetch(uv - pc.lowHFData.y * 2.0);
        Color = Color / 5.0;
    }
    if (pc.lowHFData.x > 0.0)
        Color = mix(Color, vec3(dot(Color, LuminanceConv)), pc.lowHFData.x);
    if (pc.lowHFData.z > 0.0) {
        vec2 inTex = uv - 0.5;
        float vignette = 1.0 - dot(inTex, inTex);
        Color *= saturate(pow(abs(vignette), pc.lowHFData.z) + pc.lowHFData.w);
    }
    o_color = vec4(Color, 1.0);
}
