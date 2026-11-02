// Lens effect (TESReloaded), D3D9 side. One unnamed technique, one pass (index 0).
// Dirt on the lens: the bloom light is spread over the screen through a fixed dirt-smudge image, so bright spots
// bleed soft light onto the smudges. It only adds light (never darkens). The picture is display-encoded sRGB:
// it is linearised before the light is added and encoded again at the end.
// Vulkan twin: Compute/Lens.frag (same maths, same push layout).

uniform float4 TESR_LensData;	// x: strength (0 = nothing added), y: bloom exponent, z: smudginess (dirt power is 3 - z)

// Samplers: s0..s3 in the order the C++ binds them. s0 is declared as before but this pass does not sample it.
uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_SourceBuffer : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_BloomBuffer : register(s2) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_LensSampler : register(s3) < string ResourceName = "Effects\dirtlens.png"; > = sampler_state {
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

// Helpers: linearize / delinearize. RenderRect: rectToScreen (the dirt image spans the whole screen; identity without RenderScale).
#include "includes/Helpers.hlsl"
#include "includes/RenderRect.hlsl"

float4 Lens(VSOUT px) : COLOR0
{
	float2 uv = px.UVCoord;
	float strength_v = TESR_LensData.x;
	float bloom_exponent = TESR_LensData.y;
	float smudginess_v = TESR_LensData.z;

	float3 source_v = tex2D(TESR_SourceBuffer, uv).rgb;

	// bloom shaping: unit-luma hue, with the luma raised to the bloom exponent (black stays black: the floor)
	float3 bloom_c = tex2D(TESR_BloomBuffer, uv).rgb;
	float bloom_luma = max(dot(bloom_c, float3(0.2126f, 0.7152f, 0.0722f)), 1.0e-6f);
	float3 shaped_bloom = (bloom_c / bloom_luma) * pow(bloom_luma, bloom_exponent);

	// dirt: one channel, raised to (3 - smudginess) so 3 covers the whole screen; sampled through the rect helper
	float dirt_v = tex2D(TESR_LensSampler, rectToScreen(uv)).r;
	dirt_v = pow(max(dirt_v, 0.0f), max(3.0f - smudginess_v, 0.0f));

	float3 added_v = dirt_v * shaped_bloom * strength_v;
	float3 linear_color = linearize(source_v) + added_v;
	return float4(delinearize(linear_color), 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 Lens( );
	}
}
