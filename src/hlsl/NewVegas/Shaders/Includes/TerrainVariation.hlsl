// UNOFFICIAL 2026-10-19 [Shaders.Terrain.Variation]: stochastic texture tiling for the land, after "Procedural Stochastic Textures by
// Tiling and Blending" (Deliot & Heitz, 2019, https://eheitzresearch.wordpress.com/722-2/).
//
// Credits and licence: ported from Conor's (mrfury97) NVR fork (github.com/mrfury97/Conor-sClaudeHell-NVR-SAL-Fork, branch
// claude/terrain-variation, 66653eea + d07ff4b9, 2026-09-21, GPL-3.0-or-later), itself a port of Community Shaders' "Terrain
// Variation" (github.com/community-shaders/skyrim-community-shaders, features/Terrain Variation, GPL-3.0 with the Modding and
// Linking exceptions): the lattice, hash, corner ranking and height-weighted blend below are theirs. Changed for this build: the
// layers sample with the explicit gradients this build's terrain already passes (SkipEmptyLayers), so the two taps keep the real
// anisotropic footprint; a layer with no weight at the pixel takes neither tap; and the second tap can start beyond a distance
// (StartDistance), where the repeat shows most and the parallax near the camera already costs the most.
//
// The idea: instead of sampling a terrain texture once at the tiled UV, sample it twice at two hash-randomised offsets and blend.
// The offsets come from the two dominant corners of a triangular lattice laid over the UV, so neighbouring pixels agree and the
// result is continuous, but the texture's phase changes from cell to cell -- which breaks up the repeat that makes a large open
// landscape read as wallpaper.

// x: lattice cells per UV unit (terrain UVs are in texture-tile space: 1 = one cell per repeat), y: height influence (0 a plain
// crossfade, higher lets the taller tap win its own area), z: StartDistance (game units; nearer pixels take the single tap),
// w: the distance over which the second tap fades in after it.
// c95: clear of the land's own constants (to c88 with 24 lamps), NVR's c89-c94 and Shadow.hlsl's c100-c133.
float4 TESR_TerrainVariationData : register(c95);
// x: 1 = the normal maps vary too (0: only the colour maps; half the extra reads, the relief keeps the plain tiling).
float4 TESR_TerrainVariationExtra : register(c96);

// Maps the square UV grid onto the equilateral triangular lattice the blend is built on.
static const float2x2 TERRAIN_VARIATION_SKEW = float2x2(1.0f, 0.0f, -0.57735027f, 1.15470054f);
static const float2 TERRAIN_VARIATION_HASH = float2(1271.5151f, 3337.8237f);
static const float3 TERRAIN_VARIATION_LUMA = float3(0.2126f, 0.7152f, 0.0722f);
// Fade width for the rank swap between the second and third corners, so the discarded corner cannot pop where their weights tie.
static const float TERRAIN_VARIATION_TAP_FADE = 0.1f;
// Disables that fade near a first/second corner tie, so the primary swap stays symmetric.
static const float TERRAIN_VARIATION_DOMINANCE_GUARD = 0.02f;

struct StochasticOffsets {
	float2 offset1;
	float2 offset2;
	float  tap1Weight;  // normalised weight of tap 1, in [0.5, 1]; tap 2 takes the remainder (1: one tap, the vanilla sample)
};

float2 TerrainVariationHash(float2 s) {
	s = frac(s * TERRAIN_VARIATION_HASH);
	s += dot(s, s.yx + 19.19f);
	return frac((s.xx + s.yy) * s.yx);
}

// The two highest-weighted corners of the triangular lattice cell containing uv (the contrast exponent 2: the weights squared).
StochasticOffsets ComputeStochasticOffsets(float2 uv, float scale) {
	float2 skewUV = mul(TERRAIN_VARIATION_SKEW, uv * scale);
	float2 vxID = floor(skewUV);
	float2 f = frac(skewUV);
	float bz = 1.0f - f.x - f.y;

	float3 c0, c1, c2;
	if (bz > 0.0f) {
		c0 = float3(vxID, bz);
		c1 = float3(vxID + float2(0.0f, 1.0f), f.y);
		c2 = float3(vxID + float2(1.0f, 0.0f), f.x);
	}
	else {
		c0 = float3(vxID + 1.0f, -bz);
		c1 = float3(vxID + float2(1.0f, 0.0f), 1.0f - f.y);
		c2 = float3(vxID + float2(0.0f, 1.0f), 1.0f - f.x);
	}

	// Rank the three corners by weight, descending.
	float3 t;
	if (c1.z > c0.z) { t = c0; c0 = c1; c1 = t; }
	if (c2.z > c0.z) { t = c0; c0 = c2; c2 = t; }
	if (c2.z > c1.z) { t = c1; c1 = c2; c2 = t; }

	float w1 = saturate(c0.z);
	w1 *= w1;
	float w2 = saturate(c1.z);
	w2 *= w2;
	float rankFade = smoothstep(0.0f, TERRAIN_VARIATION_TAP_FADE, c1.z - c2.z);
	float dominanceGuard = smoothstep(0.0f, TERRAIN_VARIATION_DOMINANCE_GUARD, c0.z - c1.z);
	w2 *= lerp(1.0f, rankFade, dominanceGuard);

	StochasticOffsets o;
	o.offset1 = TerrainVariationHash(c0.xy);
	o.offset2 = TerrainVariationHash(c1.xy);
	o.tap1Weight = w1 / max(w1 + w2, 1e-8f);
	return o;
}

// Height-aware tap weight, from the albedo, handed to the matching normal fetch so colour and relief blend alike. Alpha is the
// height channel when the texture has one, luminance otherwise. (Conor.)
float StochasticWeight(float4 s1, float4 s2, float tap1Weight) {
	float h1 = lerp(dot(s1.rgb, TERRAIN_VARIATION_LUMA), s1.a, step(0.001f, s1.a));
	float h2 = lerp(dot(s2.rgb, TERRAIN_VARIATION_LUMA), s2.a, step(0.001f, s2.a));
	float influence = TESR_TerrainVariationData.y;
	float w1 = tap1Weight * (1.0f + influence * h1);
	float w2 = (1.0f - tap1Weight) * (1.0f + influence * h2);
	return w1 / max(w1 + w2, 1e-8f);
}

// Albedo: two taps with the ORIGINAL footprint (dx, dy: the uv's gradients, taken at the top level).
float4 StochasticSampleAlbedo(sampler2D tex, float2 uv, float2 dx, float2 dy, StochasticOffsets o, out float weight) {
	float4 s1 = tex2Dgrad(tex, uv + o.offset1, dx, dy);
	float4 s2 = tex2Dgrad(tex, uv + o.offset2, dx, dy);
	weight = StochasticWeight(s1, s2, o.tap1Weight);
	return lerp(s2, s1, weight);
}

// The normal / gloss map on the albedo's weight.
float4 StochasticSampleWith(sampler2D tex, float2 uv, float2 dx, float2 dy, StochasticOffsets o, float weight) {
	float4 s1 = tex2Dgrad(tex, uv + o.offset1, dx, dy);
	float4 s2 = tex2Dgrad(tex, uv + o.offset2, dx, dy);
	return lerp(s2, s1, weight);
}
