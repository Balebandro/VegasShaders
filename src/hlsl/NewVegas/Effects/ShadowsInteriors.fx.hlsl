// Image space shadows shader for Oblivion Reloaded

float4x4 TESR_WorldTransform;
float4 TESR_ShadowData;
float4 TESR_ShadowFade;
float4 TESR_ReciprocalResolution;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_PointShadowBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
static const float MAXDISTANCE = TESR_ShadowFade.w;

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

#include "Includes/BlurDepth.hlsl"


// Applies the blurred shadow term to the scene colour. Shared by both techniques below (and, UNOFFICIAL 2026-10-18
// InteriorShadowFold, by the fog's interior composite: the function lives in the include so both compile from one source).
#define INTERIOR_SHADOW_MAXDISTANCE MAXDISTANCE
#include "Includes/InteriorShadowApply.hlsl"

// combine Shadow pass and source using an overlay mode + alpha blending
float4 CombineShadow( VSOUT IN ) : COLOR0 {
	return ApplyInteriorShadow(IN.UVCoord, tex2D(TESR_SourceBuffer, IN.UVCoord), tex2D(TESR_RenderedBuffer, IN.UVCoord).r);
}

technique {

	pass
	{ 
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DepthBlur(TESR_PointShadowBuffer, OffsetMaskH, 1, 3500, MAXDISTANCE);
	}
	
	pass
	{ 
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DepthBlur(TESR_RenderedBuffer, OffsetMaskV, 1, 3500, MAXDISTANCE);
	}
	
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CombineShadow();
	}
	
}


// ---- Dedicated path (ShadowsInteriorsEffect::Render) ----
// The blur passes above run on the full-resolution HDR frame through the frame chain: a copy of the
// frame for TESR_SourceBuffer, a clear of an 8 byte per pixel target before each pass, and two blurs
// of what is a single-channel mask. Here the two blurs ping-pong between the two G16R16 scratch
// targets (4 bytes per pixel, no frame copy), and only the last pass touches the HDR frame: it reads
// the untouched scene from TESR_RenderedBuffer and the blurred mask from s4, which the CPU binds.
sampler2D NVR_InteriorShadowIn : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

// The depth-aware blur of DepthBlur(), with two differences that do not change the result:
//  * pixels beyond endFade return black -- what clip() left in the cleared destination of the original
//    passes (the shadow is faded out to fully lit by that distance in ApplyInteriorShadow anyway);
//  * the twelve depth reads are skipped when every tap equals the centre, since the weighted average
//    of equal values is that value whatever the weights are.
float4 InteriorShadowBlur(VSOUT IN, uniform sampler2D buffer, uniform float2 OffsetMask, uniform float blurRadius, uniform float depthDrop, uniform float endFade) : COLOR0
{
	float4 center = tex2Dlod(buffer, float4(IN.UVCoord, 0, 0));
	float depth1 = readDepthLod(IN.UVCoord);
	[branch] if (depth1 > endFade) return float4(0, 0, 0, 1);

	float4 taps[cKernelSize];
	float4 spread = 0;
	[unroll]
	for (int t = 0; t < cKernelSize; t++) {
		// UNOFFICIAL 2026-10-27 RenderScale v2: taps kept in the drawn part (wide radius at the right / bottom edge)
		taps[t] = tex2Dlod(buffer, float4(clampToRectIfOn(IN.UVCoord + (BlurOffsets[t] * OffsetMask) * blurRadius, TESR_ReciprocalResolution.xy), 0, 0));
		spread = max(spread, abs(taps[t] - center));
	}
	[branch] if (max(spread.r, spread.g) < 0.0001f) return float4(center.rgb, 1);

	float WeightSum = 0.114725602f;
	float4 color1 = center * WeightSum;
	depthDrop *= (depth1 / farZ);

	[unroll]
	for (int i = 0; i < cKernelSize; i++)
	{
		float depth2 = readDepthLod(clampToRectIfOn(IN.UVCoord + (BlurOffsets[i] * OffsetMask) * blurRadius, TESR_ReciprocalResolution.xy));
		float diff = abs(float(depth1 - depth2));

		int useForBlur = (diff <= depthDrop);
		color1 += BlurWeights[i] * taps[i] * useForBlur;
		WeightSum += BlurWeights[i] * useForBlur;
	}
	color1 /= WeightSum;
	return float4(color1.rgb, 1);
}

float4 CombineShadowDedicated( VSOUT IN ) : COLOR0 {
	return ApplyInteriorShadow(IN.UVCoord, tex2D(TESR_RenderedBuffer, IN.UVCoord), tex2D(NVR_InteriorShadowIn, IN.UVCoord).r);
}

technique DedicatedInteriorShadows {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 InteriorShadowBlur(NVR_InteriorShadowIn, OffsetMaskH, 1, 3500, MAXDISTANCE);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 InteriorShadowBlur(NVR_InteriorShadowIn, OffsetMaskV, 1, 3500, MAXDISTANCE);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CombineShadowDedicated();
	}
}
