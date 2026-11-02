// The sun's shadow at a world point, from NVR's cascaded shadow maps (TESR_ShadowAtlas): the same lookup as
// SunShadows.fx.hlsl's GetLightAmount (single tap per cascade, the cascade blend, the per-cascade normal offset), for
// effects that need the sun's visibility themselves -- with ForwardShadows on (the default) the game shaders apply the
// sun's shadow to the sun's light only, and TESR_PointShadowBuffer holds the contact shadows alone.
// The including effect declares sampler2D TESR_ShadowAtlas (in its sampler order) before including this, and includes
// Includes/Shadows.hlsl (the VSM / EVSM evaluation) first.

uniform float4x4 TESR_ShadowCameraToLightTransformNear;
uniform float4x4 TESR_ShadowCameraToLightTransformMiddle;
uniform float4x4 TESR_ShadowCameraToLightTransformFar;
uniform float4x4 TESR_ShadowCameraToLightTransformLod;
uniform float4 TESR_SmoothedSunDir;
uniform float4 TESR_ShadowFormatData;   // x: mode (0 VSM, 1 EVSM2, 2 EVSM4), y: format bits
uniform float4 TESR_ShadowBlur;         // x: 1 / atlas resolution
uniform float4 TESR_ShadowNearCenter;   // xyz: centre (camera-relative world), w: radius
uniform float4 TESR_ShadowMiddleCenter;
uniform float4 TESR_ShadowFarCenter;
uniform float4 TESR_ShadowLodCenter;

float4 SunCascadeTexCoord(float4 clip4) {
	clip4.xyz /= clip4.w;
	clip4.x = clip4.x * 0.5f + 0.5f;
	clip4.y = clip4.y * -0.5f + 0.5f;
	return clip4;
}

float SunCascadeValue(float4x4 light_transform, float4 coord, float offset_x, float offset_y, float bias, float bleed_reduction) {
	float4 light_space = SunCascadeTexCoord(mul(coord, light_transform));
	light_space.xy = light_space.xy * 0.5 + float2(offset_x, offset_y);   // the cascade's quadrant of the atlas
	[branch] if (TESR_ShadowFormatData.x == 3.0f) return ShadowPCF2x2(TESR_ShadowAtlas, light_space.xy, light_space.z - 0.0002f, TESR_ShadowBlur.x);	// UNOFFICIAL PCF
	float4 moments_v = tex2Dlod(TESR_ShadowAtlas, float4(light_space.xy, 0.0f, 0.0f));
	float shadow_v;
	[branch] if (TESR_ShadowFormatData.x == 0.0f)
		shadow_v = GetLightAmountValueVSM(moments_v.xy, light_space.z, bias, bleed_reduction);
	else if (TESR_ShadowFormatData.x == 1.0f)
		shadow_v = GetLightAmountValueEVSM2(moments_v.xy, light_space.z, bias, bleed_reduction, TESR_ShadowFormatData.y);
	else
		shadow_v = GetLightAmountValueEVSM4(moments_v, light_space.z, bias, bleed_reduction, TESR_ShadowFormatData.y);
	return shadow_v;
}

// 1 lit .. 0 in the sun's shadow (camera-relative world position, world normal).
float GetSunCascadeLight(float3 positionWS, float3 normal) {
	float offset_scale = saturate(1.0f - dot(normal, TESR_SmoothedSunDir.xyz));
	float4 radii_v = float4(TESR_ShadowNearCenter.w, TESR_ShadowMiddleCenter.w, TESR_ShadowFarCenter.w, TESR_ShadowLodCenter.w);
	float4 offsetDistance = offset_scale * 2.5f * 4.0f * radii_v * max(TESR_ShadowBlur.x, 1.0f / 16384.0f);
	float bias = (TESR_ShadowFormatData.x == 0.0f ? 0.00001f : 0.01f) * (1.0f + offset_scale);
	const float blend_v = 0.9f;
#define SC_NEAR SunCascadeValue(TESR_ShadowCameraToLightTransformNear, float4(positionWS + offsetDistance.x * normal, 1), 0, 0, bias, 0.1f)
#define SC_MIDDLE SunCascadeValue(TESR_ShadowCameraToLightTransformMiddle, float4(positionWS + offsetDistance.y * normal, 1), 0.5, 0, bias, 0.2f)
#define SC_FAR SunCascadeValue(TESR_ShadowCameraToLightTransformFar, float4(positionWS + offsetDistance.z * normal, 1), 0, 0.5, bias, 0.6f)
#define SC_LOD SunCascadeValue(TESR_ShadowCameraToLightTransformLod, float4(positionWS + offsetDistance.w * normal, 1), 0.5, 0.5, bias, 0.8f)
	float4 distances_v = float4(length(positionWS - TESR_ShadowNearCenter.xyz), length(positionWS - TESR_ShadowMiddleCenter.xyz),
	                          length(positionWS - TESR_ShadowFarCenter.xyz), length(positionWS - TESR_ShadowLodCenter.xyz));
	float light_v = 1.0f;
	[branch] if (distances_v.x < radii_v.x) {
		light_v = distances_v.x < radii_v.x * blend_v ? SC_NEAR : lerp(SC_NEAR, SC_MIDDLE, smoothstep(radii_v.x * blend_v, radii_v.x, distances_v.x));
	}
	else if (distances_v.y < radii_v.y) {
		light_v = distances_v.y < radii_v.y * blend_v ? SC_MIDDLE : lerp(SC_MIDDLE, SC_FAR, smoothstep(radii_v.y * blend_v, radii_v.y, distances_v.y));
	}
	else if (distances_v.z < radii_v.z) {
		light_v = distances_v.z < radii_v.z * blend_v ? SC_FAR : lerp(SC_FAR, SC_LOD, smoothstep(radii_v.z * blend_v, radii_v.z, distances_v.z));
	}
	else if (distances_v.w < radii_v.w) {
		light_v = distances_v.w < radii_v.w * blend_v ? SC_LOD : lerp(SC_LOD, 1.0f, smoothstep(radii_v.w * blend_v, radii_v.w, distances_v.w));
	}
#undef SC_NEAR
#undef SC_MIDDLE
#undef SC_FAR
#undef SC_LOD
	return light_v;
}
