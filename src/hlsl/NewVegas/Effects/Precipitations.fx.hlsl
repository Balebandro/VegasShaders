// Precipitations: falling rain streaks laid over the view, in 14 layers on cylinders round the camera.
// Written from the spec (rw-tools/specs/Precipitations.md). Samplers keep the spec's declaration order; the C++ binds them by name.
// The spec gives the behaviour, not every formula: the cell hash, the streak shape and the refraction scale below are this shader's
// own choices, chosen to fit the spec's ranges (see the report that goes with this file).

uniform float4 TESR_RainData;		// x rain amount, y VerticalScale, z Speed, w Opacity
uniform float4 TESR_RainAspect;		// x Refraction, y Coloring, z (Bloom, not read), w SkipHiddenLayers
uniform float4 TESR_GameTime;		// z animation clock
uniform float4 TESR_SunDirection;	// direction toward the sun
uniform float4 TESR_SunColor;
uniform float4x4 TESR_ShadowCameraToLightTransformOrtho;

uniform sampler2D TESR_SourceBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_RenderedBuffer : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_DepthBuffer : register(s2) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_OrthoMapBuffer : register(s3) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_BloomBuffer : register(s4) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

#include "includes/Helpers.hlsl"
#include "includes/Depth.hlsl"

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
	VSOUT res = (VSOUT)0.0f;
	res.vertPos = px.vertPos;
	res.UVCoord = px.UVCoord;
	return res;
}

static const int RainLayers = 14;	// layers 1 to 14 at 100 x i game units along the ray

float RainHash(float2 p)
{
	return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453f);
}

// The ortho map test at a camera-relative point: visible (1) when the map's top surface is not closer to the sun than the point
// (bias 0.0001) and the point lies inside the map. Outside the map the point shows no rain (the spec's rule for this effect).
float RainSunOpen(float3 rel_v)
{
	float4 o = mul(float4(rel_v, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
	o.xyz /= (abs(o.w) < 1.0e-6f ? 1.0e-6f : o.w);
	float2 uv = float2(o.x * 0.5f + 0.5f, o.y * -0.5f + 0.5f);
	if (abs(o.x) > 1.0f || abs(o.y) > 1.0f || o.z < 0.0f || o.z > 1.0f) return 0.0f;
	float top_v = tex2Dlod(TESR_OrthoMapBuffer, float4(uv, 0.0f, 0.0f)).r;
	return step(o.z - 0.0001f, top_v);
}

float4 RainPS(VSOUT px) : COLOR0
{
	float2 uv = px.UVCoord;
	float4 src_c = tex2D(TESR_SourceBuffer, uv);
	float ray_depth = getHomogenousDepth(uv);		// distance along the ray through the pixel (game units)
	float3 dir = normalize(toWorld(uv));

	float amount_v = 0.0f;		// total drop coverage over the layers
	float rain_amount = TESR_RainData.x;
	float vertical_scale = TESR_RainData.y;
	float speed_v = TESR_RainData.z;
	float opacity_v = TESR_RainData.w;
	float clock_v = TESR_GameTime.z;

	if (rain_amount > 0.0f)
	{
		[loop] for (int i = 1; i <= RainLayers; i++)
		{
			float len_v = 100.0f * i;
			// A layer shows only where the pixel's surface is further away than the layer. SkipHiddenLayers only saves the work.
			float visible_layer = len_v < ray_depth ? 1.0f : 0.0f;
			if (TESR_RainAspect.w > 0.5f && visible_layer < 0.5f) continue;

			float3 rel_p = dir * len_v;
			float horiz_v = max(length(rel_p.xy), 1.0f);
			// Cylinder mapping: azimuth into 0 to 1 with a half-turn offset; height over horizontal distance.
			float u_v = atan2(rel_p.y, rel_p.x) / (2.0f * PI) + 0.5f;
			float v = rel_p.z / horiz_v * 0.004f * vertical_scale;

			float layer_speed = 1.0f + 0.1f * i;
			float2 grid_v = float2(64.0f, 32.0f);
			float2 g_v = float2(u_v * grid_v.x + RainHash(float2(i, 3.0f)) * 17.0f, v * grid_v.y - speed_v * clock_v * layer_speed * 4.0f);
			float2 cell_v = floor(g_v);
			float2 f = frac(g_v) - 0.5f;

			float h_v = RainHash(cell_v + float2(i * 7.0f, i * 13.0f));
			if (h_v < 0.55f) continue;		// only some cells hold a drop
			float brightness_v = RainHash(cell_v + float2(3.1f, 9.7f)) / (1.0f + 0.2f * i);

			// Drop size grows with the layer index and the rain amount; never zero (the smooth step breaks at zero).
			float edge_v = 0.05f * min((float)i, 4.0f * rain_amount) + 0.005f;
			float streak_x = smoothstep(edge_v, 0.0f, abs(f.x));
			float streak_y = smoothstep(0.5f, 0.15f, abs(f.y));
			float drop_v = streak_x * streak_y * brightness_v;

			amount_v += drop_v * RainSunOpen(rel_p) * rain_amount * visible_layer;
		}
	}
	amount_v = saturate(amount_v);

	// Everything below only matters where drops are; other pixels keep the source picture.
	if (amount_v <= 0.0f) return float4(src_c.rgb, 1.0f);

	float refr_c = TESR_RainAspect.x;
	float2 shifted_v = uv + float2(amount_v * refr_c * 0.004f, -amount_v * refr_c * 0.004f);
	float3 refr_l = linearize(max(tex2D(TESR_SourceBuffer, shifted_v).rgb, 0.0f));
	float3 bloom_l = tex2D(TESR_BloomBuffer, shifted_v).rgb;	// added as read, no conversion

	// Drop tint: mid grey toward twice the sun colour, by the square of the sun's alignment with the ray.
	float sun_align = saturate(dot(dir, normalize(TESR_SunDirection.xyz)));
	float3 sun_l = linearize(max(TESR_SunColor.rgb, 0.0f));
	float3 tint_rgb = lerp(float3(0.5f, 0.5f, 0.5f), 2.0f * sun_l, sun_align * sun_align);

	float3 drop_col = refr_l + bloom_l + tint_rgb * TESR_RainAspect.y * 0.02f;
	float3 orig_l = linearize(max(src_c.rgb, 0.0f));
	float3 out_l = lerp(orig_l, drop_col, amount_v * opacity_v);
	return float4(delinearize(max(out_l, 0.0f)), 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 RainPS( );
	}
}
