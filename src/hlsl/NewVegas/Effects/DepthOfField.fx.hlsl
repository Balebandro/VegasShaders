// Depth of field. Two techniques, in the order effects/DepthOfField.cpp selects them:
//   0 (unnamed, autofocus): six passes through the frame chain: Focus, Mosaic, BokehBlur, BokehBlur, PlainBlur, Combine.
//   1 DistantBlurOnly: the far-distance blur alone (one pass).
// Autofocus packing: the Mosaic pass fills the four quadrants of the drawn frame, each holding the whole picture at half
// resolution. Top left: the sharp image (blurred for the near field), top right: the same image (blurred for the far field),
// bottom left: the blur amounts (red near, green far), bottom right: the same amounts. The blur passes never read across a
// quadrant edge; the Combine pass picks sharp / near / far per pixel from the amounts.
// Depth reads use the JitterFix offset (TESR_DepthOfFieldJitter) wherever they are paired with the picture's pixel.

uniform float4 TESR_ReciprocalResolution;
uniform float4 TESR_DepthOfFieldBlur;	// x distant blur on (zero or not), y start, z end (game units), w base blur radius (texels)
uniform float4 TESR_DepthOfFieldData;	// x focus scale (BlurFallOff x enabled), y radius (texels), z diameter range (bokeh threshold), w near cut-off
uniform float4 TESR_DepthOfFieldJitter;	// xy depth jitter in UV units (zero when nothing was jittered or JitterFix is off)

uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_DepthBuffer    : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_SourceBuffer   : register(s2) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_AvgLumaBuffer  : register(s3) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

#include "includes/Depth.hlsl"	// readDepthLod, toWorld, TESR_CameraPosition, TESR_CameraData, TESR_RenderScale (rect helpers)

static const float kTiny = 0.0001f;
static const float kSkyDepth = 0.9999f;	// linear depth at the far plane (the sky)

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
	VSOUT res = (VSOUT)0.0f;
	res.vertPos = px.vertPos;
	res.UVCoord = px.UVCoord;
	return res;
}

// Explicit-LOD sample: safe in the dynamic branches below (ps_3_0 cannot take implicit gradients there). Every texture has one mip.
float4 DofTap(sampler2D s, float2 uv)
{
	return tex2Dlod(s, float4(uv, 0.0f, 0.0f));
}

float DofLuma(float3 c)
{
	return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Near cut-off attenuation of the near blur: 0.5 at the camera, 0 at half the cut-off, 1 from the cut-off on. No cut-off: 1.
float NearCutAtten(float d)
{
	float c = TESR_DepthOfFieldData.w;
	if (c <= kTiny) return 1.0f;
	float h_v = 0.5f * c;
	return 0.5f * (1.0f - smoothstep(0.0f, h_v, d)) + smoothstep(h_v, c, d);
}

// Distance blur amount (0..1) of a pixel: ramps from DistantBlurStartRange to DistantBlurEndRange, and is kept off high points
// (full at or below 5000 game units of height, none at or above 100000), so the sky is left alone.
float DistantAmount(float len_v, float2 uv)
{
	if (TESR_DepthOfFieldBlur.x == 0.0f) return 0.0f;
	float ramp_v = saturate((len_v - TESR_DepthOfFieldBlur.y) / max(TESR_DepthOfFieldBlur.z - TESR_DepthOfFieldBlur.y, 1.0f));
	float hgt = TESR_CameraPosition.z + toWorld(uv).z * len_v;
	return ramp_v * saturate((100000.0f - hgt) / 95000.0f);
}

// Quadrant helpers. local = 0..1 across the drawn frame (the uv on a native frame); quad = which quadrant (0 or 1 per axis).
float2 LocalOf(float2 uv)
{
	return uv * TESR_RenderScale.zw;
}

float2 QuadOf(float2 uv)
{
	return min(floor(LocalOf(uv) * 2.0f), 1.0f);
}

float2 QuadLocal(float2 uv)
{
	float2 l = LocalOf(uv) * 2.0f;
	return l - min(floor(l), 1.0f);
}

// The packed texture coordinate (rect uv) of local position l inside quadrant q, kept half a texel inside the quadrant.
float2 PackedUV(float2 qq, float2 l)
{
	float2 lo_v = qq * 0.5f * TESR_RenderScale.xy + 0.5f * TESR_ReciprocalResolution.xy;
	float2 hi_v = (qq + 1.0f) * 0.5f * TESR_RenderScale.xy - 0.5f * TESR_ReciprocalResolution.xy;
	return clamp((qq + l) * 0.5f * TESR_RenderScale.xy, lo_v, hi_v);
}

// Focus: the near (red) and far (green) blur amounts, from the depth at the picture's pixel and the focus distance (AvgLuma blue x H).
float4 FocusPass(VSOUT px) : COLOR0
{
	float2 uv = px.UVCoord;
	float2 duv_v = uv - TESR_DepthOfFieldJitter.xy;
	float d = readDepthLod(duv_v);

	float H = max(TESR_DepthOfFieldData.x * 1000.0f, kTiny);	// hyperfocal distance
	float f = DofTap(TESR_AvgLumaBuffer, float2(0.5f, 0.5f)).b * H;	// focus distance
	float near_limit = max(f * (H - 0.01f) / max(H + f - 0.02f, kTiny), kTiny);
	// focus at or beyond the hyperfocal distance: nothing beyond it is out of focus (no far limit)
	float far_limit = (H - f > kTiny) ? f * (H - 0.01f) / (H - f) : 1.0e6f;
	far_limit = max(far_limit, kTiny);

	float near_w = saturate(1.0f - d / near_limit) * NearCutAtten(d);
	float far_w = saturate((d - far_limit) / far_limit) * 0.8f;
	far_w *= 1.0f - step(kSkyDepth, DofTap(TESR_DepthBuffer, duv_v).x);	// the sky is not blurred by the focus
	far_w = saturate(far_w + DistantAmount(d, uv));
	return float4(near_w, far_w, 1.0f, 1.0f);
}

// Mosaic: the quadrants. Top: the sharp image, half resolution (2 x 2 box of the full-resolution pixels). Bottom: the blur amounts.
float4 MosaicPass(VSOUT px) : COLOR0
{
	float2 qq = QuadOf(px.UVCoord);
	float2 p = QuadLocal(px.UVCoord) * TESR_RenderScale.xy;
	float2 o = 0.5f * TESR_ReciprocalResolution.xy;
	float2 lo_v = float2(0.0f, 0.0f), hi_v = TESR_RenderScale.xy;
	float4 c = DofTap(TESR_SourceBuffer, clamp(p - o, lo_v, hi_v));
	float4 a = DofTap(TESR_RenderedBuffer, clamp(p - o, lo_v, hi_v));
	float4 use_amounts = float4(qq.y, qq.y, qq.y, qq.y);
	c = lerp(c, a, use_amounts);
	c += lerp(DofTap(TESR_SourceBuffer, clamp(p + float2(o.x, -o.y), lo_v, hi_v)), DofTap(TESR_RenderedBuffer, clamp(p + float2(o.x, -o.y), lo_v, hi_v)), use_amounts);
	c += lerp(DofTap(TESR_SourceBuffer, clamp(p + float2(-o.x, o.y), lo_v, hi_v)), DofTap(TESR_RenderedBuffer, clamp(p + float2(-o.x, o.y), lo_v, hi_v)), use_amounts);
	c += lerp(DofTap(TESR_SourceBuffer, clamp(p + o, lo_v, hi_v)), DofTap(TESR_RenderedBuffer, clamp(p + o, lo_v, hi_v)), use_amounts);
	return c * 0.25f;
}

// One blur pass over the packed image. Top-left pixels are blurred by the near amount (bottom left, red), top-right pixels by the
// far amount (bottom right, green). Bottom quadrants are kept as drawn. bokeh: the brightest valid tap is taken when it is above
// DiameterRange (the average otherwise); plain: the average. Taps outside the pixel's quadrant are not counted.
float4 BlurQuad(float2 uv, float radius_base, bool bokeh_v)
{
	float4 centre_v = DofTap(TESR_RenderedBuffer, uv);
	float2 qq = QuadOf(uv);
	if (qq.y > 0.5f) return centre_v;

	float2 l = QuadLocal(uv);
	float amount_v = qq.x < 0.5f ? DofTap(TESR_RenderedBuffer, PackedUV(float2(0.0f, 1.0f), l)).r
	                          : DofTap(TESR_RenderedBuffer, PackedUV(float2(1.0f, 1.0f), l)).g;
	float radius_v = radius_base * amount_v;
	if (radius_v <= 0.0f) return centre_v;

	float2 lo_v = qq * 0.5f * TESR_RenderScale.xy + 0.5f * TESR_ReciprocalResolution.xy;
	float2 hi_v = (qq + 1.0f) * 0.5f * TESR_RenderScale.xy - 0.5f * TESR_ReciprocalResolution.xy;
	float4 sum_v = 0.0f;
	float count_v = 0.0f;
	float4 best_v = centre_v;
	float best_luma = -1.0f;
	for (int k = 0; k < 12; k++)
	{
		// 12 taps on a disc: 30 degree steps, alternating between the outer (1) and inner (0.5) ring
		float a = (float)k * 0.52359878f;
		float r = fmod((float)k, 2.0f) < 0.5f ? 1.0f : 0.5f;
		float2 t = uv + float2(cos(a), sin(a)) * (r * radius_v) * TESR_ReciprocalResolution.xy;
		float valid_v = (all(t >= lo_v) && all(t <= hi_v)) ? 1.0f : 0.0f;
		float4 c = DofTap(TESR_RenderedBuffer, clamp(t, lo_v, hi_v));
		sum_v += c * valid_v;
		count_v += valid_v;
		float lum_v = DofLuma(c.rgb);
		if (valid_v > 0.5f && lum_v > best_luma)
		{
			best_luma = lum_v;
			best_v = c;
		}
	}
	if (count_v < 0.5f) return centre_v;
	if (bokeh_v && best_luma > TESR_DepthOfFieldData.z) return best_v;
	return sum_v / count_v;
}

float4 BokehPass(VSOUT px) : COLOR0
{
	return BlurQuad(px.UVCoord, TESR_DepthOfFieldBlur.w, true);
}

float4 PlainBlurPass(VSOUT px) : COLOR0
{
	return BlurQuad(px.UVCoord, TESR_DepthOfFieldData.y, false);
}

// Combine: the sharp image, the near blur where the near amount is, then the far blur where the far amount is.
float4 CombinePass(VSOUT px) : COLOR0
{
	float2 uv = px.UVCoord;
	float2 l = LocalOf(uv);
	float4 sharp_v = DofTap(TESR_SourceBuffer, uv);
	float near_amount = DofTap(TESR_RenderedBuffer, PackedUV(float2(0.0f, 1.0f), l)).r;
	float far_amount = DofTap(TESR_RenderedBuffer, PackedUV(float2(1.0f, 1.0f), l)).g;
	float3 near_colour = DofTap(TESR_RenderedBuffer, PackedUV(float2(0.0f, 0.0f), l)).rgb;
	float3 far_colour = DofTap(TESR_RenderedBuffer, PackedUV(float2(1.0f, 0.0f), l)).rgb;
	float3 c = lerp(sharp_v.rgb, near_colour, near_amount);
	c = lerp(c, far_colour, far_amount);
	return float4(c, sharp_v.a);
}

// DistantBlurOnly: the far-distance blur, four diagonal taps weighted by their depth's closeness to the centre's.
float4 DistantPass(VSOUT px) : COLOR0
{
	float2 uv = px.UVCoord;
	float2 duv_v = uv - TESR_DepthOfFieldJitter.xy;
	float4 centre_v = DofTap(TESR_RenderedBuffer, uv);
	float dc_v = readDepthLod(duv_v);
	float blur_v = DistantAmount(dc_v, uv);
	if (blur_v <= 0.0f) return centre_v;

	static const float2 k_diagonal[4] = { float2(1.0f, 1.0f), float2(-1.0f, 1.0f), float2(1.0f, -1.0f), float2(-1.0f, -1.0f) };
	float radius_v = TESR_DepthOfFieldBlur.w * (1.0f + blur_v);
	float4 sum_v = 0.0f;
	float wsum_v = 0.0f;
	for (int k = 0; k < 4; k++)
	{
		float2 t = clampToRectIfOn(uv + k_diagonal[k] * radius_v * TESR_ReciprocalResolution.xy, TESR_ReciprocalResolution.xy);
		float td_v = readDepthLod(t - TESR_DepthOfFieldJitter.xy);
		float w = 1.0f - smoothstep(0.07f, 0.22f, abs(td_v - dc_v) / max(dc_v, 1.0f));
		sum_v += DofTap(TESR_RenderedBuffer, t) * w;
		wsum_v += w;
	}
	float4 blurred_v = centre_v;
	if (wsum_v > kTiny) blurred_v = sum_v / wsum_v;
	return lerp(centre_v, blurred_v, blur_v);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader  = compile ps_3_0 FocusPass( );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader  = compile ps_3_0 MosaicPass( );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader  = compile ps_3_0 BokehPass( );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader  = compile ps_3_0 BokehPass( );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader  = compile ps_3_0 PlainBlurPass( );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader  = compile ps_3_0 CombinePass( );
	}
}

technique DistantBlurOnly
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader  = compile ps_3_0 DistantPass( );
	}
}
