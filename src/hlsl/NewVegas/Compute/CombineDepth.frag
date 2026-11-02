// CombineDepth's separate pass as a Vulkan pass (needs NVR DXVK): Effects/CombineDepth.fx.hlsl technique 0, line for line.
// Output (target 0, R32F or G32R32F): R = viewZ / far, G = the reversed-Z depth from viewZ with the world near plane.
// effects/CombineDepth.cpp ExtRender fills the push block. No RenderScale rect clamp here (the D3D9 pass has one).
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_samplerless_texture_functions : enable
#include "Common.glsl"

layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform texture2D t_depthWorld;
layout(set = 1, binding = 1) uniform texture2D t_depthViewModel;

layout(push_constant) uniform Push {
    vec4 depthConstants;	// 0: TESR_DepthConstants (x view-model near, z 1 = reversed)
    vec4 cameraData;		// 16: TESR_CameraData (x near, y far)
    uint samplerIndex;		// 32: POINT, CLAMP
    uint pad[3];
} pc;

float viewZFromRaw(float raw, float nearZ, float farZ) {
    float span = farZ - nearZ;
    float viewZ = pc.depthConstants.z > 0.5 ?
        nearZ * farZ / (nearZ + raw * span) :
        nearZ * farZ / (farZ - raw * span);
    return max(viewZ, 1.0e-6);
}

void main() {
    vec2 uv = gl_FragCoord.xy;
    vec2 size = vec2(textureSize(t_depthWorld, 0));
    uv = uv / size;
    float nearZ = pc.cameraData.x;
    float farZ = pc.cameraData.y;

    float viewZ = viewZFromRaw(texture(sampler2D(t_depthWorld, NVR_SAMPLER(pc.samplerIndex)), uv).x, nearZ, farZ);
    float viewRaw = texture(sampler2D(t_depthViewModel, NVR_SAMPLER(pc.samplerIndex)), uv).x;
    if (viewRaw > 0.0) viewZ = viewZFromRaw(viewRaw, max(pc.depthConstants.x, 1.0e-4), farZ);

    float g = nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
    outColor = vec4(viewZ / farZ, g, 1.0, 1.0);
}
