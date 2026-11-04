// Exposure: automatic brightness. The frame's linear colour is pulled toward a pivot brightness by a factor that depends on
// the measured average luma (AvgLuma's G, the animated value). The factor is the same for R, G and B, so hue is kept.
//   A = animated average luma, m = MinBrightness (pivot), M = MaxBrightness (strength, 0 to 1)
//   up   = max(ln(A / m), 0) + 1,   down = max(ln(m / A), 0) + 1,   F = up / down
//   out  = lerp(L, L / F, M)    (L = the linear colour; a screen brighter than the pivot is darkened, a darker one lifted)
// The result is delinearised again. Alpha 1. A is floored so the logarithm and the division stay finite on a black frame.

#include "Includes/Helpers.hlsl"

float4 TESR_ExposureData;	// x: MinBrightness (pivot), y: MaxBrightness (strength), z, w: adaptation speeds (AvgLuma)
// UNOFFICIAL GT7 adaptation ([Shaders.Exposure.Main] Mode 1, technique GT7 below): the weather lights for its white balance
float4 TESR_SunColor;
float4 TESR_SkyColor;
float4 TESR_SunAmbient;
float4 TESR_HDRData;		// y: Tonemapping Exposure (the setup's scene-to-display scale)
float4 TESR_SunDirection;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_AvgLumaBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

#include "Includes/GT7Adaptation.hlsl"

// The green-magenta tint of the light, from the weather's sun, sky and ambient (they change smoothly by themselves).
static const float3 envXYZ = GT7EnvironmentXYZ(TESR_SunColor.rgb, TESR_SunDirection.z, TESR_SkyColor.rgb, TESR_SunAmbient.rgb, GT7EnvWeight());
static const float2 envXY = GT7LightXY(envXYZ);
static const float envDuv = clamp(GT7XYtoDuv(envXY, clamp(GT7XYtoCCT(envXY), GT7WB_MIN_K, GT7WB_MAX_K)), -GT7WB_MAX_DUV, GT7WB_MAX_DUV);

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
    VSOUT OUT = (VSOUT) 0.0f;
    OUT.vertPos = IN.vertPos;
    OUT.UVCoord = IN.UVCoord;
    return (OUT);
}

float4 ExposurePS(VSOUT IN) : COLOR
{
    const float floorValue = 1.0e-6f;
    float average = max(tex2Dlod(TESR_AvgLumaBuffer, float4(0.5f, 0.5f, 0.0f, 0.0f)).g, floorValue);
    float pivot = max(TESR_ExposureData.x, floorValue);
    float strength = TESR_ExposureData.y;

    float up = max(log(average / pivot), 0.0f) + 1.0f;
    float down = max(log(pivot / average), 0.0f) + 1.0f;
    float factor = up / down;

    float4 colour = tex2D(TESR_RenderedBuffer, IN.UVCoord);
    float3 linearColour = linearize(colour.rgb);
    float3 corrected = lerp(linearColour, linearColour / factor, strength);
    return float4(delinearize(corrected), 1.0f);
}

// GT7 adaptation: white balance toward D65 and exposure from AvgLuma's GT7 state (technique GT7 there).
float4 ExposureGT7PS(VSOUT IN) : COLOR0
{
	float3 color = linearize(tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb);
	float4 state = tex2D(TESR_AvgLumaBuffer, float2(0.5, 0.5));

	if (state.a > 50.0) {
		// White balance toward D65 (colour temperature smoothed over time, tint from the weather lights).
		float T = 1e6 / state.a;
		color = GT7WhiteBalance(color, GT7WhiteXY(T, envDuv), GT7WBStrength());

		// Exposure: only below the light adaptation limit, as far as the short-term adaptation has got.
		color *= exp2(max(GT7LightLimit() - state.r, 0.0) * GT7AEStrength());
	}
	return delinearize(float4(max(color, 0.0), 1));
}

technique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 ExposurePS();
    }
}

// UNOFFICIAL technique 1: GT7 adaptation ([Shaders.Exposure.Main] Mode 1). D3D9 only (Exposure::ExtRender keeps technique 0).
technique GT7
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 ExposureGT7PS();
	}
}
