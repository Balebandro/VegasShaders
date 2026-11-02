// SunShadows depth-aware blur as a Vulkan pass: Effects/SunShadows.fx.hlsl BlurAcrossPS (pc.pass 0), BlurDownPS (1) and
// BlurDownFinalPS (2: the vertical blur, then both channels raised to Intensity).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SunShadows.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 blurred = depthAwareBlur(uv, pc.pass == 0u ? vec2(1.0, 0.0) : vec2(0.0, 1.0)).rg;
    if (pc.pass == 2u) blurred = applyIntensity(blurred);
    o_color = vec4(blurred, 0.0, 1.0);
}
