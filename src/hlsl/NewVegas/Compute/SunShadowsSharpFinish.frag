// SunShadows sharp finish (technique 2, second pass) as a Vulkan pass: Effects/SunShadows.fx.hlsl SharpFinishPS. The centre's red
// (twice) with the four direct neighbours whose depth agrees (2 %, at least one unit), then both channels raised to Intensity.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SunShadows.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 centre = sourceAt(uv);
    float centreZ = viewDepthAt(uv);
    float tolerance = max(0.02 * centreZ, 1.0);
    vec2 texel = cb.reciprocalResolution.xy;
    vec2 offsets[4] = vec2[4](vec2(texel.x, 0.0), vec2(-texel.x, 0.0), vec2(0.0, texel.y), vec2(0.0, -texel.y));
    float sum = 2.0 * centre.r, weights = 2.0;
    for (int i = 0; i < 4; i++) {
        vec2 tapUV = clampToTexture(uv + offsets[i]);
        float agree = abs(viewDepthAt(tapUV) - centreZ) <= tolerance ? 1.0 : 0.0;
        sum += agree * sourceAt(tapUV).r;
        weights += agree;
    }
    o_color = vec4(applyIntensity(vec2(sum / weights, centre.g)), 0.0, 1.0);
}
