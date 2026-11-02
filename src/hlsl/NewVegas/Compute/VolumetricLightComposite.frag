// VolumetricLight technique Composite (0) as a Vulkan pass (needs NVR DXVK): Effects/VolumetricLight.fx.hlsl CompositePS and
// CompositeAir line for line, full resolution, through the frame chain. effects/VolumetricLight.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_buffer;     // TESR_VolumetricLightBuffer (POINT, CLAMP)

#define VL_DEPTH
#define VL_COMPOSITE
#include "VolumetricLight.glsl"

void main() {
    vec4 color = POINT_LOD(t_rendered, uv);
    float depth = readDepthLod(uv);

    // Depth-aware bilinear upsample from the half-resolution buffer (texel k stands for its centre).
    vec2 position = uv / LowTexel - 0.5;
    vec2 base = floor(position);
    vec2 fraction = position - base;
    float depth01 = depth / farZ;
    float tolerance = max(depth01 * 0.04, 0.00002);
    vec2 air = vec2(0.0);
    float weights = 1.0e-5;
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            vec4 tap = POINT_LOD(t_buffer, (base + vec2(x, y) + 0.5) * LowTexel);
            float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);
            weight /= 1.0 + abs(tap.b - depth01) / tolerance;
            air += tap.rg * weight;
            weights += weight;
        }
    }
    air /= weights;
    o_color = CompositeAir(uv, color, air);
}
