// AmbientLight technique AmbientLightGrid pass GridPropagate as a Vulkan pass (needs NVR DXVK): Effects/AmbientLight.fx.hlsl GridPropagate,
// line for line (AmbientLight.glsl). Into the grid atlas (2 or 4 targets: the colours only when the grid carries them).
// effects/AmbientLight.cpp ExtDraw.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_geometry;    // NVR_GridGeometry (NVR_ALPrep's stage, POINT)
layout(set = 1, binding = 1) uniform texture2D t_wave;        // NVR_GridWave (POINT)
layout(set = 1, binding = 2) uniform texture2D t_accum;       // NVR_GridAccum (LINEAR)
layout(set = 1, binding = 3) uniform texture2D t_waveColor;   // NVR_GridWaveColor (POINT)
layout(set = 1, binding = 4) uniform texture2D t_accumColor;  // NVR_GridAccumColor (LINEAR)

layout(location = 0) out vec4 o_wave;
layout(location = 1) out vec4 o_accum;
layout(location = 2) out vec4 o_waveColor;
layout(location = 3) out vec4 o_accumColor;

void main() {
    vec2 uvIn = frameUV();
    vec3 cell = AtlasCell(uvIn);
    float level = uvIn.x * AtlasDims.x >= AtlasSize ? 1.0 : 0.0;
    vec4 sh = vec4(0.0);
    vec4 colour = vec4(0.0);
    vec4 blocker = pointLod(t_geometry, uvIn);
    for (int k = 0; k < 6; k++) {
        vec3 axis = k == 0 ? vec3(1, 0, 0) : k == 1 ? vec3(-1, 0, 0) : k == 2 ? vec3(0, 1, 0) : k == 3 ? vec3(0, -1, 0) : k == 4 ? vec3(0, 0, 1) : vec3(0, 0, -1);
        vec3 neighbour = cell - axis;
        if (any(lessThan(neighbour, vec3(0.0))) || neighbour.x >= GridX || neighbour.y >= GridY || neighbour.z >= GridZ) continue;
        vec4 from = pointLod(t_wave, CellUV(neighbour, level));
        float sent = max(dot(from, ShBasis(axis)), 0.0);
        sent *= 1.0 - saturate(dot(blocker.xyz, abs(axis)) * cb.gridStep.y);
        sh += CosineLobe(axis) * (sent * cb.gridData.y);
        vec4 fromColour = pointLod(t_waveColor, CellUV(neighbour, level));
        colour += vec4(fromColour.rgb / max(fromColour.a, 1e-5), 1.0) * (sent * cb.gridData.y);
    }
    o_wave = sh;
    o_accum = (cb.gridStep.x > 0.5 ? vec4(0.0) : linearLod(t_accum, uvIn)) + sh;
    o_waveColor = colour;
    o_accumColor = linearLod(t_accumColor, uvIn) + colour;
}
