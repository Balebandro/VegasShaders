// Bloom techniques 1-7 (Vulkan): one level into the next (the level above is binding 0). Bloom.glsl.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Bloom.glsl"

layout(set = 1, binding = 0) uniform texture2D t_above;   // the level above (LINEAR, CLAMP)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = vec4(downsample13(t_above, uv, false), 1.0);
}
