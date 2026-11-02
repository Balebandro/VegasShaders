// DitherBuster technique 0 as a Vulkan pass (needs NVR DXVK): Effects/DitherBuster.fx.hlsl DitherOnlyPS line for line.
// Push block: TESR_ReciprocalResolution, TESR_DitherBusterData, the sampler's heap index. No RenderScale rect steps (as the chain).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    vec4 reciprocalResolution;  // 0: TESR_ReciprocalResolution
    vec4 ditherBusterData;      // 16: TESR_DitherBusterData (x Strength, z DitherThreshold)
    uint samplerIndex;          // 32: LINEAR, CLAMP
    uint pad[3];                // 36..47
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define DB_SAMPLE(p) texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), p).rgb

void main() {
    vec2 t = pc.reciprocalResolution.xy;
    vec3 c = DB_SAMPLE(uv);
    vec3 up = DB_SAMPLE(uv + vec2(0.0, -t.y));
    vec3 down = DB_SAMPLE(uv + vec2(0.0, t.y));
    vec3 left = DB_SAMPLE(uv + vec2(-t.x, 0.0));
    vec3 right = DB_SAMPLE(uv + vec2(t.x, 0.0));

    float threshold = pc.ditherBusterData.z;
    float strength = clamp(pc.ditherBusterData.x, 0.0, 1.0);
    float lc = luma(c), lu = luma(up), ld = luma(down), ll = luma(left), lr = luma(right);
    float differences = abs(lc - lu) + abs(lc - ld) + abs(lc - ll) + abs(lc - lr);
    float iso = max(0.0, differences - 4.0 * length(vec2(lr - ll, lu - ld) * 0.5));

    vec3 mean = (c + up + down + left + right) * 0.2;
    bool isolated = threshold > 0.0 && iso > threshold;
    o_color = vec4(isolated ? mix(c, mean, strength) : c, 1.0);
}
