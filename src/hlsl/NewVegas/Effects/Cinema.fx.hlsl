// Cinema: letterbox bars, a colour split at the edges, film grain (its frame is game time x 25, see the spec), a self-overlay,
// and a vignette. Technique 1 (CinemaSkyDither) adds the sky dither to the same pass.
// Order on each pixel: letterbox (blacks the bars where they are nearer than LetterBoxDepth), colour split, self-overlay,
// grain, vignette. The sky dither is added last, to the sky pixels only when SkyDither asks for it.

uniform float4 TESR_ReciprocalResolution;	// xy: 1 / size, z: width / height of the target
uniform float4 TESR_GameTime;				// z: game time in seconds
uniform float4 TESR_CinemaData;				// x: wanted aspect (1 = none), y: vignette radius, z: vignette darkness, w: overlay strength
uniform float4 TESR_CinemaSettings;			// x: not used (the dirt lens is not wired), y: grain amount (x 0.01), z: colour split in pixels at the edge, w: letterbox depth
uniform float4 TESR_SkyDitherData;			// technique 1: x dither step, y 1 for sky only, z frame offset

uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_DepthBuffer    : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_BlueNoiseSampler : register(s2) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state {
    ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

#include "includes/Depth.hlsl"	// readDepthLod, projectedDepthFromLinear (the raw depth the sky test compares)

static const float kRawSkyLimit = 0.996f;	// raw depth below this is not sky

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
	VSOUT res = (VSOUT)0.0f;
	res.vertPos = px.vertPos;
	res.UVCoord = px.UVCoord;
	return res;
}

float OverlayChannel(float base_col, float blend_v)
{
	return base_col < 0.5f ? 2.0f * base_col * blend_v : 1.0f - 2.0f * (1.0f - base_col) * (1.0f - blend_v);
}

float3 OverlayColour(float3 base_col, float3 blend_v)
{
	return float3(OverlayChannel(base_col.r, blend_v.r), OverlayChannel(base_col.g, blend_v.g), OverlayChannel(base_col.b, blend_v.b));
}

// Interleaved gradient noise, 0..1.
float InterleavedGradient(float2 p)
{
	return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

float4 CinemaCore(float2 uv, bool sky_dither)
{
	float2 pixel_v = floor(uv / TESR_ReciprocalResolution.xy);

	// Letterbox: the bars are the part of the height the wanted ratio does not use, split evenly top and bottom.
	// Only pixels nearer than LetterBoxDepth are kept (the depth is read only when the bars can show).
	float wanted_v = TESR_CinemaData.x;
	if (wanted_v > 0.0f && wanted_v != 1.0f)
	{
		float edge_v = 0.5f * (1.0f - TESR_ReciprocalResolution.z / wanted_v);
		if ((uv.y < edge_v || uv.y > 1.0f - edge_v) && readDepthLod(uv) > TESR_CinemaSettings.w)
			return float4(0.0f, 0.0f, 0.0f, 1.0f);
	}

	// Colour split: red pulled towards the centre, blue pushed away from it, green in place.
	float2 shift_v = (uv - 0.5f) * 2.0f * TESR_CinemaSettings.z * TESR_ReciprocalResolution.xy;
	float3 c = float3(tex2Dlod(TESR_RenderedBuffer, float4(uv - shift_v, 0.0f, 0.0f)).r,
	                  tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f)).g,
	                  tex2Dlod(TESR_RenderedBuffer, float4(uv + shift_v, 0.0f, 0.0f)).b);

	// Self-overlay (contrast and colour), mixed back by its strength.
	c = lerp(c, OverlayColour(c, c), TESR_CinemaData.w);

	// Film grain: two blue-noise samples, moved per film frame, multiplied to about 0.5 and overlaid by the grain amount.
	float frame_t = floor(TESR_GameTime.z * 25.0f);
	float2 noise_uv = (pixel_v + 0.5f) / 256.0f;
	float tmp_n1 = tex2Dlod(TESR_BlueNoiseSampler, float4(noise_uv + frame_t * float2(0.6180339f, 0.3819660f), 0.0f, 0.0f)).r;
	float tmp_n2 = tex2Dlod(TESR_BlueNoiseSampler, float4(noise_uv + frame_t * float2(0.7548777f, 0.5698403f), 0.0f, 0.0f)).g;
	float grain_v = 2.0f * tmp_n1 * tmp_n2;
	c = lerp(c, OverlayColour(c, float3(grain_v, grain_v, grain_v)), TESR_CinemaSettings.y * 0.01f);

	// Vignette: 1 up to distance 1 - VignetteRadius (edge middles are at 0.5, corners at 0.71), 0 at distance 1.
	float len_v = length(uv - 0.5f);
	float falloff_v = 1.0f - smoothstep(min(1.0f - TESR_CinemaData.y, 0.999f), 1.0f, len_v);
	c *= 1.0f - TESR_CinemaData.z * (1.0f - falloff_v);
	c = max(c, 0.0f);

	float4 result_c = float4(c, 1.0f);

	// Sky dither (technique 1): one dither step of noise on the pixels that are not skipped. Sky only: a pixel nearer than the
	// raw sky limit is left alone.
	if (sky_dither)
	{
		float raw_v = projectedDepthFromLinear(tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x);
		if (!(TESR_SkyDitherData.y > 0.5f && raw_v < kRawSkyLimit))
		{
			float2 offset = TESR_SkyDitherData.z * float2(5.588238f, 5.588238f);
			float noise_v = InterleavedGradient(pixel_v + offset) + InterleavedGradient(pixel_v.yx + offset) - 1.0f;
			result_c.rgb += TESR_SkyDitherData.x * noise_v;
		}
	}
	return result_c;
}

float4 CinemaPass(VSOUT px) : COLOR0
{
	return CinemaCore(px.UVCoord, false);
}

float4 CinemaSkyDitherPass(VSOUT px) : COLOR0
{
	return CinemaCore(px.UVCoord, true);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader  = compile ps_3_0 CinemaPass( );
	}
}

technique CinemaSkyDither
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader  = compile ps_3_0 CinemaSkyDitherPass( );
	}
}
