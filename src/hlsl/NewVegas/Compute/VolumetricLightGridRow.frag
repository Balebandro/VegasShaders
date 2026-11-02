// VolumetricLight technique GridRow (6) as a Vulkan pass (needs NVR DXVK): Effects/VolumetricLight.fx.hlsl GridRowPS line for
// line, Grid -> GridTemp. effects/VolumetricLight.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_grid;       // TESR_VolumetricLightGrid (LINEAR, CLAMP; read at texel centres)

#define VL_GRID
#include "VolumetricLight.glsl"

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 tile = floor(vpos / GridSize);
    vec2 cell = vpos - tile * GridSize + 0.5;
    vec4 sum = vec4(0.0);
    for (int c = 0; c < 8; c++)
        if (float(c) <= tile.x) sum += LINEAR_LOD(t_grid, GridUV(tile.y * 8.0 + float(c), cell));
    o_color = sum;
}
