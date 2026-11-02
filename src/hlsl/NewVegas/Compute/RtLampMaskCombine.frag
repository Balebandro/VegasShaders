// UNOFFICIAL 2026-10-29 RT lamp shadows Exteriors (core\RtLampShadows.h ApplyExteriorMask; docs\next\FORK-RT-LAMPS.md): the traced
// lamp mask (RtLampTraceExterior.spv: what PointShadows.fx Shadow() sums, with traced visibility instead of the cube maps) into the
// shadow buffer's green, the lamps' channel (ShadowsExteriors.fx adds it to the sun's); drawn with only green written.
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtLampMaskCombine.spv RtLampMaskCombine.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_mask;   // R32F, 0..1

layout(push_constant) uniform Push {
    uint samplerIndex;   // 0: POINT, CLAMP
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    float m = textureLod(sampler2D(t_mask, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r;
    o_color = vec4(m, m, m, 1.0);
}
