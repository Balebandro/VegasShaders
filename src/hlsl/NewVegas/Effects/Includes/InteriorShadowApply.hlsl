// The interior darkening applied to the scene colour: ShadowsInteriors.fx's last pass, and (UNOFFICIAL 2026-10-18,
// InteriorShadowFold) the fog's interior composite (VolumetricFog.fx CompositeFogInterior), which must give the same value
// for every pixel. One source for both, so the two compile from the same expressions; change it here only.
// Needs Helpers.hlsl and Depth.hlsl (with TESR_DepthBuffer), TESR_ShadowData and TESR_ShadowFade declared first, and
// INTERIOR_SHADOW_MAXDISTANCE defined (the distance where the shadows have faded out: TESR_ShadowFade.w).

// Applies the blurred shadow term to the scene colour. Shared by both techniques of ShadowsInteriors.fx.
// (depth: readDepth(uv); the Lod form below reads it with tex2Dlod, the same texel, for use inside dynamic branches)
float4 ApplyInteriorShadowDepth(float2 uv, float4 color, float shadowSample, float depth) {
    color.rgb = pows(color.rgb, 2.2); // linearise
	float3 eyeDir = toWorld(uv);
	float uniformDepth = length(depth * eyeDir);

	float Shadow = saturate(shadowSample);
	Shadow = lerp(Shadow, 1.0, invlerps(300, INTERIOR_SHADOW_MAXDISTANCE, uniformDepth)); // fade shadows with distance
	float darkness = scaledReinhard(TESR_ShadowData.y, 5.0);
    darkness = saturate(1 - darkness * TESR_ShadowFade.y); // shadowFade.y is set to 0 when shadows are disabled

	Shadow = saturate(lerp(darkness, 1, Shadow)); // shadow darkness to apply in the scene - 0 is full shadow 1 is full light
	float shadowPower = 1 - Shadow; // 1 is full shadow, useful for scaling effect

	// apply shadows to darkest values (< 1)
	float4 finalColor = saturate(Shadow * (pow(saturate(color), 1 - shadowPower * 0.2))); // boost the gamma of the base image
	finalColor = lerp(luma(finalColor), finalColor, 1 + shadowPower * 0.3); // add some saturation back to the darker parts of the image

	// readd values above 1
	finalColor += max(color - 1, 0.0);
    finalColor.rgb = pows(finalColor.rgb, 1.0/2.2); // delinearise

	return float4(finalColor.rgb, 1);
}

float4 ApplyInteriorShadow(float2 uv, float4 color, float shadowSample) {
	return ApplyInteriorShadowDepth(uv, color, shadowSample, readDepth(uv));
}

float4 ApplyInteriorShadowLod(float2 uv, float4 color, float shadowSample) {
	return ApplyInteriorShadowDepth(uv, color, shadowSample, readDepthLod(uv));
}
