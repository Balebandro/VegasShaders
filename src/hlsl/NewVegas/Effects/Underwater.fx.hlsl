// Underwater effect (TESReloaded), D3D9 side.
// Five passes in one unnamed technique (index 0), as src/effects/Underwater.cpp expects:
//   pass 0  god rays        into the upper-left quarter of the frame (the ray image)
//   pass 1  blur, vertical  the quarter only
//   pass 2  blur, horizontal the quarter only
//   pass 3  water           the whole frame: murk, caustics, absorption, god-ray add, encode and dither
//   pass 4  wobble          the whole frame, last (it may write straight into the game target)
// Above the water every pass returns the source frame (the effect is then a no-op there).
// Vulkan twin: Compute/UnderwaterGodrays.frag, UnderwaterBlur.frag, UnderwaterWater.frag, UnderwaterDistortion.frag
// and the shared Compute/Underwater.glsl. Keep the two in step.

uniform float4 TESR_ReciprocalResolution;
uniform float4 TESR_SunDirection;	// xyz: unit vector to the sun (z up)
uniform float4 TESR_SunColor;		// rgb: sun colour (game values, linearised here)
uniform float4 TESR_GameTime;		// x: game clock (ms), z: frame value used by the wobble
uniform float4 TESR_WaterCoefficients;	// xyz: extinction per channel, w: scattering amount
uniform float4 TESR_WaterFog;		// x: near, y: far distance of the underwater fog
uniform float4 TESR_WaterVolume;	// x: caustic strength, z: turbidity, w: god-ray strength
uniform float4 TESR_WaveParams;		// z: wave speed
uniform float4 TESR_WaterSettings;	// x: water surface height, y: depth darkness (not read), z: underwater flag (not read)
uniform float4 TESR_WaterShallowColor;
uniform float4 TESR_WaterDeepColor;
uniform float4 TESR_HorizonColor;
uniform float4 TESR_SkyColor;

// Samplers: the register numbers are the slots the C++ binds (declaration order is not used).
uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_SourceBuffer : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_DepthBuffer : register(s2) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// Declared and loaded as before; the active passes do not read it (the C++ still binds it by name).
uniform sampler2D TESR_WavesSampler : register(s3) < string ResourceName = "Water\water_NRM.dds"; > = sampler_state {
    ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = ANISOTROPIC; MINFILTER = ANISOTROPIC; MIPFILTER = LINEAR; };
uniform sampler2D TESR_NormalsBuffer : register(s4) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = POINT; };
uniform sampler2D TESR_BlueNoiseSampler : register(s5) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state {
    ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = POINT; };
uniform sampler2D TESR_CausticsSampler : register(s6) < string ResourceName = "Water\caust_001.dds"; > = sampler_state {
    ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = ANISOTROPIC; MINFILTER = ANISOTROPIC; MIPFILTER = LINEAR; };
uniform sampler2D TESR_PointShadowBuffer : register(s7) = sampler_state {
    ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = ANISOTROPIC; MINFILTER = ANISOTROPIC; MIPFILTER = LINEAR; };

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
	VSOUT res = (VSOUT)0.0f;
	res.vertPos = px.vertPos;
	res.UVCoord = px.UVCoord;
	return res;
}

// Depth.hlsl brings TESR_DepthBuffer-based readDepth, toWorld (the view ray, not normalised: its forward
// component is 1, so the scene point is camera + toWorld(uv) * readDepth(uv)), the camera and projection
// matrices and the RenderScale helpers. It must come after the depth sampler is declared.
#include "includes/Helpers.hlsl"
#include "includes/Depth.hlsl"

// ---- helpers (mirrored in Compute/Underwater.glsl) ----

// The ray image: its pixels are the upper-left quarter of the picture (rect coordinates).
bool inQuarter(float2 uv)
{
	return all(uv >= 0.0f) && all(uv <= 0.5f * TESR_RenderScale.xy);
}

float3 sunDirection()
{
	return normalize(TESR_SunDirection.xyz);
}

// Scene point of a pixel, and whether it is above the water (the source frame is kept there).
// Camera well below the surface: everything is underwater. Camera well above: everything is above.
// In between: the pixel is above the water only when it sits above the surface band (W - 0.6).
bool abovePixel(float3 eye_v, float3 P)
{
	float W = TESR_WaterSettings.x;
	if (eye_v.z > W + 4.0f) return true;
	if (eye_v.z < W - 4.0f) return false;
	return P.z > W - 0.6f;
}

// 4x4 ordered dither: Bayer matrix from the 2x2 one, values 0..15.
float Bayer2(float a, float b)
{
	return (b < 0.5f) ? a * 2.0f : 3.0f - 2.0f * a;
}

float Bayer4(float2 p)
{
	float2 qq = fmod(floor(p), 4.0f);
	float2 lo_v = fmod(qq, 2.0f);
	float2 hi_v = floor(qq * 0.5f);
	return 4.0f * Bayer2(lo_v.x, lo_v.y) + Bayer2(hi_v.x, hi_v.y);
}

// Blur taps: Gaussian, centre weight 0.115, taps 1..6 steps out (sigma about 3.79 texels of two-texel steps).
float TapWeight(int i)
{
	if (i == 1) return 0.111075f;
	if (i == 2) return 0.100087f;
	if (i == 3) return 0.084136f;
	if (i == 4) return 0.065981f;
	if (i == 5) return 0.048273f;
	return 0.032948f;
}

// ---- pass 0: god rays into the upper-left quarter ----
// A quarter pixel at uv computes the shaft for the full-frame pixel at 2 * uv (the ray image is half resolution
// per axis, so a full-frame pixel reads it at uv * 0.5). Shafts are integrated along the view ray from the camera
// to the scene, only where the ray is under the water, using the caustic light the sun throws there.
float4 Godrays(VSOUT px, float2 vpos : VPOS) : COLOR0
{
	float2 uv = px.UVCoord;
	if (!inQuarter(uv)) return tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f));

	float2 local_p = uv * 2.0f;
	float3 eye_v = TESR_CameraPosition.xyz;
	float W = TESR_WaterSettings.x;
	float3 dir_v = toWorld(local_p);
	float view_z = readDepth(local_p);
	float len_v = length(dir_v) * view_z;
	float3 dir = dir_v / max(length(dir_v), 1.0e-6f);
	float max_t = min(len_v, 4000.0f);
	float step_len = max_t / 49.0f;

	float3 sun_rgb = sunDirection();
	float sun_z = max(sun_rgb.z, 0.05f);
	float sun_up = saturate(sun_rgb.z * 6.0f);
	float drift_v = TESR_GameTime.x * 0.001f * TESR_WaveParams.z * 0.02f;

	float acc_v = 0.0f;
	[loop] for (int i = 0; i < 49; i++)
	{
		// one blue-noise read per step: jitters the step so the shafts do not band
		float noise_v = tex2Dlod(TESR_BlueNoiseSampler, float4((vpos + float2(17.0f, 31.0f) * i) / 256.0f, 0.0f, 0.0f)).r;
		float t = ((float)i + noise_v) * step_len;
		float3 p = eye_v + dir * t;
		float under_v = step(p.z, W);	// 1 where the point is below the surface
		// the sun ray through p meets the surface at q; its caustic pattern is the light reaching p
		float3 qq = p + sun_rgb * (max(W - p.z, 0.0f) / sun_z);
		float tmp_c1 = tex2Dlod(TESR_CausticsSampler, float4(qq.xy / 600.0f + float2(drift_v, drift_v * 0.7f), 0.0f, 0.0f)).r;
		float tmp_c2 = tex2Dlod(TESR_CausticsSampler, float4(qq.xy / 720.0f - float2(drift_v * 0.8f, drift_v), 0.0f, 0.0f)).r;
		acc_v += min(tmp_c1, tmp_c2) * under_v * exp(-t * 0.0008f);
	}
	float ray_v = 2.0f * acc_v / 49.0f * sun_up;
	return float4(ray_v, 0.0f, 0.0f, 1.0f);
}

// ---- passes 1 and 2: 12-tap blur of the ray image (quarter only) ----
float4 Blur(float2 uv, float2 mask_v)
{
	float4 current_v = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f));
	if (!inQuarter(uv)) return current_v;

	float2 step_uv = mask_v * 2.0f * TESR_ReciprocalResolution.xy;	// two full-resolution texels per tap
	float4 acc_v = current_v * 0.115f;
	float wsum_v = 0.115f;
	[unroll] for (int i = 1; i <= 6; i++)
	{
		float w = TapWeight(i);
		float2 a = uv + step_uv * (float)i;
		float2 b = uv - step_uv * (float)i;
		if (inQuarter(a))
		{
			acc_v += tex2Dlod(TESR_RenderedBuffer, float4(a, 0.0f, 0.0f)) * w;
			wsum_v += w;
		}
		if (inQuarter(b))
		{
			acc_v += tex2Dlod(TESR_RenderedBuffer, float4(b, 0.0f, 0.0f)) * w;
			wsum_v += w;
		}
	}
	return acc_v / wsum_v;
}

float4 BlurVertical(VSOUT px) : COLOR0
{
	return Blur(px.UVCoord, float2(0.0f, 1.0f));
}

float4 BlurHorizontal(VSOUT px) : COLOR0
{
	return Blur(px.UVCoord, float2(1.0f, 0.0f));
}

// ---- pass 3: the water ----
float4 Water(VSOUT px, float2 vpos : VPOS) : COLOR0
{
	float2 uv = px.UVCoord;
	float4 source_v = tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0f, 0.0f));
	float3 eye_v = TESR_CameraPosition.xyz;
	float3 dir_v = toWorld(uv);
	float view_z = readDepth(uv);
	float3 P = eye_v + dir_v * view_z;
	if (abovePixel(eye_v, P)) return source_v;

	float len_v = length(dir_v) * view_z;
	float3 dir = dir_v / max(length(dir_v), 1.0e-6f);
	float W = TESR_WaterSettings.x;

	float3 scene_v = linearize(source_v.rgb);
	float3 shallow_v = linearize(TESR_WaterShallowColor.rgb);
	float3 deep_v = linearize(TESR_WaterDeepColor.rgb);
	float3 horizon_v = linearize(TESR_HorizonColor.rgb);
	float3 zenith_v = linearize(TESR_SkyColor.rgb);
	float3 sun_col = linearize(TESR_SunColor.rgb);
	float3 sun_rgb = sunDirection();

	// fog colour: deep for the lower view, shallow (plus a little sky, with a sun glow) toward the horizon
	float sun_align = saturate(dot(dir, sun_rgb));
	float3 sky_col = lerp(horizon_v, zenith_v, saturate(dir.z)) + sun_col * pow(sun_align, 12.0f) * 0.5f;
	float toward_v = saturate(dir.z * 1.5f + 0.5f);
	float3 fog_col = lerp(deep_v, shallow_v + 0.1f * sky_col, toward_v);
	float3 fog_lit = fog_col * sun_col * (0.4f + 0.6f * sun_align);

	// distance factor and murk (turbidity sets how sharp the falloff is)
	float fog_f = saturate((len_v - TESR_WaterFog.x) / max(TESR_WaterFog.y - TESR_WaterFog.x, 1.0f));
	float turb_v = max(TESR_WaterVolume.z, 0.0f);
	float murk_v = 1.0f - pow(max(1.0f - fog_f, 0.0f), max(turb_v, 0.01f));
	float turb_w = turb_v / (turb_v + 1.0f);
	float3 scatter_v = (1.0f - fog_col) * max(TESR_WaterCoefficients.w, 0.0f) * turb_w * 0.25f;
	float3 fog_out = fog_lit + scatter_v * fog_f;

	// caustics: floor that faces up and the sun, close below the surface, near the camera, and lit by the point shadow
	float3 n_v = expand(tex2Dlod(TESR_NormalsBuffer, float4(uv, 0.0f, 0.0f)).xyz);
	float3 up_v = mul(float4(0.0f, 0.0f, 1.0f, 0.0f), TESR_ViewTransform).xyz;
	float3 sun_v = mul(float4(sun_rgb, 0.0f), TESR_ViewTransform).xyz;
	float face_up = smoothstep(0.5f, 0.9f, dot(n_v, up_v));
	float face_sun = smoothstep(0.0f, 0.5f, dot(n_v, sun_v));
	float depth_below = max(W - P.z, 0.0f);
	float fade_depth = 1.0f - smoothstep(0.0f, 800.0f, depth_below);
	float fade_range = 1.0f - smoothstep(4000.0f, 6000.0f, len_v);
	float4 shadow_v = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0.0f, 0.0f));
	float sun_up = saturate(sun_rgb.z * 6.0f);
	float drift_v = TESR_GameTime.x * 0.001f * TESR_WaveParams.z * 0.02f;
	float sun_z = max(sun_rgb.z, 0.05f);
	float3 qq = P + sun_rgb * (depth_below / sun_z);
	float tmp_c1 = tex2Dlod(TESR_CausticsSampler, float4(qq.xy / 600.0f + float2(drift_v, drift_v * 0.7f), 0.0f, 0.0f)).r;
	float tmp_c2 = tex2Dlod(TESR_CausticsSampler, float4(qq.xy / 720.0f - float2(drift_v * 0.8f, drift_v), 0.0f, 0.0f)).r;
	float caustic_v = pow(max(min(tmp_c1, tmp_c2), 0.0f), 1.3f) * 4.0f;
	scene_v += caustic_v * TESR_WaterVolume.x * sun_col * face_up * face_sun * fade_depth * fade_range * shadow_v.r * sun_up;

	// surface absorption: shadowed water darkens toward the deep tint, fading out by 3000..6000 units
	float absorb_strength = 1.0f - smoothstep(3000.0f, 6000.0f, len_v);
	scene_v *= lerp(float3(1.0f, 1.0f, 1.0f), lerp(deep_v, float3(1.0f, 1.0f, 1.0f), shadow_v.g), absorb_strength);

	// extinction over the path (scaled so the game's distances give a usable murk), then the murk mix
	float3 tint_rgb = exp(-max(TESR_WaterCoefficients.xyz, 0.0f) * len_v * 0.0002f);
	float3 col_v = lerp(scene_v * tint_rgb, fog_out, murk_v);
	col_v = lerp(col_v, fog_out, smoothstep(4000.0f, 5000.0f, len_v));	// far geometry and sky go to the fog colour

	// god-ray shafts (the ray image's red channel, read at half the pixel's coordinates)
	float rays_v = tex2Dlod(TESR_RenderedBuffer, float4(uv * 0.5f, 0.0f, 0.0f)).r;
	col_v += rays_v * sun_col * max(TESR_WaterVolume.w, 0.0f);

	// encode, then a 4x4 ordered dither of one 8-bit step against banding
	float3 enc_v = saturate(delinearize(col_v));
	float dither_v = ((Bayer4(vpos) + 1.0f) / 17.0f - 0.5f) / 255.0f;
	return float4(enc_v + dither_v, 1.0f);
}

// ---- pass 4: the wobble (last pass; writes the game target when it is the last effect) ----
float4 Wobble(VSOUT px) : COLOR0
{
	float2 uv = px.UVCoord;
	float3 eye_v = TESR_CameraPosition.xyz;
	float3 P = eye_v + toWorld(uv) * readDepth(uv);
	if (abovePixel(eye_v, P)) return tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0f, 0.0f));

	float wobble_v = TESR_GameTime.z * TESR_WaveParams.z * 1.5f;
	float2 amplitude_v = 0.001f * TESR_RenderScale.xy;	// the on-screen size stays the same in rect frames
	float2 offset = float2(sin(wobble_v * 3.0f + uv.y * 60.0f), cos(wobble_v * 3.0f + uv.x * 60.0f)) * amplitude_v;
	return tex2Dlod(TESR_RenderedBuffer, float4(uv + offset, 0.0f, 0.0f));
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 Godrays( );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 BlurVertical( );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 BlurHorizontal( );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 Water( );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 Wobble( );
	}
}
