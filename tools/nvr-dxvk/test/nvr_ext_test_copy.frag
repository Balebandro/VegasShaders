// Test-only: sample a D3D9 texture through the sampler heap and write half of it.
#version 450
#extension GL_EXT_nonuniform_qualifier : enable
layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2D t_src;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { uint samplerIndex; } pc;
void main() {
    o_color = texture(sampler2D(t_src, s_samplers[nonuniformEXT(pc.samplerIndex & 0xFFFFu)]), uv) * 0.5;
}
