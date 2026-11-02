// UNOFFICIAL 2026-10-28 (NVR DXVK fork, hybrid RT item 2; docs/next/FORK-RT-AO.md) [Shaders.AmbientOcclusion.RT]: the Dedicated
// techniques' Estimate from the RT helper's traced visibility (RtAOTrace.comp, read back into NVR_RTAORaw) instead of the screen samples
// (PackedEstimate / GTAOEstimate). Into NVR_AOBuffer1 (or Low1): AmbientOcclusionTemporalRT.frag then averages it with last frame's into
// NVR_AOBuffer0, where the blurs and the combine read Estimate's result as always. Output as Estimate's: x the AO after
// StrengthMultiplier, the fog and the far fade (GTAOValue's tail), y the depth. cb.rtTemporal.w = [Shaders.AmbientOcclusion.RT]
// DebugView. uv is PackedVS's: the AO target's texel centre. effects/AmbientOcclusion.cpp ExtEstimateRT.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientOcclusion.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_rt;         // NVR_RTAORaw (half resolution; LINEAR: AOLowRes reads it at quarter)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

float readDepth(vec2 coord) { return LINEAR_LOD(t_depth, coord).x * farZ; }

vec3 reconstructPosition(vec2 uv) {
    float z = projectedDepthFromLinear(LINEAR_LOD(t_depth, uv).x);
    vec4 viewSpace = cb.invProjectionTransform * vec4(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, z, 1.0);
    return viewSpace.xyz / viewSpace.w;
}

float fogCoeff(float depth) { return saturate(invlerp(cb.fogData.x, cb.fogData.y, depth)); }

void main() {
    vec3 P = reconstructPosition(uv);
    float depth = min(readDepth(uv), endFade);
    if (P.z > endFade) { o_color = vec4(1.0, depth, 0.0, 1.0); return; }
    vec4 rt = LINEAR_LOD(t_rt, uv);
    float view = cb.rtTemporal.w;
    if ((view > 0.5 && view < 1.5) || view > 3.5) { o_color = vec4(rt.x, depth, 0.0, 1.0); return; }   // 4, 5: the input checks
    if (view > 2.5 && view < 3.5) { o_color = vec4(rt.w, depth, 0.0, 1.0); return; }
    // GTAOValue's tail: the same strength, fog and fade, so the settings mean what they mean for the screen AO.
    float darkness = saturate(1.0 - (1.0 - rt.x) * AOstrength);
    float fogColor = luma(cb.fogColor.rgb);
    darkness = min(max(mix(darkness, fogColor, fogCoeff(P.z)), darkness), 1.0);
    o_color = vec4(mix(darkness, 1.0, saturate(invlerp(startFade, endFade, P.z))), depth, 0.0, 1.0);
}
