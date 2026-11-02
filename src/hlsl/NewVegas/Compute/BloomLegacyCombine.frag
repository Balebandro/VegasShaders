// BloomLegacy pass 4 as a Vulkan pass (needs NVR DXVK): Effects/BloomLegacy.fx.hlsl CombinePS line for line.
// Binding 0 is the blurred glow, binding 1 the picture from before the effect (TESR_SourceBuffer). Push block: TESR_BloomLegacyValues.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_blurred;    // the blurred glow (TESR_RenderedBuffer)
layout(set = 1, binding = 1) uniform texture2D t_source;     // TESR_SourceBuffer

layout(push_constant) uniform Push {
    vec4 bloomValues;   // 0: TESR_BloomLegacyValues (x BloomIntensity, y OriginalIntensity, z BloomSaturation, w OriginalSaturation)
    uint samplerIndex;  // 16: LINEAR, CLAMP
    uint pad[3];        // 20..31
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec3 blurred = texture(sampler2D(t_blurred, NVR_SAMPLER(pc.samplerIndex)), uv).rgb;
    vec3 original = texture(sampler2D(t_source, NVR_SAMPLER(pc.samplerIndex)), uv).rgb;

    vec3 glow = clamp(blurred, 0.0, 1.0);
    float glowLuma = luma(glow);
    vec3 bloomColour = mix(vec3(glowLuma), glow, pc.bloomValues.z) * pc.bloomValues.x;

    float originalLuma = luma(original);
    vec3 originalColour = mix(vec3(originalLuma), original, pc.bloomValues.w) * pc.bloomValues.y;
    vec3 darkening = max(max(original, vec3(1.0)) - glow, vec3(0.0));

    o_color = vec4(originalColour * darkening + bloomColour, 1.0);
}
