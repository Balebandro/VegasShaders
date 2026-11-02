// Bloom techniques 8-13 (Vulkan): upsample into level i. Binding 0 is level i+1 (the wider one), binding 1 is level i's own
// downsample (kept in its own target by effects/Bloom.cpp, so nothing is read and written at once). Bloom.glsl.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Bloom.glsl"

layout(set = 1, binding = 0) uniform texture2D t_above;   // level i+1
layout(set = 1, binding = 1) uniform texture2D t_own;     // level i's downsample

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = vec4(upsample(t_above, t_own, uv, false), 1.0);
}
