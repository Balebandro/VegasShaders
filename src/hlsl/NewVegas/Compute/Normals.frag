// Normals' pass as a Vulkan pass (needs NVR DXVK): Effects/Normals.fx.hlsl technique 0 (NormalsPS), line for line.
// Target 0: the normals buffer (A16B16G16R16F), n * 0.5 + 0.5, alpha 1. effects/Normals.cpp ExtRender fills the push block.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "Normals.glsl"

layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform texture2D t_depth;	// CombineDepth's output (R = viewZ / far)

layout(push_constant) uniform Push {
    vec4 reciprocalResolution;	// 0: TESR_ReciprocalResolution
    vec4 cameraData;			// 16: TESR_CameraData (x near, y far)
    mat4 invProjection;			// 32: TESR_InvProjectionTransform
    uint samplerIndex;			// 96: POINT, CLAMP
    uint pad[3];
} pc;

float viewZAt(vec2 uv) {
    return max(texture(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv).x * pc.cameraData.y, 1.0e-6);
}

void main() {
    vec2 t = pc.reciprocalResolution.xy;
    vec2 uv = gl_FragCoord.xy * t;
    vec2 lo = 0.5 * t;
    vec2 hi = vec2(1.0) - 0.5 * t;
    vec2 uvL1 = clamp(uv - vec2(t.x, 0.0), lo, hi), uvL2 = clamp(uv - vec2(2.0 * t.x, 0.0), lo, hi);
    vec2 uvR1 = clamp(uv + vec2(t.x, 0.0), lo, hi), uvR2 = clamp(uv + vec2(2.0 * t.x, 0.0), lo, hi);
    vec2 uvU1 = clamp(uv - vec2(0.0, t.y), lo, hi), uvU2 = clamp(uv - vec2(0.0, 2.0 * t.y), lo, hi);
    vec2 uvD1 = clamp(uv + vec2(0.0, t.y), lo, hi), uvD2 = clamp(uv + vec2(0.0, 2.0 * t.y), lo, hi);

    vec3 pC = nvrViewPosition(uv, viewZAt(uv), pc.cameraData, pc.invProjection);
    vec3 pL1 = nvrViewPosition(uvL1, viewZAt(uvL1), pc.cameraData, pc.invProjection);
    vec3 pL2 = nvrViewPosition(uvL2, viewZAt(uvL2), pc.cameraData, pc.invProjection);
    vec3 pR1 = nvrViewPosition(uvR1, viewZAt(uvR1), pc.cameraData, pc.invProjection);
    vec3 pR2 = nvrViewPosition(uvR2, viewZAt(uvR2), pc.cameraData, pc.invProjection);
    vec3 pU1 = nvrViewPosition(uvU1, viewZAt(uvU1), pc.cameraData, pc.invProjection);
    vec3 pU2 = nvrViewPosition(uvU2, viewZAt(uvU2), pc.cameraData, pc.invProjection);
    vec3 pD1 = nvrViewPosition(uvD1, viewZAt(uvD1), pc.cameraData, pc.invProjection);
    vec3 pD2 = nvrViewPosition(uvD2, viewZAt(uvD2), pc.cameraData, pc.invProjection);

    vec3 n = nvrNormal(pC, pL1, pL2, pR1, pR2, pU1, pU2, pD1, pD2);
    outColor = vec4(n * 0.5 + 0.5, 1.0);
}
