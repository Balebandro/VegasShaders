// Underwater pass 4, the wobble (NVR DXVK). Effects/Underwater.fx.hlsl Wobble, line for line. Last pass: may write the game target.
// Push: LINEAR CLAMP sampler index at 0.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (the water result)
layout(set = 1, binding = 1) uniform texture2D t_source;     // TESR_SourceBuffer
layout(set = 1, binding = 2) uniform texture2D t_depth;      // TESR_DepthBuffer

layout(push_constant) uniform Push {
    uint linearIndex;     // 0
    uint pad0;            // 4
    uint pad1;            // 8
    uint pad2;            // 12
} pc;

#include "Underwater.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec3 eye = cb.cameraPosition.xyz;
    vec3 P = eye + toWorld(uv) * readDepthAt(t_depth, pc.linearIndex, uv);
    if (abovePixel(eye, P)) {
        o_color = sampleLod(t_source, pc.linearIndex, uv);
        return;
    }
    float wobble = cb.gameTime.z * cb.waveParams.z * 1.5;
    vec2 amplitude = vec2(0.001);
    vec2 offset = vec2(sin(wobble * 3.0 + uv.y * 60.0), cos(wobble * 3.0 + uv.x * 60.0)) * amplitude;
    o_color = sampleLod(t_rendered, pc.linearIndex, uv + offset);
}
