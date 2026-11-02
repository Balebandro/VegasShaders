// DepthOfField autofocus pass 2 as a Vulkan pass (needs NVR DXVK): the Mosaic pass of Effects/DepthOfField.fx.hlsl, line for line.
// Top quadrants: the sharp image at half resolution (2 x 2 box); bottom quadrants: the blur amounts (the focus pass's output).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "DepthOfField.glsl"

layout(set = 1, binding = 0) uniform texture2D t_chain;      // the blur amounts (the focus pass's output, red near, green far)
layout(set = 1, binding = 1) uniform texture2D t_source;     // TESR_SourceBuffer: the image before the effect

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 q = dofQuadOf(uv);
    vec2 p = dofQuadLocal(uv);
    vec2 o = 0.5 * pc.reciprocalResolution.xy;
    float useAmounts = q.y;
    vec4 c = mix(DOF_SAMPLE(t_source, clamp(p - o, 0.0, 1.0)), DOF_SAMPLE(t_chain, clamp(p - o, 0.0, 1.0)), useAmounts);
    c += mix(DOF_SAMPLE(t_source, clamp(p + vec2(o.x, -o.y), 0.0, 1.0)), DOF_SAMPLE(t_chain, clamp(p + vec2(o.x, -o.y), 0.0, 1.0)), useAmounts);
    c += mix(DOF_SAMPLE(t_source, clamp(p + vec2(-o.x, o.y), 0.0, 1.0)), DOF_SAMPLE(t_chain, clamp(p + vec2(-o.x, o.y), 0.0, 1.0)), useAmounts);
    c += mix(DOF_SAMPLE(t_source, clamp(p + o, 0.0, 1.0)), DOF_SAMPLE(t_chain, clamp(p + o, 0.0, 1.0)), useAmounts);
    o_color = c * 0.25;
}
