// Lamps 6 to 11 for the point-shadow buffer, for a PointShadows shader without the MergedPointShadows technique (wave 3, written from
// specs/PointShadows2.md, 2026-11-02). Adds the light of lamps 6 to 11 at each pixel to the buffer's value before this pass, clamped.
// Lamp 11 has no cube map and no colour weight. Sky and zero-length vectors give 0 (no NaN).
// OPEN (not fixed here, see the spec): this pass reads TESR_PointShadowBuffer while it writes the same surface. The Vulkan path copies the
// buffer first; the D3D9 path cannot, without a copy made by the C++ (not changed in wave 3). The pass is only reached with the
// non-merged PointShadows shader, which the shipped one is not.

float4 TESR_ShadowFade;				// z: point-light shadows on (1) or off (0)
float4 TESR_ShadowLightPosition[12];	// xyz lamp position, w radius (0 = empty)
float4 TESR_LightColor[24];			// entries 6 to 10: rgb colour, w dimmer

sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_PointShadowBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer6 : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer7 : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer8 : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer9 : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer10 : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

struct P2VertexIn
{
	float4 pos : POSITION0;
	float2 uv : TEXCOORD0;
};

struct P2VertexOut
{
	float4 pos : POSITION;
	float2 uv : TEXCOORD0;
};

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"

P2VertexOut ScreenVS(P2VertexIn IN)
{
	P2VertexOut o = (P2VertexOut)0.0f;
	o.pos = IN.pos;
	o.uv = IN.uv;
	return o;
}

float LumaOfLampColour(float3 c)
{
	return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// One cube-mapped lamp: its amount (falloff, facing and shadow test) times luma x dimmer. Empty (w 0, or NaN) gives 0.
float ShadowedLampShare(uniform samplerCUBE cube, float3 pos, float3 n, float4 lp, float4 col)
{
	if (!(lp.w > 0.0f)) return 0.0f;
	float3 toLamp = lp.xyz - pos;
	float len = length(toLamp);
	float d = len / lp.w;
	float lit = 1.0f;
	if (TESR_ShadowFade.z > 0.5f) {
		float3 dir = float3(-toLamp.x, -toLamp.y, toLamp.z);	// the cube map's read direction
		float stored = texCUBElod(cube, float4(dir, 0.0f)).r;
		if (stored > 0.0f && stored < 1.0f) lit = (stored + 0.018f * d > d) ? 1.0f : 0.0f;
	}
	float s = saturate(d * d);
	float falloff = (1.0f - s) * (1.0f - s) / (1.0f + 5.0f * s);
	float facing = dot(toLamp / max(len, 1e-6f), n);
	facing = lerp(1.0f, facing, smoothstep(0.0f, 0.2f, d));
	float amount = saturate(saturate(facing) * falloff * lit);
	return amount * LumaOfLampColour(col.rgb) * col.w;
}

// Lamp 11: no shadow test and no colour weight; the falloff and the facing term only.
float UnshadowedLampShare(float3 pos, float3 n, float4 lp)
{
	if (!(lp.w > 0.0f)) return 0.0f;
	float3 toLamp = lp.xyz - pos;
	float len = length(toLamp);
	float d = len / lp.w;
	float s = saturate(d * d);
	float falloff = (1.0f - s) * (1.0f - s) / (1.0f + 5.0f * s);
	float facing = dot(toLamp / max(len, 1e-6f), n);
	facing = lerp(1.0f, facing, smoothstep(0.0f, 0.2f, d));
	return saturate(saturate(facing) * falloff);
}

float4 LampsPS(P2VertexOut IN) : COLOR0
{
	float2 uv = IN.uv;
	float depth = readDepth(uv);
	float3 pos = TESR_CameraPosition.xyz + toWorld(uv) * depth;
	float3 n = GetWorldNormal(uv);	// the normal rotated by the view matrix, not normalised (as the engine's helper does)

	float total = 0.0f;
	total += ShadowedLampShare(TESR_ShadowCubeMapBuffer6, pos, n, TESR_ShadowLightPosition[6], TESR_LightColor[6]);
	total += ShadowedLampShare(TESR_ShadowCubeMapBuffer7, pos, n, TESR_ShadowLightPosition[7], TESR_LightColor[7]);
	total += ShadowedLampShare(TESR_ShadowCubeMapBuffer8, pos, n, TESR_ShadowLightPosition[8], TESR_LightColor[8]);
	total += ShadowedLampShare(TESR_ShadowCubeMapBuffer9, pos, n, TESR_ShadowLightPosition[9], TESR_LightColor[9]);
	total += ShadowedLampShare(TESR_ShadowCubeMapBuffer10, pos, n, TESR_ShadowLightPosition[10], TESR_LightColor[10]);
	total += UnshadowedLampShare(pos, n, TESR_ShadowLightPosition[11]);

	float prev = tex2D(TESR_PointShadowBuffer, uv).r;
	float v = saturate(prev + total);
	return float4(v, v, v, 1.0f);
}

technique LampsSixToEleven
{
	pass P0
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 LampsPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}
