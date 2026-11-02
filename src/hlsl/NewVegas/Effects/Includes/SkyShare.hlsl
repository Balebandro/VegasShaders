// UNOFFICIAL 2026-10-17 Skylighting v2 ([Shaders.AmbientOcclusion.Skylighting] Version 2; deep roadmap 3.4, evidence\skylight2-LOG.md).
// The factor the sky pass (AmbientOcclusion.fx SkyVisibilityV2) writes for the composites, from the sky visibility V (0 = the sky is
// blocked in every direction, 1 = open). A blocked sky can only take away the SKY's share of the pixel's light, never the sun's:
//   factor = 1 - Strength (1 - V) share_sky,   share_sky = A / (A + S (N.L) s)
// A = the weather's ambient (sky) light, S = the sun's light (both linear luminances, worked out on the CPU), s = the sun shadow
// mask at the pixel (1 lit, 0 in shade). In shade all the light is sky light (share 1); a sunlit surface facing the sun keeps almost
// all of its light. The same per-light idea as screen-space contact shadows applied to one light's direct term (Bend Studio, Days
// Gone; Unreal contact shadows) and as the deep roadmap's L-C1 ContactShare, here for the sky instead of the sun.
// Needs before the include: TESR_AmbientOcclusionSky2 (x on, y strength, z A, w S), TESR_AmbientOcclusionSky3 (z share apply, w debug
// view), TESR_SmoothedSunDir.

float SkyShare(float3 worldNormal, float sunMask)
{
	float ambientE = TESR_AmbientOcclusionSky2.z;
	float sunE = TESR_AmbientOcclusionSky2.w * saturate(dot(worldNormal, TESR_SmoothedSunDir.xyz)) * sunMask;
	return ambientE / max(ambientE + sunE, 1.0e-5f);
}

// Debug views (DebugView; the composites multiply the picture by the value): 1 V, 2 the sky share, 3 the factor, 4 0 where covered.
float SkyFactor(float visibility, float3 worldNormal, float sunMask, float covered)
{
	float share = TESR_AmbientOcclusionSky3.z > 0.5f ? SkyShare(worldNormal, sunMask) : 1.0f;
	float factor = 1.0f - saturate(TESR_AmbientOcclusionSky2.y * (1.0f - visibility) * share);
	float view = TESR_AmbientOcclusionSky3.w;
	if (view > 0.5f) factor = view < 1.5f ? visibility : (view < 2.5f ? share : (view < 3.5f ? factor : 1.0f - covered));
	return factor;
}
