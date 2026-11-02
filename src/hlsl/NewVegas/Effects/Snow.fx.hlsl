// Snow: drifting snowflakes in 7 layers on cylinders round the camera, lit flakes tinted toward the sun colour.
// Written from the spec (rw-tools/specs/Snow.md). Samplers keep the spec's declaration order; the C++ binds them by name.
// The spec gives the behaviour, not every formula: the cell hash, the flake shape and the sway amplitude are this shader's choices.

uniform float4 TESR_SnowData;		// x snow amount, z fall speed (Speed)
uniform float4 TESR_GameTime;		// z animation clock
uniform float4 TESR_SunColor;
uniform float4 TESR_SunAmbient;
uniform float4x4 TESR_ShadowCameraToLightTransformOrtho;

uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_DepthBuffer : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_OrthoMapBuffer : register(s2) = sampler_state {
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

static const int SnowLayers = 7;	// layers 1 to 7 at 50 x i game units along the ray

// Fixed 3 by 3 mixing: a pseudo-random value in 0 to 1 for a grid cell.
float SnowHash(float2 p)
{
	float3 qq = frac(float3(p.xyx) * float3(0.1031f, 0.1030f, 0.0973f));
	qq += dot(qq, qq.yzx + 33.33f);
	return frac((qq.x + qq.y) * qq.z);
}

// Ortho map test at a camera-relative point: visible when the map's top is not closer to the sun than the point (bias 0.0001), or
// when the point lies outside the map (outside counts as open for snow).
float SnowSunOpen(float3 rel_v)
{
	float4 o = mul(float4(rel_v, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
	o.xyz /= (abs(o.w) < 1.0e-6f ? 1.0e-6f : o.w);
	if (abs(o.x) > 1.0f || abs(o.y) > 1.0f || o.z < 0.0f || o.z > 1.0f) return 1.0f;
	float2 uv = float2(o.x * 0.5f + 0.5f, o.y * -0.5f + 0.5f);
	float top_v = tex2Dlod(TESR_OrthoMapBuffer, float4(uv, 0.0f, 0.0f)).r;
	return step(o.z - 0.0001f, top_v);
}

// The snow tint: the larger of the linearised sun ambient and sun colour, per channel.
float4 SnowPS(VSOUT px) : COLOR0
{
	float2 uv = px.UVCoord;
	float4 src_c = tex2D(TESR_RenderedBuffer, uv);
	float ray_depth = getHomogenousDepth(uv);
	float3 dir = normalize(toWorld(uv));
	float clock_v = TESR_GameTime.z;
	float snow_amount = TESR_SnowData.x;
	float speed_v = TESR_SnowData.z;

	float flakes_v = 0.0f;
	if (snow_amount > 0.0f)
	{
		[loop] for (int i = 1; i <= SnowLayers; i++)
		{
			float len_v = 50.0f * i;
			if (len_v >= ray_depth) continue;		// closer geometry hides the layer

			float3 rel_p = dir * len_v;
			float horiz_v = max(length(rel_p.xy), 1.0f);
			// Cylinder mapping: azimuth negated, divided by pi, mapped into 0 to 1; height over horizontal distance times 0.1.
			float u_v = -atan2(rel_p.y, rel_p.x) / (2.0f * PI) + 0.5f;
			float v = rel_p.z / horiz_v * 0.1f;

			// Sway: a sine of half the clock with unit amplitude, offset per layer (period about 4 pi clock units).
			float sway_f = sin(0.5f * clock_v + i * 1.3f);
			float2 grid_v = float2(40.0f, 24.0f);
			float2 g_v = float2(u_v * grid_v.x + sway_f * 0.6f + SnowHash(float2(i, 5.0f)) * 11.0f, v * grid_v.y - speed_v * clock_v * (1.0f + 0.15f * i));
			float2 cell_v = floor(g_v);
			float2 f = frac(g_v) - 0.5f;

			float h_v = SnowHash(cell_v + float2(i * 3.0f, i * 5.0f));
			if (h_v < 0.5f) continue;		// only some cells hold a flake
			// Brightness: a guarded division (the divisor is at least 1, so it never reaches zero).
			float frac_prod = frac(h_v * 7.0f) * frac(h_v * 13.0f + 0.5f);
			float brightness_v = h_v / max(1.0f + frac_prod, 1.0e-4f);

			// Flake size grows with the layer index and the snow amount; never zero.
			float edge_v = 0.05f * min((float)i, 4.0f * snow_amount) + 0.005f;
			float shape_v = smoothstep(edge_v, edge_v * 0.5f, length(f));

			flakes_v += shape_v * brightness_v * SnowSunOpen(rel_p) * snow_amount;
		}
	}
	flakes_v = saturate(flakes_v);

	if (flakes_v <= 0.0f) return float4(src_c.rgb, src_c.a);

	float3 base_l = linearize(max(src_c.rgb, 0.0f));
	float3 tint_l = max(linearize(max(TESR_SunAmbient.rgb, 0.0f)), linearize(max(TESR_SunColor.rgb, 0.0f)));
	float3 out_l = lerp(base_l, tint_l, flakes_v);
	return float4(delinearize(max(out_l, 0.0f)), src_c.a);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 SnowPS( );
	}
}
