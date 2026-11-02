// Point-light cube shadow lookups (TESR_ShadowData: x filter mode, -1 = done in post (ShadowsInteriors.fx), y distance fade,
// z texel offset scale).

static const float BIAS = 0.01f;

// One cube tap, nudged by tap * texel scale in x/y. Returns the shadow value blend when the stored depth is in front of us.
float Lookup(samplerCUBE cube, float3 dir, float dist, float dark, float2 tap) {
	float3 nudged = dir + float3(tap.x * TESR_ShadowData.z, tap.y * TESR_ShadowData.z, 0.0f);
	float stored = texCUBE(cube, nudged).r;
	if (stored > 0.0f && stored < 1.0f && stored < dist - BIAS) return dark;	// something nearer the light
	return 1.0f;
}

// Adds a square grid of taps from lo to hi (inclusive, steps of stride, the same on both axes) to sum.
void cubeGridSum(inout float sum, samplerCUBE cube, float3 dir, float dist, float dark, float lo, float hi, float stride) {
	for (float v = lo; v <= hi; v += stride) {
		for (float u = lo; u <= hi; u += stride) {
			sum += Lookup(cube, dir, dist, dark, float2(u, v));
		}
	}
}

// Direction to the receiver in cube space (z flipped) and its distance relative to the light radius (light.w).
void cubeSetup(float4 receiver, float4 light, out float3 dir, out float dist) {
	float3 toPixel = receiver.xyz - light.xyz;
	toPixel.z *= -1;
	float len = length(toPixel);
	dir = toPixel / len;
	dist = len / light.w;
}

float GetLightAmount(samplerCUBE cube, float4 receiver, float4 light, float fade) {
	if (TESR_ShadowData.x == -1.0f) return 1.0f;	// post-process shadows instead

	float lit = 0.0f;
	float3 dir;
	float dist;
	cubeSetup(receiver, light, dir, dist);
	float dark = max(1.0f - fade, saturate(dist) * TESR_ShadowData.y);

	// filter sizes 3x3, 4x4, 5x5, 6x6
	if (TESR_ShadowData.x == 0.0f)
		{ cubeGridSum(lit, cube, dir, dist, dark, -0.5f, 0.5f, 0.5f); lit /= 9.0f; }
	else if (TESR_ShadowData.x == 1.0f)
		{ cubeGridSum(lit, cube, dir, dist, dark, -1.5f, 1.5f, 1.0f); lit /= 16.0f; }
	else if (TESR_ShadowData.x == 2.0f)
		{ cubeGridSum(lit, cube, dir, dist, dark, -1.0f, 1.0f, 0.5f); lit /= 25.0f; }
	else
		{ cubeGridSum(lit, cube, dir, dist, dark, -2.5f, 2.5f, 1.0f); lit /= 36.0f; }
	return lit;
}

// Wider, coarser 5x5 kernel for skin.
float GetLightAmountSkin(samplerCUBE cube, float4 receiver, float4 light, float fade) {
	if (TESR_ShadowData.x == -1.0f) return 1.0f;	// post-process shadows instead

	float lit = 0.0f;
	float3 dir;
	float dist;
	cubeSetup(receiver, light, dir, dist);
	float dark = max(1.0f - fade, saturate(dist) * TESR_ShadowData.y);
	cubeGridSum(lit, cube, dir, dist, dark, -4.0f, 4.0f, 2.0f);
	lit = lit / 25.0f;
	return lit;
}
