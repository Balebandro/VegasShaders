// Interior lamp shadows: darkens what the lamp shadow mask marks as shadowed, indoors only.
// Written from specs/ShadowsInteriors.md (wave 3, 2026-11-02).
// Techniques:
//   0 GenericInteriorShadows   three passes through the frame chain: blur H of TESR_PointShadowBuffer, blur V of the result
//                              (bound to TESR_RenderedBuffer), apply (scene from TESR_SourceBuffer, blurred term from RenderedBuffer)
//   1 DedicatedInteriorShadows three passes on the two scratch targets the C++ binds: blur H and blur V read NVR_InteriorShadowIn
//                              (the mask, then scratch A), apply reads the untouched scene (RenderedBuffer) and the blurred term
//                              (NVR_InteriorShadowIn = scratch B, or the raw mask in skip-blur mode).

float4 TESR_ShadowData;			// y: darkness (Darkness setting)
float4 TESR_ShadowFade;			// y: interior shadows enabled (0 or 1), w: draw distance (MAXDISTANCE, game units)
float4 TESR_ReciprocalResolution;	// 1 / width, 1 / height

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_SourceBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_PointShadowBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// Dedicated path only: the mask (blur H input), scratch A (blur V input), scratch B (apply input). Bound by the C++ on stage 4.
sampler2D NVR_InteriorShadowIn : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

struct RawVertex
{
	float4 pos : POSITION0;
	float2 uv : TEXCOORD0;
};

struct PassVertex
{
	float4 pos : POSITION;
	float2 uv : TEXCOORD0;
};

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"

PassVertex ScreenVS(RawVertex IN)
{
	PassVertex vo = (PassVertex)0.0f;
	vo.pos = IN.pos;
	vo.uv = IN.uv;
	return vo;
}

// The 12 taps (offsets -6..-1 and +1..+6 texels, in that order) and the centre weight.
static const float BlurWeight[12] = {
	0.057424882f, 0.058107773f, 0.061460144f, 0.071020611f, 0.088092873f, 0.106530916f,
	0.106530916f, 0.088092873f, 0.071020611f, 0.061460144f, 0.058107773f, 0.057424882f };
static const float BlurCentreWeight = 0.114725602f;

// Depth-aware blur of the shadow term along dir (texels). Beyond the draw distance the result is black (as the older passes left it).
// A tap counts only when its depth is within 3500 game units x (centre depth / far) of the centre's. Taps are clamped to the drawn
// part of a render-scale rect.
float BlurTerm(uniform sampler2D term, float2 uv, float2 dir)
{
	float centreDepth = readDepthLod(uv);
	if (centreDepth >= TESR_ShadowFade.w) return 0.0f;
	float centre = tex2Dlod(term, float4(uv, 0.0f, 0.0f)).r;
	float threshold = 3500.0f * (centreDepth / farZ);
	float sum = centre * BlurCentreWeight;
	float wsum = BlurCentreWeight;
	[unroll]
	for (int i = 0; i < 12; i++) {
		float offset = i < 6 ? float(i) - 6.0f : float(i) - 5.0f;
		float2 tuv = clampToRectIfOn(uv + dir * offset * TESR_ReciprocalResolution.xy, TESR_ReciprocalResolution.xy);
		float tapDepth = readDepthLod(tuv);
		if (abs(tapDepth - centreDepth) < threshold) {
			sum += BlurWeight[i] * tex2Dlod(term, float4(tuv, 0.0f, 0.0f)).r;
			wsum += BlurWeight[i];
		}
	}
	return sum / wsum;
}

// The darkening (the same for both paths): term is the blurred shadow (1 = lit, 0 = shadow).
float4 Darken(float2 uv, float4 sceneGamma, float term)
{
	// Scene colour to linear (gamma 2.2), kept above 1 for the highlights.
	float3 colour = pow(max(sceneGamma.rgb, 0.0f), 2.2f);
	float3 original = colour;

	// Uniform distance: depth (game units) times the length of the view direction.
	float dist = getHomogenousDepth(uv);

	// Fully lit beyond the draw distance; fades in from 300 units to the draw distance.
	float shadow = saturate(term);
	float fadeEnd = max(TESR_ShadowFade.w, 301.0f);
	shadow = lerp(shadow, 1.0f, saturate((dist - 300.0f) / (fadeEnd - 300.0f)));

	// Darkness from the Darkness setting (scaled Reinhard curve, speed 5); 1 (no darkening) when the interior shadows are off.
	float d = TESR_ShadowData.y;
	float curve = (5.0f * d) / (1.0f + 5.0f * d);
	float darkness = saturate(1.0f - curve * TESR_ShadowFade.y);
	shadow = saturate(lerp(darkness, 1.0f, shadow));
	float power = 1.0f - shadow;

	// Darken the colour, keeping some of its hue in the shadow.
	colour = saturate(shadow * pow(max(colour, 0.0f), 1.0f - power * 0.2f));
	float l = dot(colour, float3(0.2126f, 0.7152f, 0.0722f));
	colour = max(lerp(float3(l, l, l), colour, 1.0f + power * 0.3f), 0.0f);

	// Highlights above 1 in the original come back.
	colour += max(original - 1.0f, 0.0f);

	return float4(pow(max(colour, 0.0f), 1.0f / 2.2f), 1.0f);
}

// Generic technique passes.
float4 GenericBlurH(PassVertex IN) : COLOR0
{
	float v = BlurTerm(TESR_PointShadowBuffer, IN.uv, float2(1.0f, 0.0f));
	return float4(v, 0.0f, 0.0f, 1.0f);
}

float4 GenericBlurV(PassVertex IN) : COLOR0
{
	float v = BlurTerm(TESR_RenderedBuffer, IN.uv, float2(0.0f, 1.0f));
	return float4(v, 0.0f, 0.0f, 1.0f);
}

float4 GenericApply(PassVertex IN) : COLOR0
{
	float term = tex2Dlod(TESR_RenderedBuffer, float4(IN.uv, 0.0f, 0.0f)).r;
	float4 scene = tex2Dlod(TESR_SourceBuffer, float4(IN.uv, 0.0f, 0.0f));
	return Darken(IN.uv, scene, term);
}

// Dedicated technique passes.
float4 DedicatedBlurH(PassVertex IN) : COLOR0
{
	float v = BlurTerm(NVR_InteriorShadowIn, IN.uv, float2(1.0f, 0.0f));
	return float4(v, 0.0f, 0.0f, 1.0f);
}

float4 DedicatedBlurV(PassVertex IN) : COLOR0
{
	float v = BlurTerm(NVR_InteriorShadowIn, IN.uv, float2(0.0f, 1.0f));
	return float4(v, 0.0f, 0.0f, 1.0f);
}

float4 DedicatedApply(PassVertex IN) : COLOR0
{
	float term = tex2Dlod(NVR_InteriorShadowIn, float4(IN.uv, 0.0f, 0.0f)).r;
	float4 scene = tex2Dlod(TESR_RenderedBuffer, float4(IN.uv, 0.0f, 0.0f));
	return Darken(IN.uv, scene, term);
}

technique GenericInteriorShadows
{
	pass BlurH
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 GenericBlurH();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass BlurV
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 GenericBlurV();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass Apply
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 GenericApply();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}

technique DedicatedInteriorShadows
{
	pass BlurH
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 DedicatedBlurH();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass BlurV
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 DedicatedBlurV();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass Apply
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 DedicatedApply();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}
