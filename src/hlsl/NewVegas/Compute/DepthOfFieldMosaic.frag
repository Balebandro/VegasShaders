// DepthOfField, technique 0, pass 2 (halfRes) as a Vulkan pass (needs NVR DXVK): Effects/DepthOfField.fx.hlsl halfRes line for
// line; effects/DepthOfField.cpp ExtRender. t_rendered is TESR_RenderedBuffer (the blur amounts of the DoF pass), t_source is
// TESR_SourceBuffer (the image before the effect). Both LINEAR, CLAMP.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_source;     // TESR_SourceBuffer

#include "DepthOfField.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 uv = inUV;

    // remap uv to [0, 1] for each quadrant
    vec4 quadrants = getQuadrants(uv);
    uv = remapFromQuadrant(uv, quadrants);

    vec4 blur = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0);
    vec4 color = textureLod(sampler2D(t_source, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0);
    // only show the blur for both bottom quadrants (HLSL lerp with the bool quadrants.z || quadrants.w)
    color = mix(color, vec4(blur.rg, 1.0, 1.0), (quadrants.z != 0.0 || quadrants.w != 0.0) ? 1.0 : 0.0);

    o_color = color;
}
