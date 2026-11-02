// DepthOfField autofocus pass 6 as a Vulkan pass (needs NVR DXVK): the Combine pass of Effects/DepthOfField.fx.hlsl, line for line.
// The sharp image, the near blur where the near amount is, then the far blur where the far amount is. effects/DepthOfField.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "DepthOfField.glsl"

layout(set = 1, binding = 0) uniform texture2D t_chain;      // the blurred packed image (the blur passes' output)
layout(set = 1, binding = 1) uniform texture2D t_source;     // TESR_SourceBuffer: the image before the effect

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 sharp = DOF_SAMPLE(t_source, uv);
    float nearAmount = DOF_SAMPLE(t_chain, dofPacked(vec2(0.0, 1.0), uv)).r;
    float farAmount = DOF_SAMPLE(t_chain, dofPacked(vec2(1.0, 1.0), uv)).g;
    vec3 nearColour = DOF_SAMPLE(t_chain, dofPacked(vec2(0.0, 0.0), uv)).rgb;
    vec3 farColour = DOF_SAMPLE(t_chain, dofPacked(vec2(1.0, 0.0), uv)).rgb;
    vec3 c = mix(sharp.rgb, nearColour, nearAmount);
    c = mix(c, farColour, farAmount);
    o_color = vec4(c, sharp.a);
}
