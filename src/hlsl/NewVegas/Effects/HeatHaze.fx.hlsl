// UNOFFICIAL: ported from Physically-Based-Reloaded (github.com/Balebandro/Physically-Based-Reloaded, testbed branch,
// GPL-3.0, the same licence as this project). Credit its authors when shipping.

// Heat haze: the shimmer of distant ground through the hot air rising above it.
//
// Each pixel is fetched from a slightly displaced spot, the displacement an animated noise that drifts
// upward. How far it is displaced grows with distance (none nearer than StartDistance, full from
// FullDistance) and is strongest along the horizon, where the line of sight runs longest through the
// hot layer of air over the ground -- looking down at nearby ground, or up at the sky, there is none.
// How hot it is now (time of day and weather, HeatHazeEffect::UpdateConstants) scales all of it.
//
// The noise is laid out by view direction, not screen position, so turning the camera turns past it
// like the world rather than dragging it along like a smeared pane of glass. A displaced fetch that
// would land on something near (a rock, a character, the weapon) is refused, so nothing near is ever
// pulled into the shimmer of what is behind it.

uniform float4 TESR_ReciprocalResolution;   // x: 1 / width, y: 1 / height, z: width / height
uniform float4 TESR_HeatHazeData;           // x: offset at full haze (fraction of the screen height), y: start distance, z: full distance, w: horizon band
uniform float4 TESR_HeatHazeNoise;          // x: noise scale, y: speed, z: time (s), w: heat 0-1

uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_DepthBuffer : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

#include "includes/Depth.hlsl"

static const float PI = 3.14159265f;

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
	VSOUT res = (VSOUT)0.0f;
	res.vertPos = px.vertPos;
	res.UVCoord = px.UVCoord;
	return res;
}

float hash(float2 p)
{
	return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453f);
}

// Value noise that wraps every `period` cells in x, so the full turn around the camera has no seam.
float valueNoise(float2 p, float period_v, float seed_v)
{
	float2 i = floor(p);
	float2 f = frac(p);
	float2 u_v = f * f * (3.0f - 2.0f * f);
	float tmp_x0 = fmod(i.x, period_v);
	float tmp_x1 = fmod(i.x + 1.0f, period_v);
	float a = hash(float2(tmp_x0, i.y) + seed_v);
	float b = hash(float2(tmp_x1, i.y) + seed_v);
	float c = hash(float2(tmp_x0, i.y + 1.0f) + seed_v);
	float d = hash(float2(tmp_x1, i.y + 1.0f) + seed_v);
	return lerp(lerp(a, b, u_v.x), lerp(c, d, u_v.x), u_v.y);
}

// Two octaves, -1 to 1.
float hazeNoise(float2 p, float period_v, float seed_v)
{
	float n = valueNoise(p, period_v, seed_v) * 0.65f + valueNoise(p * 2.0f, period_v * 2.0f, seed_v + 17.0f) * 0.35f;
	return n * 2.0f - 1.0f;
}

float4 HeatHazePS(VSOUT px) : COLOR0
{
	float2 uv = px.UVCoord;
	float3 color = tex2D(TESR_RenderedBuffer, uv).rgb;

	float start_distance = TESR_HeatHazeData.y;
	float full_distance = TESR_HeatHazeData.z;

	float eye_range = readDepth(uv);
	float distance_factor = smoothstep(start_distance, full_distance, eye_range);
	[branch] if (distance_factor <= 0.0f) return float4(color, 1.0f);

	// Strongest along the horizon, fading above and below it.
	float3 dir = normalize(toWorld(uv));
	float horizon_v = 1.0f - smoothstep(0.0f, TESR_HeatHazeData.w, abs(dir.z));
	float amount_v = TESR_HeatHazeData.x * distance_factor * horizon_v;
	[branch] if (amount_v <= 1e-6f) return float4(color, 1.0f);

	// Noise coordinates from the view direction: around the camera (a whole number of cells per turn)
	// and up the view angle, drifting upward over time, the vertical stretched so the ripples come out
	// as wavering horizontal bands, like rising heat.
	float period_v = max(1.0f, floor(TESR_HeatHazeNoise.x * 2.0f * PI));
	float around_v = (atan2(dir.y, dir.x) / (2.0f * PI) + 0.5f) * period_v;
	float up_vec = dir.z * TESR_HeatHazeNoise.x * 3.0f - TESR_HeatHazeNoise.z * TESR_HeatHazeNoise.y;
	float2 p = float2(around_v, up_vec);

	// Mostly vertical waver, some sideways.
	float2 offset = float2(hazeNoise(p, period_v, 3.0f) * 0.5f, hazeNoise(p, period_v, 41.0f)) * amount_v;
	offset.x /= TESR_ReciprocalResolution.z;   // the same distance in pixels both ways

	// Refuse a fetch that lands on something near: it would smear that object into the haze.
	float2 sample_uv = uv + offset;
	if (readDepth(sample_uv) < start_distance) return float4(color, 1.0f);

	return float4(tex2D(TESR_RenderedBuffer, sample_uv).rgb, 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 HeatHazePS( );
	}
}
