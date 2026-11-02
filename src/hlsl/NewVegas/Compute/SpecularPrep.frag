// Specular, technique SpecularPrep (needs NVR DXVK): each 4 x 4 block's normal and depth, as Effects/Specular.fx.hlsl's
// SpecularPrepPS line for line. effects/Specular.cpp ExtRender (technique 1) draws it into TESR_SpecularNormals.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SpecularCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer
layout(set = 1, binding = 1) uniform texture2D t_normals;    // TESR_NormalsBuffer

layout(push_constant) uniform Push {
    uint pointSampler;       // 0: POINT/CLAMP heap index (filled by the host)
    uint linearSampler;      // 4: LINEAR/CLAMP heap index
    uint pad[2];
} pc;

layout(location = 0) out vec4 o_color;

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);                 // VPOS
    vec2 px = cb.reciprocalResolution.xy;
    float lowScale = cb.specularEffects.z > 0.0 ? cb.specularEffects.z : 4.0;
    vec2 uv = (vpos * lowScale + 0.5 * lowScale) * px;  // the centre of the block (a corner between its pixels)
    float depth = readDepthLod(t_depth, pc.pointSampler, uv - 0.5 * px);
    float tolerance = 0.12 * depth + 2.0;
    vec3 sum = sampleLod(t_normals, pc.linearSampler, uv - px).xyz * 2.0 - 1.0;
    float weight = 1.0;
    const vec2 q[3] = vec2[3](vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    for (int k = 0; k < 3; k++) {
        float w = abs(readDepthLod(t_depth, pc.pointSampler, uv + q[k] * 0.5 * px) - depth) < tolerance ? 1.0 : 0.0;
        sum += w * (sampleLod(t_normals, pc.linearSampler, uv + q[k] * px).xyz * 2.0 - 1.0);
        weight += w;
    }
    o_color = vec4(sum / weight, depth);
}
