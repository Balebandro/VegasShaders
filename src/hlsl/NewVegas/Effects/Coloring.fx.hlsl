// Coloring fullscreen shader for Oblivion/Skyrim Reloaded

float4 TESR_ColoringColorCurve;
float4 TESR_ColoringEffectGamma;
float4 TESR_ColoringData;
float4 TESR_ColoringValues;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL 2026-10-14: the LUT's own textures and constants (LUT.fx.hlsl), for the second technique, which does the coloring and
// then the LUT in one pass (ShaderManager runs it in place of the two when both are on after the tonemap). Same maths as the two.
float4 TESR_LUTData;
float4 TESR_LUTBlend;
sampler2D TESR_LUTDayBuffer      : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_LUTNightBuffer    : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_LUTInteriorBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

static const float3x3 XYZ_RGB  = {{0.412453, 0.357580, 0.180423},
								  {0.212671, 0.715160, 0.072169},
								  {0.019334, 0.119193, 0.950227}};		 
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

float GetLuminance( const float3 rgb )
{
    return dot( XYZ_RGB[1], rgb );
}

// UNOFFICIAL 2026-10-16: the S-curve, normalised so 0 and 1 stay put; k = 0 is no change (the formula is 0/0 there).
float ColoringCurve(float x, float k)
{
	float y0 = 1.0 / (1.0 + exp(k / 2.0));
	return abs(k) < 1e-4 ? x : (1.0 / (1.0 + exp(-k * (x - 0.5))) - y0) / (1.0 - 2.0 * y0);
}

float4 ColorGrade(float3 color)
{
	float3 H = 0.01;
	
	float3 lumCoeff = float3(0.2126f, 0.7152f, 0.0722f); // same as our "luma()" helper function
	float lum = saturate(dot(lumCoeff, color));
	float L = saturate(10 * (lum - 0.45));
	float3 result1 = min(2.0f * color * lum, 65504);
	float3 result2 = 1.0f - 2.0f * (1.0f - lum) * (1.0f - color);
	float3 newColor = lerp(result1, result2, L);
	color = lerp(color, newColor, TESR_ColoringValues.z);

	color = pow(abs(color), TESR_ColoringValues.w) * sign(color); // same as our "pows()" helper function
	color = lerp(H, color, TESR_ColoringData.w);

	// UNOFFICIAL 2026-10-16: a curve slider at 0 made every pixel 0/0 = NaN (the S-curve's normalisation is 1 - 2 x 0.5); its limit
	// there is no change at all, so ColoringCurve() returns the input unchanged then (the same values as before otherwise).
	float3 luminance = GetLuminance(color);
	luminance.r = ColoringCurve(luminance.r, TESR_ColoringColorCurve.y);
	luminance.g = ColoringCurve(luminance.g, TESR_ColoringColorCurve.z);
	luminance.b = ColoringCurve(luminance.b, TESR_ColoringColorCurve.w);
	luminance = pow(max(luminance, 0), 1.0 / TESR_ColoringEffectGamma.x);

	float3 luminancei = 1.0 - luminance;
	luminance = lerp(luminance, luminancei, TESR_ColoringValues.y);
	luminance.r = pow(abs(luminance.r), 1.0 / TESR_ColoringEffectGamma.y);
	luminance.g = pow(abs(luminance.g), 1.0 / TESR_ColoringEffectGamma.z);
	luminance.b = pow(abs(luminance.b), 1.0 / TESR_ColoringEffectGamma.w);
	
	color = pow(abs(color), 1.0 / TESR_ColoringData.y) * sign(color);
	float3 color2 = color;

	if (luminance.r < 0.5)
		color2.r = (2.0 * luminance.r - 1.0) * (color.r - color.r * color.r) + color.r;
	else
		color2.r = (2.0 * luminance.r - 1.0) * (sqrt(max(color.r, 0)) - color.r) + color.r;	// (UNOFFICIAL 2026-10-16: as color4 below)

	if (luminance.g < 0.5)
		color2.g = (2.0 * luminance.g - 1.0) * (color.g - color.g * color.g) + color.g;
	else
		color2.g = (2.0 * luminance.g - 1.0) * (sqrt(max(color.g, 0)) - color.g) + color.g;

	if (luminance.b < 0.5)
		color2.b = (2.0 * luminance.b - 1.0) * (color.b - color.b * color.b) + color.b;
	else
		color2.b = (2.0 * luminance.b - 1.0) * (sqrt(max(color.b, 0)) - color.b) + color.b;

	float3 color3 = lerp(color, color2, TESR_ColoringData.x);
	color3 = float3(ColoringCurve(color3.r, TESR_ColoringColorCurve.x), ColoringCurve(color3.g, TESR_ColoringColorCurve.x), ColoringCurve(color3.b, TESR_ColoringColorCurve.x));

	float r2R = 1.0 - TESR_ColoringValues.x;
	float g2R = 0.0 + TESR_ColoringValues.x;
	float b2R = 0.0 + TESR_ColoringValues.x;

	float r2G = 0.0 + TESR_ColoringValues.x;
	float g2G = (1.0 - TESR_ColoringData.z) - TESR_ColoringValues.x;
	float b2G = (0.0 + TESR_ColoringData.z) + TESR_ColoringValues.x;

	float r2B = 0.0 + TESR_ColoringValues.x;
	float g2B = (0.0 + TESR_ColoringData.z) + TESR_ColoringValues.x;
	float b2B = (1.0 - TESR_ColoringData.z) - TESR_ColoringValues.x;

	// UNOFFICIAL 2026-10-16: the mix reads the colour before it (it overwrote red, then read the new red for green and blue).
	float3 mixIn = color3;
	color3.r = (mixIn.r * r2R + mixIn.g * g2R + mixIn.b * b2R);
	color3.g = (mixIn.r * r2G + mixIn.g * g2G + mixIn.b * b2G);
	color3.b = (mixIn.r * r2B + mixIn.g * g2B + mixIn.b * b2B);

	float luminance2 = GetLuminance(color3);
	float3 color4 = color3;

	if (luminance2 < 0.5)
		color4 = (2.0 * luminance2 - 1.0) * (color3 - color3 * color3) + color3;
	else
		color4 = (2.0 * luminance2 - 1.0) * (sqrt(max(color3, 0)) - color3) + color3; // "color3" could have channels below 0

	color4 = pow(abs(color4), 1.0 / TESR_ColoringValues.w) * sign(color4);

	return float4(lerp(color, color4, TESR_ColoringData.x), 1.0f);
}

float4 ColorPass(VSOUT IN) : COLOR0
{
	return ColorGrade(tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb);
}

// UNOFFICIAL: LUT.fx.hlsl's SampleLUT and LUTPass, the latter on a colour instead of the buffer. Keep in step with LUT.fx.hlsl.
// Sample a horizontal-strip LUT texture.
// N = cell count = cell size (e.g. 16 for 256x16, 32 for 1024x32, 64 for 4096x64).
// Texture layout: N cells wide, each cell N pixels square, blue axis left-to-right.
// N below 2 means the slot has no usable LUT (missing file or not a strip): the colour passes through.
float3 SampleLUT(sampler2D lut, float3 color, float slotN)
{
	float N      = max(slotN, 2.0); // keeps the unused path finite; the result is discarded below
	float b      = color.b * (N - 1.0);
	float bCell  = floor(b);
	float bFrac  = frac(b);

	float invW = 1.0 / (N * N); // 1 / textureWidth
	float invH = 1.0 / N;       // 1 / textureHeight

	float2 uv1, uv2;
	uv1.x = (bCell       * N + color.r * (N - 1.0) + 0.5) * invW;
	uv1.y = (                  color.g * (N - 1.0) + 0.5) * invH;
	uv2.x = ((bCell + 1.0) * N + color.r * (N - 1.0) + 0.5) * invW;
	uv2.y = uv1.y;

	float3 graded = lerp(tex2D(lut, uv1).rgb, tex2D(lut, uv2).rgb, bFrac);
	return slotN >= 2.0 ? graded : color;
}

float4 LUTGrade(float3 color)
{
	float  strength  = TESR_LUTData.y;
	float  hdrCompat = TESR_LUTBlend.z; // 0=off, 1=max-channel normalization for SDR LUTs on HDR input

	// HDR compat: normalize so the brightest channel = 1.0 before sampling the LUT,
	// then restore the scale afterwards. For SDR pixels (all channels <= 1) scale=1, no change.
	float  scale    = max(max(color.r, color.g), max(color.b, 1.0));
	float3 lutInput = color / lerp(1.0, scale, hdrCompat);

	float3 dayColor      = SampleLUT(TESR_LUTDayBuffer,      lutInput, TESR_LUTData.x);
	float3 nightColor    = SampleLUT(TESR_LUTNightBuffer,    lutInput, TESR_LUTData.z);
	float3 interiorColor = SampleLUT(TESR_LUTInteriorBuffer, lutInput, TESR_LUTData.w);

	float3 exteriorColor = lerp(nightColor, dayColor, TESR_LUTBlend.x);
	float3 graded        = lerp(exteriorColor, interiorColor, TESR_LUTBlend.y);

	graded *= lerp(1.0, scale, hdrCompat); // restore HDR luminance scale if hdrCompat active

	return float4(lerp(color, graded, strength), 1.0f);
}

float4 ColorLUTPass(VSOUT IN) : COLOR0
{
	return LUTGrade(ColorGrade(tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb).rgb);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 ColorPass();
	}
}

technique ColoringLUT
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 ColorLUTPass();
	}
}