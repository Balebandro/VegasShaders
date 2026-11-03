// Bloom techniques 1 to 7 (Downsample: one level into the next) as a Vulkan pass (needs NVR DXVK): Effects/Bloom.fx.hlsl
// line for line, see Bloom.glsl. effects/Bloom.cpp RenderBloomExt.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "Bloom.glsl"

layout(set = 1, binding = 0) uniform texture2D t_level;  // the level above (LINEAR, CLAMP)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = Downsample(t_level, NVR_SAMPLER(pc.samplerIndex), uv);
}
