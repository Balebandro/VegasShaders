// Exposure: automatic brightness. The frame's linear colour is pulled toward a pivot brightness by a factor that depends on
// the measured average luma (AvgLuma's G, the animated value). The factor is the same for R, G and B, so hue is kept.
//   A = animated average luma, m = MinBrightness (pivot), M = MaxBrightness (strength, 0 to 1)
//   up   = max(ln(A / m), 0) + 1,   down = max(ln(m / A), 0) + 1,   F = up / down
//   out  = lerp(L, L / F, M)    (L = the linear colour; a screen brighter than the pivot is darkened, a darker one lifted)
// The result is delinearised again. Alpha 1. A is floored so the logarithm and the division stay finite on a black frame.

#include "Includes/Helpers.hlsl"

float4 TESR_ExposureData;	// x: MinBrightness (pivot), y: MaxBrightness (strength), z, w: adaptation speeds (AvgLuma)

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_AvgLumaBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

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

technique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 ExposurePS();
    }
}
