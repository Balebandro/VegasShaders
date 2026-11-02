// UNOFFICIAL 2026-10-29 RT lamp shadows Denoise (core\RtLampShadows.h; docs\next\FORK-RT-LAMPS.md): the traced result (RG32F, three
// entries a pixel = cube slot + 64 x visibility in 16 steps, + the depth code; RtLampTrace.comp) written again with ranks 0 and 1's
// visibility taken from the shared denoiser's outputs (RtDenoise, run per rank with the cube slot as its id). Slots, rank 2 and the
// depth code stay as traced, so the lamp shaders read it as before (s15).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtLampRepack.spv RtLampRepack.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_traced;   // TESR_RtLampShadow (RG32F, exact integers)
layout(set = 1, binding = 1) uniform texture2D t_vis0;     // rank 0's denoised visibility (.r, 0..1)
layout(set = 1, binding = 2) uniform texture2D t_vis1;     // rank 1's

layout(push_constant) uniform Push {
    uint samplerIndex;   // 0: POINT, CLAMP
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 traced = textureLod(sampler2D(t_traced, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).rg;
    float hi = floor(traced.x / 1024.0);
    float code = floor(traced.y / 1024.0);
    vec2 e = vec2(traced.x - 1024.0 * hi, hi);
    float e2 = traced.y - 1024.0 * code;
    vec2 slots = e - 64.0 * floor(e / 64.0);
    vec2 v = vec2(textureLod(sampler2D(t_vis0, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r,
                  textureLod(sampler2D(t_vis1, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r);
    vec2 steps = floor(clamp(v, 0.0, 1.0) * 15.0 + 0.5);
    // a "none" entry (slot 63) keeps its own value
    e = mix(slots + 64.0 * steps, e, vec2(greaterThan(slots, vec2(62.5))));
    o_color = vec4(e.x + 1024.0 * e.y, e2 + 1024.0 * code, 0.0, 0.0);
}
