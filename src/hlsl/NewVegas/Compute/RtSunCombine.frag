// UNOFFICIAL 2026-10-28 RT sun shadows: the traced sun visibility (TESR_RtSunShadow, from the RT helper) into the sun shadow buffer's
// red channel (core\RtSunShadows.cpp Apply). Drawn with blend MULTIPLY and only red written (combine: red x visibility), or blend
// NONE and only red written (DebugView 1: red = visibility, the cascades and contact shadows left out). The point shadows (green)
// are never touched. DebugView 4: grey (all channels written) over the finished picture (RtSunShadows::RenderDebug). Compile: glslangValidator -V --target-env vulkan1.1 -o RtSunCombine.spv RtSunCombine.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_visibility;   // TESR_RtSunShadow

layout(push_constant) uniform Push {
    float strength;      // 0: no effect .. 1: full
    uint samplerIndex;   // 4: POINT, CLAMP
    uint grey;           // 8: 1 = vec4(v, v, v, 1) (DebugView 4)
    uint channel;        // 12: 2026-10-31 DebugView 13 / 14 (the denoisers' backbone): 0 red, 1 blue (disoccluded), 2 screen move (|rg| x 100)
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    const vec4 texel = textureLod(sampler2D(t_visibility, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0);
    float visibility = pc.channel == 1u ? texel.b : pc.channel == 2u ? min(length(texel.rg) * 100.0, 1.0) : texel.r;
    float v = mix(1.0, visibility, pc.strength);
    o_color = pc.grey != 0u ? vec4(v, v, v, 1.0) : vec4(v, 1.0, 1.0, 1.0);
}
