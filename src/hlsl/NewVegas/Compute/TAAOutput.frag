// TAA's output (Effects/TAA.fx.hlsl OutputPS: the resolved frame, sharpened) as a Vulkan pass (needs NVR DXVK): effects/TAA.cpp
// ExtDraw. TAA.glsl holds the rest.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "TAA.glsl"

void main() {
    vec3 center = textureLod(sampler2D(t_history, LINEAR_S), uv, 0.0).rgb;
    float sharpness = cb.sharpen.x;
    if (sharpness <= 0.0) {
        o_color = vec4(center, 1.0);
        return;
    }

    vec2 texel = cb.reciprocalResolution.xy;
    vec3 up    = textureLod(sampler2D(t_history, LINEAR_S), uv + vec2(0.0, -texel.y), 0.0).rgb;
    vec3 down  = textureLod(sampler2D(t_history, LINEAR_S), uv + vec2(0.0, texel.y), 0.0).rgb;
    vec3 left  = textureLod(sampler2D(t_history, LINEAR_S), uv + vec2(-texel.x, 0.0), 0.0).rgb;
    vec3 right = textureLod(sampler2D(t_history, LINEAR_S), uv + vec2(texel.x, 0.0), 0.0).rgb;

    vec3 lo = min(center, min(min(up, down), min(left, right)));
    vec3 hi = max(center, max(max(up, down), max(left, right)));

    // Local contrast, relative to brightness: 0 on flat areas, toward 1 across hard edges.
    float lumaLo = dot(lo, vec3(0.2126, 0.7152, 0.0722));
    float lumaHi = dot(hi, vec3(0.2126, 0.7152, 0.0722));
    float contrast = clamp((lumaHi - lumaLo) / (lumaHi + 0.05), 0.0, 1.0);
    float amount = sharpness * 2.0 * (1.0 - 0.75 * contrast);

    vec3 blurred = 0.25 * (up + down + left + right);
    vec3 sharpened = center + (center - blurred) * amount;
    o_color = vec4(clamp(sharpened, lo, hi), 1.0);
}
