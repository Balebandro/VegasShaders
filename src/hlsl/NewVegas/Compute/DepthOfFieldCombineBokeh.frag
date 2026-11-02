// DepthOfField's Combine with the bokeh sprites as a Vulkan pass (needs NVR DXVK; fork-only, [Shaders.DepthOfField.Bokeh]).
// DepthOfFieldCombine.frag, plus the resolved sprites (DepthOfFieldBokehResolve.frag, half resolution, sampled once at the full-resolution
// position) added in linear light: 1 - (1 - colour) x exp(-sprites), clamped to white. effects/DepthOfField.cpp ExtRender (bokeh).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "DepthOfField.glsl"

layout(set = 1, binding = 0) uniform texture2D t_chain;      // the blurred packed image (the blur passes' output)
layout(set = 1, binding = 1) uniform texture2D t_source;     // TESR_SourceBuffer: the image before the effect
layout(set = 1, binding = 2) uniform texture2D t_bokeh;      // the resolved sprites (half resolution, RGBA16F)

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

    vec3 sprites = DOF_SAMPLE(t_bokeh, uv).rgb;
    vec3 lin = linearize(c);
    vec3 outLin = min(1.0 - (1.0 - lin) * exp(-sprites), vec3(1.0));
    o_color = vec4(delinearize(outLin), sharp.a);
}
