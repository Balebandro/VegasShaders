// Image adjust: saturation, linear-light brightness and strength, contrast, and dark / light tints.
// The day, night and interior values are blended on the CPU (ImageAdjustEffect::UpdateConstants), so this pass only sees the result.
// Order on each pixel: saturation (display space), linearise, tone-weighted tints and brightness, strength blend, delinearise,
// then contrast around mid-grey blended by strength. Defaults (brightness, saturation, contrast, strength 1, tints 1) are the identity.

float4 TESR_ImageAdjustData;	// x brightness (at least 0), y contrast, z saturation, w strength
float4 TESR_DarkAdjustColor;	// rgb tint of the darker tones (gamma encoded, 1 = none)
float4 TESR_LightAdjustColor;	// rgb tint of the lighter tones (gamma encoded, 1 = none)

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

static const float kGamma = 2.2f;

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

// Sign-preserving power: negative channels (from a tint or the saturation step) stay finite.
float3 PowSigned(float3 c, float e)
{
	return pow(abs(c), e) * sign(c);
}

float4 AdjustPass(VSOUT IN) : COLOR0
{
	float3 c = tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb;

	// Saturation: blend between the pixel's Rec. 709 grey and its colour (1 unchanged, 0 grey).
	float grey = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
	c = lerp(float3(grey, grey, grey), c, TESR_ImageAdjustData.z);

	// Linear light.
	float3 lin = PowSigned(c, kGamma);

	// Tone weights: the dark weight is high for dark colours, the light weight for bright ones; they add to 1 per channel,
	// so at tint 1 the colour comes back unchanged. Each part takes its own tint (linearised).
	float3 light = smoothstep(0.0f, 1.0f, lin);
	float3 dark = 1.0f - light;
	float3 darkTint = PowSigned(TESR_DarkAdjustColor.rgb, kGamma);
	float3 lightTint = PowSigned(TESR_LightAdjustColor.rgb, kGamma);
	float3 toned = lin * dark * darkTint + lin * light * lightTint;

	// Brightness (clamped at zero from below), then the strength blend in linear light.
	float3 bright = max(toned * TESR_ImageAdjustData.x, 0.0f);
	float3 blended = lerp(lin, bright, TESR_ImageAdjustData.w);

	// Back to display space, then contrast around mid-grey, blended by strength.
	float3 display = PowSigned(blended, 1.0f / kGamma);
	float3 contrasted = (display - 0.5f) * TESR_ImageAdjustData.y + 0.5f;
	return float4(lerp(display, contrasted, TESR_ImageAdjustData.w), 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 AdjustPass();
	}
}
