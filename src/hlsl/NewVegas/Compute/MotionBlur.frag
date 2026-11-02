// MotionBlur as a Vulkan pass (needs NVR DXVK), drawn twice as the D3D9 technique's two passes are: Effects/MotionBlur.fx.hlsl line for
// line. effects/MotionBlur.cpp ExtRender. Native frames only (the Vulkan passes are not used in render-rectangle frames).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    vec4 reciprocalResolution;   // 0: TESR_ReciprocalResolution
    vec4 blurParams;             // 16: TESR_MotionBlurParams (x gaussian weight, y blur scale, z offset max)
    vec4 blurData;               // 32: TESR_MotionBlurData (x horizontal, y vertical turn vector)
    uint samplerIndex;           // 48: LINEAR, CLAMP
    uint pad0, pad1, pad2;       // 52..63
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 texel = pc.reciprocalResolution.xy;
    vec2 offset = clamp(pc.blurData.xy, -pc.blurParams.z, pc.blurParams.z) * texel * pc.blurParams.y;

    float g = max(pc.blurParams.x, 0.001);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int i = 0; i < 12; i++) {
        float fi = float(i);
        float w = exp((fi * fi - 121.0) / (5.0 * g * g));
        vec2 stp = offset * (fi / 24.0);
        vec4 a = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv + stp, 0.0);
        vec4 b = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv - stp, 0.0);
        sum += (a + b) * w;
        wsum += 2.0 * w;
    }
    o_color = vec4((sum / wsum).rgb, 1.0);
}
