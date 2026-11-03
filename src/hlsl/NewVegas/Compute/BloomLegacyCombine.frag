// BloomLegacy pass 4 (CombinePass) as a Vulkan pass (needs NVR DXVK): Effects/BloomLegacy.fx.hlsl line for line.
// effects/BloomLegacy.cpp ExtRender. t_bloom is the blurred bloom (TESR_RenderedBuffer), t_source the image before the effect (TESR_SourceBuffer).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_bloom;      // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_source;     // TESR_SourceBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    vec4 bloomValues;     // 0: TESR_BloomLegacyValues (x bloom intensity, y original intensity, z bloom saturation, w original saturation)
    uint samplerIndex;    // 16: LINEAR, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// HLSL lerp(grey, color, saturation) = mix(vec3(grey), color, saturation)
vec3 adjustSaturation(vec3 color, float saturation) {
    float grey = dot(color, vec3(0.2126, 0.7152, 0.0722)); // same as our luma() helper function

    return mix(vec3(grey), color, saturation);
}

void main() {
    // Needs saturate() to not go crazy as the bloom wasn't targeting the 0-1 range
    vec3 bloomColor = saturate(texture(sampler2D(t_bloom, NVR_SAMPLER(pc.samplerIndex)), uv).rgb);
    vec3 originalColor = texture(sampler2D(t_source, NVR_SAMPLER(pc.samplerIndex)), uv).rgb;

    bloomColor = adjustSaturation(bloomColor, pc.bloomValues.z) * pc.bloomValues.x;
    originalColor = adjustSaturation(originalColor, pc.bloomValues.w) * pc.bloomValues.y;
    // darken the background by the inverse bloom channel value (relative to the background max, to support HDR backgrounds beyond 1)
    originalColor *= max((max(originalColor, vec3(1.0)) - bloomColor), vec3(0.0));

    o_color = vec4(originalColor + bloomColor, 1.0);
}
