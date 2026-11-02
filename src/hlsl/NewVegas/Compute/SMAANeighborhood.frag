// SMAA's neighborhood blending as a Vulkan pass (needs NVR DXVK): Effects/SMAA.fx.hlsl technique NeighborhoodBlending line for
// line, with SMAANeighborhoodBlendingPS from Includes/SMAA.hlsl inlined (SMAA_HLSL_3, SMAA_REPROJECTION 0). effects/SMAA.cpp
// ExtRender. Both samplers of the .fx are LINEAR, CLAMP; the targets have no mips.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_blend;      // TESR_SMAA_Blend

layout(push_constant) uniform Push {
    vec4 resolution;            // 0: TESR_SMAAResolution (SMAA_RT_METRICS: 1/w, 1/h, w, h)
    vec4 data;                  // 16: TESR_SMAAData (w subpixel shift)
    vec4 depthData;             // 32: TESR_SMAADepthData
    vec4 pred;                  // 48: TESR_SMAAPred
    vec4 toneMappingClip;       // 64: TESR_ToneMappingClip
    uint samplerIndex;          // 80: LINEAR, CLAMP
} pc;

layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 o_color;

#define SMAA_RT_METRICS pc.resolution

vec4 Blend(vec2 coord) { return texture(sampler2D(t_blend, NVR_SAMPLER(pc.samplerIndex)), coord); }	// SMAASample
vec4 ColorLevelZero(vec2 coord) { return textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), coord, 0.0); }

void main() {
    // DX9_SMAANeighborhoodBlendingVS: texcoord += SMAA_SUBPIXEL_SHIFT; SMAANeighborhoodBlendingVS
    vec2 texcoord = uv_in + pc.data.w * pc.resolution.xy;
    vec4 offset = SMAA_RT_METRICS.xyxy * vec4(1.0, 0.0, 0.0, 1.0) + texcoord.xyxy;

    // SMAANeighborhoodBlendingPS
    vec4 a;
    a.x = Blend(offset.xy).a; // Right
    a.y = Blend(offset.zw).g; // Top
    a.wz = Blend(texcoord).xz; // Bottom / Left

    if (dot(a, vec4(1.0, 1.0, 1.0, 1.0)) < 1e-5) {
        o_color = ColorLevelZero(texcoord);
    } else {
        bool h = max(a.x, a.z) > max(a.y, a.w); // max(horizontal) > max(vertical)
        vec4 blendingOffset = vec4(0.0, a.y, 0.0, a.w);
        vec2 blendingWeight = a.yw;
        // SMAAMovc(bool4(h, h, h, h), blendingOffset, float4(a.x, 0.0, a.z, 0.0)); SMAAMovc(bool2(h, h), blendingWeight, a.xz);
        if (h) blendingOffset = vec4(a.x, 0.0, a.z, 0.0);
        if (h) blendingWeight = a.xz;
        blendingWeight /= dot(blendingWeight, vec2(1.0, 1.0));
        vec4 blendingCoord = blendingOffset * vec4(SMAA_RT_METRICS.xy, -SMAA_RT_METRICS.xy) + texcoord.xyxy;
        vec4 color = blendingWeight.x * ColorLevelZero(blendingCoord.xy);
        color += blendingWeight.y * ColorLevelZero(blendingCoord.zw);
        o_color = color;
    }
}
