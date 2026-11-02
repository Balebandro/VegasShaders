// LowHF effect (TESReloaded), D3D9 side. One unnamed technique, one pass (index 0).
// Low health and low stamina warning: the picture loses colour, gets a soft blur and darkens at the edges (health only).
// The C++ (LowHF.cpp) only draws when x is not zero. Vulkan twin: Compute/LowHF.frag (same maths, same push layout).

uniform float4 TESR_LowHFData;	// x: desaturation (0 = none, 1 = grey), y: blur offset (texture units), z: vignette exponent, w: vignette floor

uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
	VSOUT res = (VSOUT)0.0f;
	res.vertPos = px.vertPos;
	res.UVCoord = px.UVCoord;
	return res;
}

float4 LowHF(VSOUT px) : COLOR0
{
	float2 uv = px.UVCoord;
	float desaturation_v = TESR_LowHFData.x;
	float blur_v = TESR_LowHFData.y;
	float vignette_exponent = TESR_LowHFData.z;
	float vignette_floor = TESR_LowHFData.w;

	// 1. blur: a cross of taps (centre, one and two offsets along each axis), so the blur is the same in every direction
	float3 color = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f)).rgb;
	if (blur_v > 0.0f)
	{
		float3 sum_v = color * 0.2f;
		sum_v += tex2Dlod(TESR_RenderedBuffer, float4(uv + float2(blur_v, 0.0f), 0.0f, 0.0f)).rgb * 0.15f;
		sum_v += tex2Dlod(TESR_RenderedBuffer, float4(uv - float2(blur_v, 0.0f), 0.0f, 0.0f)).rgb * 0.15f;
		sum_v += tex2Dlod(TESR_RenderedBuffer, float4(uv + float2(0.0f, blur_v), 0.0f, 0.0f)).rgb * 0.15f;
		sum_v += tex2Dlod(TESR_RenderedBuffer, float4(uv - float2(0.0f, blur_v), 0.0f, 0.0f)).rgb * 0.15f;
		sum_v += tex2Dlod(TESR_RenderedBuffer, float4(uv + float2(2.0f * blur_v, 0.0f), 0.0f, 0.0f)).rgb * 0.05f;
		sum_v += tex2Dlod(TESR_RenderedBuffer, float4(uv - float2(2.0f * blur_v, 0.0f), 0.0f, 0.0f)).rgb * 0.05f;
		sum_v += tex2Dlod(TESR_RenderedBuffer, float4(uv + float2(0.0f, 2.0f * blur_v), 0.0f, 0.0f)).rgb * 0.05f;
		sum_v += tex2Dlod(TESR_RenderedBuffer, float4(uv - float2(0.0f, 2.0f * blur_v), 0.0f, 0.0f)).rgb * 0.05f;
		color = sum_v;
	}

	// 2. desaturation toward the luma (Rec.709-like weights as the original), unclamped
	if (desaturation_v > 0.0f)
	{
		float luma_v = dot(color, float3(0.2125f, 0.7154f, 0.0721f));
		color = lerp(color, float3(luma_v, luma_v, luma_v), desaturation_v);
	}

	// 3. vignette: centre bright, edges darkened; the floor keeps part of the edge brightness; clamped to 0..1
	if (vignette_exponent > 0.0f)
	{
		float2 d = uv - 0.5f;
		float factor_v = 1.0f - dot(d, d);	// about 1 at the centre, about 0.5 at the corners
		float multiplier_v = saturate(pow(abs(factor_v), abs(vignette_exponent)) + vignette_floor);
		color *= multiplier_v;
	}

	return float4(color, 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 LowHF( );
	}
}
