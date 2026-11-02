// Debug. Two techniques (effects/Debug.cpp):
//   0 (unnamed): the overlay: buffer panels along the top, colour swatches down the right, and the light markers on the picture.
//   ClipView:    the clipping view: pixels with a channel at full brightness are marked (stripes), the rest are dimmed.
// Both are developer tools and run on D3D9 only. The overlay samplers keep the order the C++ binds them by (s0 to s8);
// the clipping view's start copy is s9, declared last.

uniform float4 TESR_SunAmbient;
uniform float4 TESR_SunColor;
uniform float4 TESR_FogColor;
uniform float4 TESR_WaterShallowColor;
uniform float4 TESR_WaterDeepColor;
uniform float4 TESR_HorizonColor;
uniform float4 TESR_SkyColor;
uniform float4 TESR_ViewSpaceLightDir;
uniform float4 TESR_SunDirection;
uniform float4 TESR_DebugVar;			// the four [Main.Develop.Main] DebugVar values (registered; not read by the overlay)
uniform float4 TESR_LightPosition[12];		// ordinary lights: xyz world position, w radius (drawn red)
uniform float4 TESR_ShadowLightPosition[12];	// shadow-casting lights, same layout (the first seven green, the rest blue)
uniform float4 TESR_ClipView;			// x: clipping view mode (1, 2 or 3)
uniform float4 TESR_ReciprocalResolution;	// xy: 1 / size

uniform sampler2D TESR_RenderedBuffer    : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_SourceBuffer      : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_DepthBuffer       : register(s2) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_NormalsBuffer     : register(s3) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D TESR_AvgLumaBuffer     : register(s4) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D TESR_PointShadowBuffer : register(s5) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D TESR_BloomBuffer       : register(s6) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D TESR_SMAA_Edges        : register(s7) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_SMAA_Blend        : register(s8) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_ClipStartBuffer   : register(s9) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

#include "includes/Depth.hlsl"	// readDepthLod, toWorld, TESR_CameraPosition, TESR_CameraData

// A channel at 254.5 / 255 or more is clipped (the 8-bit chain).
static const float kClipLevel = 254.5f / 255.0f;
// A light's marker: the view ray within about 0.00028 degrees of the light's direction (a solid marker of a few pixels).
static const float kMarkerCos = 1.0f - 4.0e-6f;

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
	VSOUT res = (VSOUT)0.0f;
	res.vertPos = px.vertPos;
	res.UVCoord = px.UVCoord;
	return res;
}

float4 DebugTap(sampler2D s, float2 uv)
{
	return tex2Dlod(s, float4(uv, 0.0f, 0.0f));
}

bool InBox(float2 uv, float2 origin_v, float2 size_v)
{
	return all(uv >= origin_v) && all(uv < origin_v + size_v);
}

float2 BoxUV(float2 uv, float2 origin_v, float2 size_v)
{
	return (uv - origin_v) / size_v;
}

// A light: a solid marker where the view ray points at it, otherwise a soft glow (radius squared over radius squared plus
// distance squared, one fifth of the colour). A radius of zero gives no glow.
void ApplyLight(inout float3 c, float4 light, float3 colour_v, float3 P, float3 cam_pos, float3 ray_n)
{
	float3 toLight = light.xyz - cam_pos;
	float dist = length(toLight);
	if (dot(ray_n, toLight / max(dist, 0.001f)) > kMarkerCos)
	{
		c = colour_v;
		return;
	}
	float d = length(P - light.xyz);
	float r = light.w;
	float glow = (r * r) / max(d * d + r * r, 1.0e-6f);
	c += colour_v * glow * 0.2f;
}

float4 OverlayPass(VSOUT px) : COLOR0
{
	float2 uv = px.UVCoord;
	float3 c = DebugTap(TESR_RenderedBuffer, uv).rgb;

	// Light markers, on the picture: shadow-casting lights first (green, then blue), ordinary lights last (red, on top).
	float eye_range = readDepthLod(uv);
	float3 cam_pos = TESR_CameraPosition.xyz;
	float3 ray_v = toWorld(uv);
	float3 ray_n = normalize(ray_v);
	float3 P = cam_pos + ray_v * eye_range;
	for (int i = 0; i < 12; i++)
	{
		float3 colour_v = (i < 7) ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
		ApplyLight(c, TESR_ShadowLightPosition[i], colour_v, P, cam_pos, ray_n);
	}
	for (int j = 0; j < 12; j++)
	{
		ApplyLight(c, TESR_LightPosition[j], float3(1.0f, 0.0f, 0.0f), P, cam_pos, ray_n);
	}

	// Buffer panels (each shows its buffer in its box).
	if (InBox(uv, float2(0.10f, 0.05f), float2(0.15f, 0.15f)))
		return float4(DebugTap(TESR_NormalsBuffer, BoxUV(uv, float2(0.10f, 0.05f), float2(0.15f, 0.15f))).rgb, 1.0f);
	if (InBox(uv, float2(0.30f, 0.05f), float2(0.15f, 0.15f)))
	{
		float linear_depth = DebugTap(TESR_DepthBuffer, BoxUV(uv, float2(0.30f, 0.05f), float2(0.15f, 0.15f))).x;
		float grey_v = pow(max(linear_depth, 0.0f), 0.3f);
		return float4(grey_v, grey_v, grey_v, 1.0f);
	}
	if (InBox(uv, float2(0.50f, 0.05f), float2(0.15f, 0.15f)))
	{
		float4 shadow_v = DebugTap(TESR_PointShadowBuffer, BoxUV(uv, float2(0.50f, 0.05f), float2(0.15f, 0.15f)));
		float grey_v = shadow_v.r * 0.5f + shadow_v.g;
		return float4(grey_v, grey_v, grey_v, 1.0f);
	}
	if (InBox(uv, float2(0.70f, 0.05f), float2(0.15f, 0.15f)))
		return float4(DebugTap(TESR_BloomBuffer, BoxUV(uv, float2(0.70f, 0.05f), float2(0.15f, 0.15f))).rgb, 1.0f);
	if (InBox(uv, float2(0.10f, 0.25f), float2(0.15f, 0.15f)))
		return float4(DebugTap(TESR_SMAA_Edges, BoxUV(uv, float2(0.10f, 0.25f), float2(0.15f, 0.15f))).rgb, 1.0f);
	if (InBox(uv, float2(0.30f, 0.25f), float2(0.15f, 0.15f)))
		return float4(DebugTap(TESR_SMAA_Blend, BoxUV(uv, float2(0.30f, 0.25f), float2(0.15f, 0.15f))).rgb, 1.0f);

	// Colour swatches down the right edge (raw values, not gamma corrected).
	if (uv.x >= 0.90f && uv.x < 0.95f)
	{
		if (uv.y >= 0.15f && uv.y < 0.20f) return float4(TESR_SunColor.rgb, 1.0f);
		if (uv.y >= 0.20f && uv.y < 0.25f) return float4(TESR_SunAmbient.rgb, 1.0f);
		if (uv.y >= 0.25f && uv.y < 0.30f) return float4(TESR_FogColor.rgb, 1.0f);
		if (uv.y >= 0.30f && uv.y < 0.35f) return float4(TESR_HorizonColor.rgb, 1.0f);
		if (uv.y >= 0.35f && uv.y < 0.40f) return float4(TESR_SkyColor.rgb, 1.0f);
		if (uv.y >= 0.40f && uv.y < 0.45f) return float4(TESR_WaterShallowColor.rgb, 1.0f);
		if (uv.y >= 0.45f && uv.y < 0.50f) return float4(TESR_WaterDeepColor.rgb, 1.0f);
		if (uv.y >= 0.55f && uv.y < 0.60f)
		{
			float avg_v = DebugTap(TESR_AvgLumaBuffer, float2(0.5f, 0.5f)).r;
			return float4(avg_v, avg_v, avg_v, 1.0f);
		}
		if (uv.y >= 0.65f && uv.y < 0.70f) return float4(TESR_ViewSpaceLightDir.xyz, 1.0f);
		if (uv.y >= 0.70f && uv.y < 0.75f) return float4(TESR_SunDirection.xyz, 1.0f);
	}
	return float4(c, 1.0f);
}

// The clipping view. Mode 1 marks the finished picture; mode 2 marks the same way, from the copy the chain started with;
// mode 3 marks red where the copy is already clipped (the tonemapper) and yellow where only the final picture is.
// Magenta: one or two channels clipped, cyan: all three. Marked pixels take a diagonal stripe (full colour, then 60 percent).
float4 ClipPass(VSOUT px) : COLOR0
{
	float2 uv = px.UVCoord;
	float3 base_col = DebugTap(TESR_RenderedBuffer, uv).rgb;
	float mode_v = TESR_ClipView.x;

	float2 pixel_v = floor(uv / TESR_ReciprocalResolution.xy);
	float stripe_v = fmod(pixel_v.x + pixel_v.y, 8.0f) < 4.0f ? 1.0f : 0.6f;

	float luma_v = dot(base_col, float3(0.2126f, 0.7152f, 0.0722f));
	float3 dimmed_v = lerp(float3(luma_v, luma_v, luma_v), base_col, 0.25f) * 0.4f;

	if (mode_v > 2.5f)
	{
		float3 start_v = DebugTap(TESR_ClipStartBuffer, uv).rgb;
		bool start_clipped = any(start_v >= kClipLevel);
		bool final_clipped = any(base_col >= kClipLevel);
		if (start_clipped) return float4(float3(1.0f, 0.0f, 0.0f) * stripe_v, 1.0f);
		if (final_clipped) return float4(float3(1.0f, 1.0f, 0.0f) * stripe_v, 1.0f);
		return float4(dimmed_v, 1.0f);
	}

	float3 test_v = base_col;
	if (mode_v > 1.5f) test_v = DebugTap(TESR_ClipStartBuffer, uv).rgb;
	float3 clipped_v = step(kClipLevel, test_v);
	float count_v = clipped_v.r + clipped_v.g + clipped_v.b;
	if (count_v < 0.5f) return float4(dimmed_v, 1.0f);
	if (count_v > 2.5f) return float4(float3(0.0f, 1.0f, 1.0f) * stripe_v, 1.0f);
	return float4(float3(1.0f, 0.0f, 1.0f) * stripe_v, 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader  = compile ps_3_0 OverlayPass( );
	}
}

technique ClipView
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader  = compile ps_3_0 ClipPass( );
	}
}
