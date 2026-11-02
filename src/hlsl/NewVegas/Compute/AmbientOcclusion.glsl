// AmbientOcclusion's shared header for NVR's Vulkan passes (needs NVR DXVK): the uniform block every AO pass reads (named cb), the
// push block (aoPush) and the sampler macros. The members are the effect's TESR_ constants in the order effects/AmbientOcclusion.cpp
// fills them (the D3D9 effect is Effects/AmbientOcclusion.fx.hlsl; the RT passes also read rtReproject and rtTemporal). Each AO pass
// keeps its own buffer of this block; the set and binding are the same for all of them.
#ifndef NVR_AMBIENT_OCCLUSION_GLSL
#define NVR_AMBIENT_OCCLUSION_GLSL

#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 2, binding = 0, std140) uniform AOBlock {
    mat4 projectionTransform;		// TESR_ProjectionTransform
    mat4 invProjectionTransform;	// TESR_InvProjectionTransform
    mat4 invViewTransform;			// TESR_InvViewTransform
    mat4 shadowCameraToLightOrtho;	// TESR_ShadowCameraToLightTransformOrtho (zero when not registered)
    vec4 aoData;					// TESR_AmbientOcclusionAOData: x samples, y strength, z clamp strength, w range
    vec4 data;						// TESR_AmbientOcclusionData: x angle bias, y luminance threshold, z blur drop, w blur radius
    vec4 gtao;						// TESR_AmbientOcclusionGTAO: x GTAO on, w max radius in pixels
    vec4 sky;						// TESR_AmbientOcclusionSky (Skylighting v1)
    vec4 sky2;						// TESR_AmbientOcclusionSky2 (Skylighting v2)
    vec4 sky3;						// TESR_AmbientOcclusionSky3
    vec4 sky4;						// TESR_AmbientOcclusionSky4
    vec4 sky5;						// TESR_AmbientOcclusionSky5
    vec4 smoothedSunDir;			// TESR_SmoothedSunDir
    vec4 shadowFade;				// TESR_ShadowFade
    vec4 reciprocalResolution;		// TESR_ReciprocalResolution
    vec4 fogData;					// TESR_FogData
    vec4 fogColor;					// TESR_FogColor
    vec4 depthConstants;			// TESR_DepthConstants
    vec4 cameraData;				// TESR_CameraData
    vec4 aoLayout;					// NVR_AOLayout
    vec4 skyLayout;					// x, y: 1 / the sky target's size; z: NVR_SkySmooth.x (0 for the AO passes)
    mat4 rtReproject;				// RT path only
    vec4 rtTemporal;				// RT path only
} cb;

// The push block of every AO pass (24 bytes).
layout(push_constant) uniform AOPush {
    vec2 axis;				// 0: the blur axis, (1, 0) for BlurX and (0, 1) for BlurY
    uint gtaoEstimate;		// 8: 1 = the GTAO estimate (only used without a compile-time variant)
    uint linearClampIndex;	// 12: sampler heap index, LINEAR, CLAMP
    uint pointClampIndex;	// 16: POINT, CLAMP
    uint pointWrapIndex;	// 20: POINT, WRAP (the blue-noise image)
} aoPush;

// The AO strength (StrengthMultiplier) as an expression of the block.
#define AOstrength (cb.aoData.y)

// The camera planes (game units) as expressions of the block.
#define nearZ (cb.cameraData.x)
#define farZ (cb.cameraData.y)

// Reads of a texture at a texture coordinate, with level 0, through the heap sampler of the push block.
#define LINEAR_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(aoPush.linearClampIndex)), (coord), 0.0)
#define POINT_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(aoPush.pointClampIndex)), (coord), 0.0)

// The AO fade: 1 from startFade (game units) on, and nothing is occluded past endFade.
const float startFade = 2000.0;
const float endFade = 8000.0;

// The sky factor of one pixel (Skylighting v2, the sky share): 1 open, down to 1 - Strength when fully covered and all sky light.
// sunMask is the sun shadow mask as the caller holds it (1 where not bound); covered is the RT path's covered value (not used here).
float SkyFactor(float visibility, vec3 worldNormal, float sunMask, float covered) {
    float skyLight = cb.sky2.z;
    float sunLight = cb.sky2.w;
    float sunE = sunLight * max(dot(worldNormal, cb.smoothedSunDir.xyz), 0.0) * sunMask;
    float share = cb.sky3.z > 0.5 ? skyLight / max(skyLight + sunE, 1.0e-6) : 1.0;
    return saturate(1.0 - cb.sky2.y * (1.0 - visibility) * share);
}

// The position of x between a and b (0 at a, 1 at b); a degenerate range is kept finite.
float invlerp(float a, float b, float x) {
    return (x - a) / max(b - a, 1.0e-6);
}

// The post-projection (reversed-Z) depth of a linear depth (viewZ / far), as CombineDepth stores it.
float projectedDepthFromLinear(float linear01) {
    float viewZ = max(linear01 * farZ, 1.0e-6);
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

#endif
