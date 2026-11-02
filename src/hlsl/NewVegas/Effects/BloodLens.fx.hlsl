// BloodLens effect (TESReloaded), D3D9 side. One unnamed technique, one pass (index 0).
// A red, wet film over the screen after a hit: the picture is warped by a blotchy pattern, the blotches take the blood
// colour and get a glossy edge. The pattern is fixed for the whole film; only the amount (Intensity x fade) changes.
// Minimal pass: the C++ does not start a film at present (the hit trigger is not wired), so this is written to the spec's
// behaviour and kept small. Vulkan twin: Compute/BloodLens.frag (same maths, same push layout).

uniform float4 TESR_BloodLensParams;	// x: pattern offset (fixed per film), y: blotch threshold, z: warp strength, w: amount (Intensity x fade)
uniform float4 TESR_BloodLensColor;	// xyz: blood colour

uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// File: red and green = warp offset field, alpha = noise field for the blotches (blue is not read).
uniform sampler2D TESR_BloodLensSampler : register(s1) < string ResourceName = "Effects\bloodlens.dds"; > = sampler_state {
    ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
	VSOUT res = (VSOUT)0.0f;
	res.vertPos = px.vertPos;
	res.UVCoord = px.UVCoord;
	return res;
}

float4 BloodLens(VSOUT px) : COLOR0
{
	float2 uv = px.UVCoord;
	float offset = TESR_BloodLensParams.x;
	float threshold_v = TESR_BloodLensParams.y;
	float warp_strength = TESR_BloodLensParams.z;
	float amount_v = TESR_BloodLensParams.w;

	// warp: the file's red and green give a two-channel offset; wrap addressing (no stretched edges)
	float2 field_v = tex2D(TESR_BloodLensSampler, uv + offset).rg * 2.0f - 1.0f;
	float2 warped_uv = uv + field_v * (0.02f * warp_strength);
	float3 scene_v = tex2D(TESR_RenderedBuffer, warped_uv).rgb;

	// blotches: the file's alpha is the noise, saturated, and only the part above the threshold turns to blood
	float noise_v = saturate(tex2D(TESR_BloodLensSampler, warped_uv + offset).a);
	float blotch_v = saturate((noise_v - threshold_v) * 4.0f);
	float film_v = blotch_v * amount_v;

	float3 tinted_v = lerp(scene_v, TESR_BloodLensColor.rgb, saturate(film_v));

	// glossy edge: the screen-space slope of the film amount, zero on flat areas
	float edge_v = length(float2(ddx(film_v), ddy(film_v))) * 20.0f;
	float gloss_v = saturate(edge_v) * saturate(blotch_v) * 0.25f;

	return float4(tinted_v + gloss_v, 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 BloodLens( );
	}
}
