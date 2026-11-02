// Coloring: the colour grade of the tonemapped picture (saturation, contrast, gamma, a bleach look, colour S-curves and a split tone
// between shadows and highlights). Technique 0 (Coloring) is the grade alone. Technique 1 (ColoringLUT) is the grade and then the LUT
// (the LUT effect's day, night and interior tables and constants), in the same pass.

uniform float4 TESR_ColoringColorCurve;   // x ColorCurve (whole grade); y, z, w ColorCurveR, G, B (the tone mask's curves)
uniform float4 TESR_ColoringEffectGamma;  // x EffectGamma (tone mask); y, z, w EffectGammaR, G, B
uniform float4 TESR_ColoringData;         // x Strength; y BaseGamma; z Fade; w Contrast
uniform float4 TESR_ColoringValues;       // x Saturation; y Bleach; z BleachLuma; w Linearization

// technique 1 only: the LUT effect's values (set by it for the frame)
uniform float4 TESR_LUTData;              // x day N; y LUT strength; z night N; w interior N
uniform float4 TESR_LUTBlend;             // x day/night blend (1 = day); y interior flag; z HDR compatibility

uniform sampler2D TESR_RenderedBuffer    : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_LUTDayBuffer      : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_LUTNightBuffer    : register(s2) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_LUTInteriorBuffer : register(s3) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
	VSOUT res = (VSOUT)0.0f;
	res.vertPos = px.vertPos;
	res.UVCoord = px.UVCoord;
	return res;
}

float Luma709(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// Sign-preserving power. The exponent is kept above zero so a zero setting cannot divide by zero.
float3 PowSigned(float3 c, float e) { return sign(c) * pow(abs(c), max(e, 0.01f)); }

// An S-curve through (0, 0) and (1, 1) with steepness k. Zero steepness is the identity (the normalising term would be 0 / 0).
float SCurve(float x, float k)
{
	if (abs(k) < 1e-3f) return x;
	const float s = 1.0f / (1.0f + exp(-k * (x - 0.5f)));
	const float tmp_y0 = 1.0f / (1.0f + exp(k * 0.5f));    // the curve at 0
	const float tmp_y1 = 1.0f / (1.0f + exp(-k * 0.5f));   // the curve at 1
	return (s - tmp_y0) / (tmp_y1 - tmp_y0);
}

// Overlay of a picture with a luma value (the picture is the base layer).
float3 Overlay(float3 base_col, float blend_v)
{
	const float3 dark_v = 2.0f * base_col * blend_v;
	const float3 light_v = 1.0f - 2.0f * (1.0f - base_col) * (1.0f - blend_v);
	return lerp(light_v, dark_v, step(base_col, 0.5f));
}

// The split tone: a value below the mid mask moves toward its square (darker), one above it toward its square root (brighter), by an
// amount that grows with the distance of the mask from 0.5. The sign of the value is kept.
float3 SplitTone(float3 c, float3 mask_v)
{
	const float3 t = saturate(mask_v);
	const float3 a = abs(c);
	const float3 dark_amount = saturate((0.5f - t) * 2.0f);
	const float3 bright_amount = saturate((t - 0.5f) * 2.0f);
	const float3 mag_v = lerp(lerp(a, a * a, dark_amount), sqrt(a), bright_amount);
	return sign(c) * mag_v;
}

// The grade (the ten steps of the effect, in order). Output is not clamped.
float3 Grade(float3 picture_v)
{
	const float strength_v = TESR_ColoringData.x;
	const float base_gamma = TESR_ColoringData.y;
	const float fade = saturate(TESR_ColoringData.z);
	const float contrast_v = TESR_ColoringData.w;
	const float saturation_v = TESR_ColoringValues.x;
	const float bleach_v = TESR_ColoringValues.y;
	const float bleach_luma = TESR_ColoringValues.z;
	const float linearization_v = max(TESR_ColoringValues.w, 0.01f);

	// 1. Overlay of the picture with its own luma, weighted by BleachLuma in the highlights (above a luma of about 0.45).
	const float l = Luma709(picture_v);
	const float3 overlaid_v = min(Overlay(picture_v, l), 65504.0f);
	float3 c = lerp(picture_v, overlaid_v, bleach_luma * smoothstep(0.45f, 1.0f, l));

	// 2. Linearization: a sign-preserving power.
	c = PowSigned(c, linearization_v);

	// 3. Contrast: blend toward a near-black floor (Contrast 1 leaves the picture as it is).
	c = lerp(0.01f, c, contrast_v);

	// 4. Tone mask: the luminance (the green row of the XYZ-to-RGB matrix), an S-curve per channel, the EffectGamma power,
	//    inverted and blended by Bleach, then the per-channel EffectGamma power.
	const float lum_v = max(dot(c, float3(-0.9689f, 1.8758f, 0.0415f)), 0.0f);
	const float3 curves_v = TESR_ColoringColorCurve.yzw;
	float3 mask_v = float3(SCurve(lum_v, curves_v.x), SCurve(lum_v, curves_v.y), SCurve(lum_v, curves_v.z));
	mask_v = pow(saturate(mask_v), 1.0f / max(TESR_ColoringEffectGamma.x, 0.01f));
	mask_v = lerp(mask_v, 1.0f - mask_v, bleach_v);
	mask_v = pow(saturate(mask_v), 1.0f / max(TESR_ColoringEffectGamma.yzw, 0.01f));

	// 5. Base gamma: a sign-preserving power.
	c = PowSigned(c, 1.0f / max(base_gamma, 0.01f));
	const float3 before_split = c;

	// 6. Split tone by the mask, blended over the picture by Strength.
	c = lerp(c, SplitTone(c, mask_v), strength_v);

	// 7. The whole-grade S-curve (ColorCurve), per channel.
	const float k = TESR_ColoringColorCurve.x;
	c = float3(SCurve(c.r, k), SCurve(c.g, k), SCurve(c.b, k));

	// 8. Channel mixing: Saturation pulls the channels toward their mean (negative values separate them); Fade pulls green and blue together.
	const float mean_v = (c.r + c.g + c.b) / 3.0f;
	c = lerp(c, float3(mean_v, mean_v, mean_v), saturation_v);
	const float green_blue = 0.5f * (c.g + c.b);
	c.g = lerp(c.g, green_blue, fade);
	c.b = lerp(c.b, green_blue, fade);

	// 9. A second split tone, masked by the luma of the mixed picture, then the Linearization power back.
	const float mixed_luma = Luma709(c);
	const float3 split_v = SplitTone(c, float3(mixed_luma, mixed_luma, mixed_luma));
	const float3 relinearized_v = PowSigned(split_v, 1.0f / linearization_v);

	// 10. Output: the picture from step 5, blended toward the result of step 9 by Strength.
	return lerp(before_split, relinearized_v, strength_v);
}

// The LUT grade (as LUT.fx.hlsl): strip tables, HDR compatibility, day/night and interior, then the LUT strength.
float3 LutStrip(sampler2D table_v, float cells_v, float3 c)
{
	const float b = c.b * (cells_v - 1.0f);
	const float cell_lo = floor(b);
	const float cell_hi = min(cell_lo + 1.0f, cells_v - 1.0f);
	const float blend_b = b - cell_lo;
	const float2 in_cell = float2(c.r * (cells_v - 1.0f), c.g * (cells_v - 1.0f));
	const float2 uv_lo = float2((cell_lo * cells_v + 0.5f + in_cell.x) / (cells_v * cells_v), (0.5f + in_cell.y) / cells_v);
	const float2 uv_hi = float2((cell_hi * cells_v + 0.5f + in_cell.x) / (cells_v * cells_v), (0.5f + in_cell.y) / cells_v);
	return lerp(tex2D(table_v, uv_lo).rgb, tex2D(table_v, uv_hi).rgb, blend_b);
}

float3 LutGradeWith(sampler2D table_v, float cells_v, float3 c)
{
	return (cells_v >= 2.0f) ? LutStrip(table_v, cells_v, saturate(c)) : c;
}

float3 LutGrade(float3 col_v)
{
	const bool hdr_v = TESR_LUTBlend.z > 0.5f;
	const float scl = hdr_v ? max(1.0f, max(col_v.r, max(col_v.g, col_v.b))) : 1.0f;
	const float3 lookup_v = col_v / scl;
	const float3 day_v = LutGradeWith(TESR_LUTDayBuffer, TESR_LUTData.x, lookup_v);
	const float3 night_v = LutGradeWith(TESR_LUTNightBuffer, TESR_LUTData.z, lookup_v);
	const float3 interior_v = LutGradeWith(TESR_LUTInteriorBuffer, TESR_LUTData.w, lookup_v);
	const float3 exterior_v = lerp(night_v, day_v, TESR_LUTBlend.x);
	const float3 graded_v = lerp(exterior_v, interior_v, TESR_LUTBlend.y) * scl;
	return lerp(col_v, graded_v, TESR_LUTData.y);
}

float4 ColoringPS(VSOUT px) : COLOR0
{
	return float4(Grade(tex2D(TESR_RenderedBuffer, px.UVCoord).rgb), 1.0f);
}

float4 ColoringWithLUTPS(VSOUT px) : COLOR0
{
	return float4(LutGrade(Grade(tex2D(TESR_RenderedBuffer, px.UVCoord).rgb)), 1.0f);
}

technique Coloring
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 ColoringPS( );
	}
}

technique ColoringLUT
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 ColoringWithLUTPS( );
	}
}
