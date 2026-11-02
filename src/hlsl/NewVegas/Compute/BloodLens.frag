// BloodLens as a Vulkan pass (NVR DXVK). Effects/BloodLens.fx.hlsl, line for line.
// Textures: set 1 binding 0 = the rendered frame, binding 1 = the blood file (Effects\bloodlens.dds).
// Samplers: LINEAR CLAMP at push offset 32 (frame), LINEAR WRAP at 36 (file).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_blood;      // TESR_BloodLensSampler

layout(push_constant) uniform Push {
    vec4 params;          // 0   TESR_BloodLensParams
    vec4 bloodColor;      // 16  TESR_BloodLensColor
    uint samplerIndex;    // 32  LINEAR CLAMP
    uint wrapIndex;       // 36  LINEAR WRAP
    uint pad0;            // 40
    uint pad1;            // 44
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

vec4 sampleLod(texture2D t, uint samplerIndex, vec2 at) {
    return textureLod(sampler2D(t, NVR_SAMPLER(samplerIndex)), at, 0.0);
}

void main() {
    float offset = pc.params.x;
    float threshold = pc.params.y;
    float warpStrength = pc.params.z;
    float amount = pc.params.w;

    vec2 field = sampleLod(t_blood, pc.wrapIndex, uv + offset).rg * 2.0 - 1.0;
    vec2 warpedUV = uv + field * (0.02 * warpStrength);
    vec3 scene = sampleLod(t_rendered, pc.samplerIndex, warpedUV).rgb;

    float noise = clamp(sampleLod(t_blood, pc.wrapIndex, warpedUV + offset).a, 0.0, 1.0);
    float blotch = clamp((noise - threshold) * 4.0, 0.0, 1.0);
    float film = blotch * amount;

    vec3 tinted = mix(scene, pc.bloodColor.rgb, clamp(film, 0.0, 1.0));

    float edge = length(vec2(dFdx(film), dFdy(film))) * 20.0;
    float gloss = clamp(edge, 0.0, 1.0) * clamp(blotch, 0.0, 1.0) * 0.25;

    o_color = vec4(tinted + gloss, 1.0);
}
