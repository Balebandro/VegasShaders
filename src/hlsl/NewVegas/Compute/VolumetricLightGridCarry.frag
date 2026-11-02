// VolumetricLight technique GridCarry (7) as a Vulkan pass (needs NVR DXVK): Effects/VolumetricLight.fx.hlsl GridCarryPS line
// for line, GridTemp -> Grid. effects/VolumetricLight.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_gridTemp;   // TESR_VolumetricLightGridTemp (POINT, CLAMP)

#define VL_GRID
#include "VolumetricLight.glsl"

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 tile = floor(vpos / GridSize);
    vec2 cell = vpos - tile * GridSize + 0.5;
    vec4 sum = POINT_LOD(t_gridTemp, GridUV(tile.y * 8.0 + tile.x, cell));
    for (int r = 0; r < 7; r++)
        if (float(r) < tile.y) sum += POINT_LOD(t_gridTemp, GridUV(float(r) * 8.0 + 7.0, cell));
    o_color = sum;
}
