// Bloom techniques 8 to 13 (Upsample: the level above, tent filtered, added to the level itself) as a Vulkan pass (needs NVR
// DXVK): Effects/Bloom.fx.hlsl line for line, see Bloom.glsl. effects/Bloom.cpp RenderBloomExt.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "Bloom.glsl"

layout(set = 1, binding = 0) uniform texture2D t_buffer;  // the level above (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_add;     // this level's downsample (LINEAR, CLAMP)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = Upsample(t_buffer, t_add, NVR_SAMPLER(pc.samplerIndex), uv);
}
