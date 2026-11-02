// SkyDither as a Vulkan pass (needs NVR DXVK): Effects/SkyDither.fx.hlsl line for line (VPOS = the integer pixel position:
// floor(gl_FragCoord.xy)). effects/SkyDither.cpp ExtRender. POINT samplers, as the technique's.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (POINT)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT)

layout(push_constant) uniform Push {
    vec4 skyDitherData;  // 0: TESR_SkyDitherData (x noise size, y sky only, z frame offset)
    uint samplerIndex;   // 16: POINT, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

float GradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec4 color = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv);
    if (pc.skyDitherData.y > 0.0 && texture(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv).x < 0.996) { o_color = color; return; }
    vec2 pixel = floor(gl_FragCoord.xy) + pc.skyDitherData.z * vec2(5.588238, 3.235);
    float noise = GradientNoise(pixel) + GradientNoise(pixel + vec2(47.0, 17.0)) - 1.0;
    color.rgb += noise * pc.skyDitherData.x;
    o_color = color;
}
