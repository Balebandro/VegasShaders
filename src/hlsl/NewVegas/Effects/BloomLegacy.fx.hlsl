// BloomLegacy: a soft bloom on the tonemapped picture (the older, Oblivion Reloaded style). Four passes in one technique:
// 1 the bright pass (no clamp, so negative values take part in the blur on purpose), 2 a horizontal and 3 a vertical blur,
// 4 the combine: the glow is added over the picture, which is darkened where the glow is.

uniform float4 TESR_BloomLegacyData;    // x Luminance; y MiddleGray; z WhiteCutOff (the category for the time of day)
uniform float4 TESR_BloomLegacyValues;  // x BloomIntensity; y OriginalIntensity; z BloomSaturation; w OriginalSaturation
uniform float4 TESR_ReciprocalResolution;

uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_SourceBuffer   : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

// RenderScale: the blur taps are kept inside the drawn part of a rect frame (no-op on native frames).
#include "includes/RenderRect.hlsl"

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
	VSOUT res = (VSOUT)0.0f;
	res.vertPos = px.vertPos;
	res.UVCoord = px.UVCoord;
	return res;
}

float Luma709(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// Pass 1: the bright pass. Scale to the middle grey, boost by the white cut-off, then map to (x - 5) / (10 + |x|): negative for most of the
// picture, zero at 5 and approaching 1 for very bright values.
float4 BrightPS(VSOUT px) : COLOR0
{
	const float3 c = tex2D(TESR_RenderedBuffer, px.UVCoord).rgb;
	const float scl = TESR_BloomLegacyData.y / (TESR_BloomLegacyData.x + 0.001f);
	const float cut_v = max(TESR_BloomLegacyData.z, 0.001f);   // guard: a zero cut-off would divide by zero
	const float3 v = c * scl;
	const float3 boosted_v = v * (1.0f + v / (cut_v * cut_v));
	return float4((boosted_v - 5.0f) / (10.0f + max(boosted_v, 0.0f)), 1.0f);
}

// Passes 2 and 3: a 13-tap separable blur (a Gaussian of sigma 2.1 texels, weights normalised). Taps are kept inside the drawn part.
float3 BlurLine(float2 uv, float2 direction_v)
{
	const float2 texel_v = TESR_ReciprocalResolution.xy;
	const float2 step_v = texel_v * direction_v;
	float3 sum_v = 0.0f;
	float total_f = 0.0f;
	[unroll] for (int i = -6; i <= 6; i++) {
		const float w = exp(-(float)(i * i) / 8.82f);
		sum_v += w * tex2D(TESR_RenderedBuffer, clampToRectIfOn(uv + step_v * (float)i, texel_v)).rgb;
		total_f += w;
	}
	return sum_v / total_f;
}

float4 BlurHorizontalPS(VSOUT px) : COLOR0 { return float4(BlurLine(px.UVCoord, float2(1.0f, 0.0f)), 1.0f); }
float4 BlurVerticalPS(VSOUT px) : COLOR0 { return float4(BlurLine(px.UVCoord, float2(0.0f, 1.0f)), 1.0f); }

// Pass 4: the glow (clamped to 0..1, saturation and intensity) added over the original (saturation and intensity), which is darkened
// where the glow is by max(max(original, 1) - glow, 0) per channel.
float4 CombinePS(VSOUT px) : COLOR0
{
	const float3 blurred_v = tex2D(TESR_RenderedBuffer, px.UVCoord).rgb;
	const float3 original_v = tex2D(TESR_SourceBuffer, px.UVCoord).rgb;

	const float3 glow_v = saturate(blurred_v);
	const float glow_luma = Luma709(glow_v);
	const float3 bloom_colour = lerp(float3(glow_luma, glow_luma, glow_luma), glow_v, TESR_BloomLegacyValues.z) * TESR_BloomLegacyValues.x;

	const float original_luma = Luma709(original_v);
	const float3 original_colour = lerp(float3(original_luma, original_luma, original_luma), original_v, TESR_BloomLegacyValues.w) * TESR_BloomLegacyValues.y;
	const float3 darkening_v = max(max(original_v, 1.0f) - glow_v, 0.0f);

	return float4(original_colour * darkening_v + bloom_colour, 1.0f);
}

technique BloomLegacy
{
	pass Bright
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 BrightPS( );
	}
	pass BlurHorizontal
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 BlurHorizontalPS( );
	}
	pass BlurVertical
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 BlurVerticalPS( );
	}
	pass Combine
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 CombinePS( );
	}
}
