// Sharpening as a Vulkan pass (needs NVR DXVK): Effects/Sharpening.fx.hlsl (kernelSize 3, falloffExponent 4) line for line.
// effects/Sharpening.cpp ExtRender. readDepth: linear depth / farZ in TESR_DepthBuffer's first channel, times farZ.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer

layout(push_constant) uniform Push {
    vec4 sharpeningData;        // 0: TESR_SharpeningData (x intensity, y clamp, z offset / depth falloff)
    vec4 reciprocalResolution;  // 16: TESR_ReciprocalResolution
    vec4 cameraData;            // 32: TESR_CameraData (y farZ)
    uint samplerIndex;          // 48: LINEAR, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const int kernelSize = 3;
const float falloffExponent = 4.0;

float readDepth(vec2 coord) {
    return texture(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord).x * pc.cameraData.y;
}

void main() {
    float intensity = pc.sharpeningData.x * 2.0;
    float maxDepth = pc.sharpeningData.z * 1000.0;

    vec4 color = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv);
    float depth = readDepth(uv);

    float falloffFactor = pow(saturate((maxDepth - depth) / maxDepth), falloffExponent);

    vec3 average = vec3(0.0);
    float sum = 0.0;
    for (int x = -kernelSize / 2; x <= kernelSize / 2; x++) {
        for (int y = -kernelSize / 2; y <= kernelSize / 2; y++) {
            float weight = 1.0 - float(abs(x) + abs(y)) / float(kernelSize);
            average += texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv + vec2(float(x), float(y)) * pc.reciprocalResolution.xy).rgb * weight;
            sum += weight;
        }
    }
    average /= sum;

    float contrast = dot(abs(color.rgb - average), vec3(luma(color.rgb) / (luma(color.rgb) + 0.05)));

    color.rgb = mix(color.rgb, color.rgb + (color.rgb - average) * saturate(contrast * intensity), falloffFactor);
    o_color = color;
}
