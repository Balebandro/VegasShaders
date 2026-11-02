// Bloom technique 14 (Vulkan): the last upsample into level 0, the frame's glow (spread scale, or LinearLight back to display).
// Binding 0 is level 1, binding 1 is level 0's downsample. Bloom.glsl.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Bloom.glsl"

layout(set = 1, binding = 0) uniform texture2D t_above;   // level 1
layout(set = 1, binding = 1) uniform texture2D t_own;     // level 0's downsample

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = vec4(upsample(t_above, t_own, uv, true), 1.0);
}
