// VolumetricLight technique CompositeGrid (8) as a Vulkan pass (needs NVR DXVK): Effects/VolumetricLight.fx.hlsl
// CompositeGridPS and CompositeAir line for line, full resolution, through the frame chain. effects/VolumetricLight.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_grid;       // TESR_VolumetricLightGrid (LINEAR, CLAMP)

#define VL_DEPTH
#define VL_COMPOSITE
#define VL_GRID
#include "VolumetricLight.glsl"

void main() {
    vec4 color = POINT_LOD(t_rendered, uv);
    float depth = readDepthLod(uv);
    float planar = depth >= farZ * 0.996 ? MarchDistance : min(depth, MarchDistance);

    float s = sqrt(planar / MarchDistance) * 64.0;   // slice coordinate of the pixel's depth
    float z = min(floor(s), 63.0);
    vec2 local = uv * GridSize;
    float farSide = LINEAR_LOD(t_grid, GridUV(z, local)).r;
    float nearSide = z > 0.5 ? LINEAR_LOD(t_grid, GridUV(z - 1.0, local)).r : 0.0;
    float sigma = Density * length(toWorld(uv));
    vec2 air = vec2(mix(nearSide, farSide, saturate(s - z)) / cb.gridData.y, 1.0 - exp(-sigma * planar));
    o_color = CompositeAir(uv, color, air);
}
