// UNOFFICIAL 2026-10-29 RT alpha-tested casters (core\RtCore.cpp AlphaSlotOf): a diffuse texture's alpha into a 64 x 64 R16F target
// (the texture's own mips do the downsampling), which then goes into the shared alpha atlas (RtAlpha.glsl reads it in the helper).
// Any pool and any format the game uses. Compile: glslangValidator -V --target-env vulkan1.1 -o RtAlphaToAtlas.spv RtAlphaToAtlas.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_diffuse;

layout(push_constant) uniform Push {
    uint samplerIndex;   // 0: the bound sampler (linear, mips, wrap)
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    float a = texture(sampler2D(t_diffuse, NVR_SAMPLER(pc.samplerIndex)), uv).a;
    o_color = vec4(a, a, a, a);
}
