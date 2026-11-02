// Motion blur: when the view turns, the picture is averaged with copies of itself shifted along the turn on screen. Two passes
// of the same blur (technique 0); the second reads the first's result. The taps run 12 steps out along the turn vector, each step
// as a mirrored pair, with a weight that rises with the step (the defined look). Brightness is kept: the sum is normalised.

float4 TESR_ReciprocalResolution;	// xy: 1 / full target size
float4 TESR_MotionBlurParams;		// x gaussian weight (above zero), y blur scale, z offset max (texels per component), w unused
float4 TESR_MotionBlurData;			// x horizontal turn vector, y vertical turn vector (texels before the scale), zw unused

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

#include "Includes/RenderRect.hlsl"	// clampToRectIfOn: the taps stay in the drawn part of a render-rectangle frame

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

float4 BlurPass(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float2 texel = TESR_ReciprocalResolution.xy;
	// Each component clamped to the offset max (in texels), then scaled by one texel and the blur scale.
	float2 offset = clamp(TESR_MotionBlurData.xy, -TESR_MotionBlurParams.z, TESR_MotionBlurParams.z) * texel * TESR_MotionBlurParams.y;

	// The weight exp(i^2 / (5 g^2)) / g, shifted by the largest exponent (i = 11) so no term overflows. The shift and the 1 / g
	// are the same for every tap, so they cancel in the normalised sum. A gaussian weight of zero or less is held at 0.001.
	float g = max(TESR_MotionBlurParams.x, 0.001f);
	float4 sum = 0.0f;
	float wsum = 0.0f;
	for (int i = 0; i < 12; i++)
	{
		float fi = (float)i;
		float w = exp((fi * fi - 121.0f) / (5.0f * g * g));
		float2 stp = offset * (fi / 24.0f);
		float2 t1 = clampToRectIfOn(uv + stp, texel);
		float2 t2 = clampToRectIfOn(uv - stp, texel);
		sum += (tex2D(TESR_RenderedBuffer, t1) + tex2D(TESR_RenderedBuffer, t2)) * w;
		wsum += 2.0f * w;
	}
	return float4((sum / wsum).rgb, 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 BlurPass();
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 BlurPass();
	}
}
