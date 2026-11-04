// 2026-11-04 local exposure ([Shaders.LocalExposure], effects/LocalExposure, off by default): the brightness of each part of the
// picture, for the tone mapper. Unreal Engine 5's Local Exposure and the eye do this: in harsh desert sun the shade goes black
// and the sand next to it burns white, and from a dark room a doorway is pure white; the eye sees into both. Here, before the
// tone mapper, four small passes measure each neighbourhood's brightness:
//   0  the scene's log brightness at 1/16 resolution (16 taps per texel, each the mean of 2 x 2 pixels), decoded as the tone
//      mapper decodes it (Linearization), so the two compare like for like;
//   1  a horizontal blur of that,
//   2  a vertical blur back into TESR_LocalExposureBuffer, which the tone mapper reads (Includes/Tonemapping.hlsl localExposure);
//   3  the whole view's mean log brightness into the 1x1 TESR_LocalExposureMean.
// The tone mapper then pulls each neighbourhood toward the view's own mean (not a fixed grey: a night stays a night, only the
// differences inside the view even out): dark regions lifted, bright ones
// calmed, each by its own strength, and less where the pixel itself differs strongly from its neighbourhood (an edge: there a
// plain blur would leave a dark or bright halo round the bright sky or a lamp). A log average, so one bright light does not
// darken everything round it.

float4 TESR_ToneMapping;			// w: Linearization (how the tone mapper decodes the scene)
float4 TESR_LocalExposureSize;		// xy: one buffer texel (1 / buffer size), zw: one screen pixel (1 / screen size)
float4 TESR_LocalExposureData;		// x shadows, y highlights, z edge guard, w 1 = on (the tone mapper's); here unused
float4 TESR_LocalExposureBlur;		// x: blur radius (sigma) in buffer texels

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_LocalExposureBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_LocalExposureTemp : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

#include "Includes/Helpers.hlsl"
#include "Includes/RenderRect.hlsl"

// RenderScale v2: in a rect frame the image and this buffer (screen-shaped) are drawn only in their top-left TESR_RenderScale part,
// and TEXCOORD0 runs over that part: reads stay inside it (clampToRectIfOn). After the upscale, RenderRect::RescaleAfterUpscale
// stretches the buffer's drawn part over all of it for the tone mapper (as Bloom's top level).

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

// 0: the log2 brightness of the 16 x 16 screen pixels under this texel
float4 Measure(VSOUT IN) : COLOR0
{
	float sum = 0.0f;
	[unroll] for (int j = 0; j < 4; j++) {
		[unroll] for (int i = 0; i < 4; i++) {
			const float2 uv = clampToRectIfOn(saturate(IN.UVCoord + (float2(i, j) - 1.5f) * 4.0f * TESR_LocalExposureSize.zw), TESR_LocalExposureSize.zw);
			const float3 c = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f)).rgb;
			float y = luma(pows(max(c, 0.0f), TESR_ToneMapping.w));
			y = abs(y) < 65504.0f ? y : 0.0f;	// a pixel that is not a number counts as black (AvgLuma's guard)
			sum += log2(max(y, 1e-4f));
		}
	}
	return float4(sum / 16.0f, 0.0f, 0.0f, 1.0f);
}

// 1, 2: a Gaussian of TESR_LocalExposureBlur.x texels, 13 taps spread over it
float BlurAlong(sampler2D source, float2 uv, float2 direction)
{
	const float sigma = max(TESR_LocalExposureBlur.x, 0.5f);
	const float stepTexels = sigma / 3.0f;	// 13 taps cover +-2 sigma
	float sum = 0.0f, weights = 0.0f;
	[unroll] for (int k = -6; k <= 6; k++) {
		const float w = exp(-0.5f * (k * k) / 9.0f);
		const float2 at = clampToRectIfOn(uv + direction * (k * stepTexels) * TESR_LocalExposureSize.xy, TESR_LocalExposureSize.xy);
		sum += w * tex2Dlod(source, float4(at, 0.0f, 0.0f)).r;
		weights += w;
	}
	return sum / weights;
}

float4 BlurH(VSOUT IN) : COLOR0 { return float4(BlurAlong(TESR_LocalExposureBuffer, IN.UVCoord, float2(1.0f, 0.0f)), 0.0f, 0.0f, 1.0f); }
float4 BlurV(VSOUT IN) : COLOR0 { return float4(BlurAlong(TESR_LocalExposureTemp, IN.UVCoord, float2(0.0f, 1.0f)), 0.0f, 0.0f, 1.0f); }

// 3: the mean of the buffer (an 8 x 8 grid of bilinear reads over the drawn part) into the 1x1 TESR_LocalExposureMean
float4 Mean(VSOUT IN) : COLOR0
{
	float sum = 0.0f;
	[unroll] for (int j = 0; j < 8; j++) {
		[unroll] for (int i = 0; i < 8; i++)
			sum += tex2Dlod(TESR_LocalExposureBuffer, float4(screenToRect((float2(i, j) + 0.5f) / 8.0f), 0.0f, 0.0f)).r;
	}
	return float4(sum / 64.0f, 0.0f, 0.0f, 1.0f);
}

technique	// 0: measure into TESR_LocalExposureBuffer
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Measure();
	}
}

technique	// 1: TESR_LocalExposureBuffer -> TESR_LocalExposureTemp, horizontal
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BlurH();
	}
}

technique	// 2: TESR_LocalExposureTemp -> TESR_LocalExposureBuffer, vertical
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BlurV();
	}
}

technique	// 3: TESR_LocalExposureBuffer -> TESR_LocalExposureMean (1x1)
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Mean();
	}
}
