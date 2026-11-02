// Test-only: two-target gradient through the NVR extension's full-screen pass.
#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_a;
layout(location = 1) out vec4 o_b;
layout(push_constant) uniform Push { vec4 value; } pc;
void main() {
    o_a = vec4(uv, pc.value.x, 1.0);
    o_b = vec4(1.0 - uv, pc.value.y, 1.0);
}
