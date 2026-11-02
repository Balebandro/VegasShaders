// Sharpening as a Vulkan pass (needs NVR DXVK): Effects/Sharpening.fx.hlsl line for line. effects/Sharpening.cpp ExtRender.
// The neighbourhood weights are the Manhattan ones the D3D9 file uses (centre 1, edges 2/3, corners 1/3).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (linear depth / farZ)

layout(push_constant) uniform Push {
    vec4 sharpeningData;       // 0: TESR_SharpeningData (x strength, y clamp (unused), z offset, w unused)
    vec4 reciprocalResolution; // 16: TESR_ReciprocalResolution
    vec4 cameraData;           // 32: TESR_CameraData (y far plane)
    uint samplerIndex;         // 48: LINEAR, CLAMP
} pc;                          // 64 bytes

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 centre = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0);

    float maxDepth = pc.sharpeningData.z * 1000.0;
    float depth = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).x * pc.cameraData.y;
    float fade = clamp((maxDepth - depth) / max(maxDepth, 0.001), 0.0, 1.0);
    fade = (maxDepth > 0.0) ? pow(fade, 4.0) : 0.0;

    vec3 sum = vec3(0.0);
    float weightSum = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            float w = (3.0 - abs(float(x)) - abs(float(y))) / 3.0;
            vec2 t = uv + vec2(float(x), float(y)) * pc.reciprocalResolution.xy;
            sum += textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), t, 0.0).rgb * w;
            weightSum += w;
        }
    }
    vec3 average = sum / weightSum;
    vec3 diff = centre.rgb - average;

    float L = dot(centre.rgb, vec3(0.2126, 0.7152, 0.0722));
    float lumaWeight = L / (L + 0.05);
    float contrast = (abs(diff.r) + abs(diff.g) + abs(diff.b)) * lumaWeight;
    float boost = clamp(contrast * 2.0 * pc.sharpeningData.x, 0.0, 1.0);

    vec3 sharpened = centre.rgb + diff * boost;
    o_color = vec4(clamp(mix(centre.rgb, sharpened, fade), 0.0, 1.0), centre.a);
}
