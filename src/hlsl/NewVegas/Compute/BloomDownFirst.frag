// Bloom technique 0 (Vulkan): the scene into level 0 (GlowBoost, LinearLight and AntiFlicker apply here). Bloom.glsl.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Bloom.glsl"

layout(set = 1, binding = 0) uniform texture2D t_scene;   // TESR_RenderedBuffer (LINEAR, CLAMP)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = vec4(downsample13(t_scene, uv, true), 1.0);
}
