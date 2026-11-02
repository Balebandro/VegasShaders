// UNOFFICIAL: sky dither (src/effects/SkyDither.cpp). The last colour change before the picture goes to the 8-bit screen:
// smooth gradients (the sky above all) round to 256 levels there and show bands. Adding a faint noise of about one level
// first makes neighbouring pixels round up or down in proportion to where the true value lies, so the eye sees the smooth
// gradient instead of the steps. Triangular noise (two uniform values added) spreads the rounding error evenly at every
// brightness, which plain uniform noise does not. Sky only by default; the weapon and the world are left alone.

float4 TESR_SkyDitherData;   // x: noise size (1/255 = one 8-bit step), y: 1 = sky only, z: noise frame offset

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/FrameNoise.hlsl"	// UNOFFICIAL 2026-11-01 the shared noise code (the same pattern as before)

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };
struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };
VSOUT FrameVS(VSIN IN) { VSOUT OUT = (VSOUT)0.0f; OUT.vertPos = IN.vertPos; OUT.UVCoord = IN.UVCoord; return OUT; }

float4 DitherPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float4 color = tex2D(TESR_RenderedBuffer, IN.UVCoord);
	[branch] if (TESR_SkyDitherData.y > 0.0 && tex2D(TESR_DepthBuffer, IN.UVCoord).x < 0.996) return color;

	// Two noise values from different spots of the pattern, both moved by the frame index (z); their sum is triangular.
	// (z: core/TemporalBackbone.h with SharedNoise: moves only while TAA / FSR / DLSS averages frames.)
	float noise = FrameNoiseDither(vpos, TESR_SkyDitherData.z);
	// Per channel the same noise: a grey step stays grey instead of picking up coloured speckle.
	color.rgb += noise * TESR_SkyDitherData.x;
	return color;
}

technique {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DitherPS();
	}
}
