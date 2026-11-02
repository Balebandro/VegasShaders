// VolumetricFog: the atmospheric haze over the whole picture (wave 3, 2026-11-02, written from
// src/effects/VolumetricFog/README.md and effects/VolumetricFog.cpp; the Vulkan passes are Compute/VolumetricFog.glsl,
// VolumetricFogEstimate.frag and VolumetricFogApply.frag, the same maths).
//
// Techniques, as VolumetricFog.cpp asks for them (names, pass counts and order are the C++'s contract):
//   (unnamed, first)              full resolution, one pass                     EffectRecord::Render fallback, techniqueIndex 0
//   DedicatedFog                  Estimate (half size, MRT) + Reconstruct       RenderDedicated
//   CompositeFog                  as DedicatedFog; Reconstruct applies sun shadow, AO and sky take first
//   CompositeFogInterior          as CompositeFog with the interior darkening (InteriorShadowFold) instead of the sun shadow
//   ApplyShadowAO                 sun shadow and AO only (RenderShadowAO)
//   PackedFog                     the one-target fallback: coefficients packed into a quarter of the target, then the apply
//   HalfStoreProbe                measures how this GPU rounds a 16-bit float store (HalfStoreMode, for the interior fold)
//   VolumetricFogLegacy, DedicatedFogLegacy, CompositeFogLegacy, CompositeFogInteriorLegacy, PackedFogLegacy    LegacyFog
//   VolumetricFogClassic, DedicatedFogClassic, CompositeFogClassic, CompositeFogInteriorClassic                 ClassicLook
// FogReference (a development-only diagnostic that draws the original upstream shader) is not here: the C++ then draws the
// unnamed technique.
//
// Conventions. Colours: the frame is stored gamma-encoded; the fog works in linear light with the piecewise sRGB curve (Helpers.hlsl
// linearize / delinearize); the shadow, interior and AO composites use the 2.2 power curve of the passes they replace. Depth:
// TESR_DepthBuffer red = view z / far. Positions: camera-relative world, toWorld(uv) * view z (the projection's TAA jitter terms are
// not used, so the fog does not wobble with the jitter). RenderScale v2: uv are rect coordinates, rectToScreen gives the screen
// position; every read is clamped to the drawn part with clampToRectIfOn.
// Branches: every switched-off feature is skipped with a dynamic branch on a float uniform ([branch]; ps_3_0 compares float
// constants directly, no bool registers are used); the looks (modern / legacy / classic), the interior estimate and the composite
// kinds are compile-time arguments of the pixel shaders, one shader per technique pass. Texture reads use explicit level 0
// (tex2Dlod / tex3Dlod) so they are legal inside those branches.

#ifndef PHYSICAL_SKY_ON
	#define PHYSICAL_SKY_ON 0
#endif

#include "Includes/Helpers.hlsl"

// ---- samplers. TESR_ ones bind by their order in this file (s0..s5); NVR_ ones are bound by the C++ to the stage given ----
sampler2D TESR_SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_RenderedBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_ShadowAtlas : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_PointShadowBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D NVR_FogBuffer : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_FogBufferAdd : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_AOBuffer : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler3D NVR_FogNoise : register(s9) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D NVR_FogSkyView : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D NVR_FogGrid : register(s11) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D NVR_SkyVis : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_SunFar : register(s13) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

// ---- the fog's own constants (VolumetricFogEffect::RegisterConstants) ----
float4 TESR_VolumetricFogDensity;       // x BaseDensity, y WeatherImpact, z MorningFogDip, w SunriseSunsetBoost
float4 TESR_VolumetricFogShape;         // x HeightFalloff (HeightLayer on: -1 / LayerThickness), y MaxHeight (layer on: its base, world z), z Extinction, w Inscattering
float4 TESR_VolumetricFogWind;          // xy wind direction (cos, sin of WindAngle), z WindSpeed, w NoiseScale
float4 TESR_VolumetricFogScatter;       // x PhaseAsymmetry, y ShadowStrength, z NoiseStrength, w HeightInfluence
float4 TESR_VolumetricFogWeather;       // x WeatherFilterBlend (1 = sky masked out), y isExterior, z the Sky effect's SH is fresh, w FogSaturation
float4 TESR_VolumetricFogAerial;        // x AerialStrength, y AerialRangeStart, z AerialTintBlend, w AerialDayFadeStart
float4 TESR_VolumetricFogAerialTint;    // xyz manual aerial tint
float4 TESR_VolumetricFogDistant;       // x DistantFogRange, y DistantFogBlend, z DistantFogHeight, w EdgeAA
float4 TESR_VolumetricFogGlobal;        // x Amount, y Night DisableSkyMask, z moon-phase night floor (C++), w MinDensityFloor
float4 TESR_VolumetricFogNight;         // x DensityScale, y AmountScale, z HeightFalloffScale, w MaxHeightOffset (thousands of units)
float4 TESR_VolumetricFogNightScatter;  // x NoiseStrengthScale, y WindSpeedScale, z ExtinctionScale, w InscatteringScale
float4 TESR_VolumetricFogNightTint;     // xyz night tint, w NightAmbientStrength
float4 TESR_VolumetricFogSun;           // x SunGlow, y WeatherFogColor, z GameFogFloor this frame, w the weather's raw fog near
float4 TESR_VolumetricFogLow;           // legacy: x FogSaturation, y WeatherImpact, z DistantFogRange, w SunPower
float4 TESR_VolumetricFogHigh;          // legacy: x HeightFogDensity, y HeightFogFalloff, z HeightFogDist, w HeightFogSkyColor
float4 TESR_VolumetricFogSimple;        // legacy: x Extinction, y Inscattering, z FogNight, w SimpleFogSkyColor
float4 TESR_VolumetricFogBlend;         // legacy: x DistantFogBlend, y HeightFogBlend, z HeightFogRolloff, w SimpleFogBlend
float4 TESR_VolumetricFogHeight;        // legacy: x DistantFogHeight, y HeightFogHeight, z SimpleFogHeight, w isExterior
float4 TESR_VolumetricFogData;          // legacy: x MinimumBaseFog, y HeightFogSaturation, z Amount, w HeightFogInscattering

// ---- constants of the weather, sky, sun, shadows and AO (registered by those systems) ----
float4 TESR_FogData;                    // x fog near, y fog far, z sun glare, w fog power
float4 TESR_FogWeatherColor;            // the weather's fog colour (sky look on top)
float4 TESR_FogHorizonColor;            // the fog's copies of the sky's horizon / low / top colours
float4 TESR_FogSkyLowColor;
float4 TESR_FogSkyColor;
float4 TESR_ReciprocalResolution;       // 1 / full target size
float4 TESR_GameTime;                   // x game time, ms
float4 TESR_SunPosition;                // the sun's direction (its height)
float4 TESR_SunColor;
float4 TESR_SunsetColor;
float4 TESR_SunAmbient;                 // the weather's ambient colour
float4 TESR_SkyData;                    // x atmosphere thickness
float4 TESR_SunAmount;                  // x sun amount (0 night .. 1 day)
float4 TESR_PBRData;                    // z direct light scale, w ambient scale
float4 TESR_SkyIrradiance[9];           // order-2 SH of the sky (convolved, /pi folded in: evaluates to radiance)
float4 TESR_SkyIrradianceGame[9];       // the same for the game's own sky (ClassicLook)
float4 TESR_ShadowFade;                 // x sun shadow fade (1 = no shadow), y shadow maps on, z lamp shadows on, w interior shadow distance
float4 TESR_ShadowData;                 // y Darkness
float4 TESR_WaterSettings;              // x water height, z camera underwater
float4 TESR_AmbientOcclusionAOData;     // z ClampStrength (AO floor)
float4 TESR_AmbientOcclusionData;       // y LumThreshold, z BlurDropThreshold
float4 TESR_VolumetricLightData;        // x strength, y shade darkening, z air density per unit, w march distance
float4 TESR_VolumetricLightData2;       // y phase asymmetry, z fade
float4 TESR_VolumetricLightGridLayout;  // xy grid columns / rows, zw one atlas texel
float4 TESR_VolumetricLightGridData;    // x fog volume written (FogVolume), y scale of the lit share
#if PHYSICAL_SKY_ON
float4 TESR_PhysSky;                    // x physical sky share, y brightness, z weather tint, w haze
float4 TESR_PhysSkySun;                 // xyz the sun's real direction
#endif

// ---- set per draw by the C++ ----
float4 NVR_FogLayout;                   // xy half extent / full extent, zw 1 / half size
float4 NVR_CompositeFlags;              // x sun shadow, y AO, z sky visibility, w 16-bit rounding of the interior fold (0 even, 1 none, 2 toward zero)
float4 NVR_CompositeAOTexel;            // xy 1 / AO target size, zw SkyTake cap and no-limit
float4 NVR_FogGridFlags;                // x froxel grid bound, y the volumetric light's composite is done here
float4 NVR_SunFarFlags;                 // x SunFar factor bound

#define INTERIOR_SHADOW_MAXDISTANCE TESR_ShadowFade.w

#include "Includes/Depth.hlsl"          // toWorld, readDepthLod, farZ, TESR_CameraPosition, the transforms, RenderRect
#include "Includes/Normals.hlsl"        // GetWorldNormalLod
#include "Includes/Shadows.hlsl"        // VSM / EVSM / PCF evaluation
#include "Includes/SunCascades.hlsl"    // GetSunCascadeLight, TESR_SmoothedSunDir and the cascade constants
#include "Includes/InteriorShadowApply.hlsl"
#include "Includes/SkyTake.hlsl"
#include "Includes/Sky.hlsl"            // GetSunColor
#if PHYSICAL_SKY_ON
#include "Includes/Atmosphere.hlsl"     // AtmSunDepth, AtmGround
#endif

#define LOOK_MODERN 0
#define LOOK_LEGACY 1
#define LOOK_CLASSIC 2
#define COMP_NONE 0
#define COMP_EXTERIOR 1
#define COMP_INTERIOR 2

static const float SkyDepth = 0.9961f;          // normalised depth at and past which a pixel is sky
static const float NoiseTile = 6000.0f;          // world units per noise tile at NoiseScale 1 (4 cells of 1500, as the froxel grid)
static const float2 Quad[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };

#define LOD2(s, coord) tex2Dlod(s, float4((coord), 0.0f, 0.0f))

// ---- vertex shaders ----
struct FrameIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct FrameOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
FrameOut FrameVS(FrameIn i) {
	FrameOut o;
	o.pos = float4(i.pos, 1.0f);
	o.uv = i.uv;
	return o;
}
// HalfStoreProbe's quad comes as pre-transformed XYZW (VolumetricFog.cpp HalfStoreMode).
struct ProbeIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
FrameOut ProbeVS(ProbeIn i) {
	FrameOut o;
	o.pos = i.pos;
	o.uv = i.uv;
	return o;
}

// ==== small helpers ====

float NightShare(bool inside) {
	return inside ? 0.0f : TESR_VolumetricFogWeather.y * (1.0f - smoothstep(0.4f, 0.8f, TESR_SunAmount.x));
}

// The SH sky radiance seen looking straight up (the coefficients already carry the band weights and 1/pi).
float3 SkyUp(float4 sh[9]) {
	return max(sh[0].rgb + sh[2].rgb + 2.0f * sh[6].rgb, 0.0f);
}

// A colour scaled so its largest channel is 1 (a tint that never brightens).
float3 TintOf(float3 c) {
	return c / max(max(c.r, max(c.g, c.b)), 1e-4f);
}

// 1 when one of the four direct neighbours of the pixel is sky.
float SkyNeighbour(float2 uv) {
	float2 t = TESR_ReciprocalResolution.xy;
	float4 d = float4(LOD2(TESR_DepthBuffer, clampToRectIfOn(uv + float2(t.x, 0.0f), t)).x, LOD2(TESR_DepthBuffer, clampToRectIfOn(uv - float2(t.x, 0.0f), t)).x,
	                  LOD2(TESR_DepthBuffer, clampToRectIfOn(uv + float2(0.0f, t.y), t)).x, LOD2(TESR_DepthBuffer, clampToRectIfOn(uv - float2(0.0f, t.y), t)).x);
	return step(SkyDepth, max(max(d.x, d.y), max(d.z, d.w)));
}

// Height profile of the fog, relative density at a world height. layer > 0.5: full below base, exp(-k (z - base)) above
// (HeightLayer); otherwise exp(-k (z - base)) everywhere (the HeightFalloff / MaxHeight curve).
float HeightRho(float z, float k, float base, float layer) {
	float h = z - base;
	return exp(clamp(-k * (layer > 0.5f ? max(h, 0.0f) : h), -30.0f, 30.0f));
}

// The mean of that profile along the straight path from height zc to zp (closed form; no division by the distance).
float HeightPathMean(float zc, float zp, float k, float base, float layer) {
	float dz = zp - zc;
	float mid = HeightRho(0.5f * (zc + zp), k, base, layer);
	float mean;
	if (layer > 0.5f) {
		float hc = zc - base, hp = zp - base;
		float fc = hc <= 0.0f ? hc : (1.0f - exp(-k * hc)) / k;
		float fp = hp <= 0.0f ? hp : (1.0f - exp(-k * hp)) / k;
		mean = (fp - fc) / (abs(dz) < 1.0f ? 1.0f : dz);
	}
	else {
		float x = clamp(k * dz, -30.0f, 30.0f);
		float f = abs(x) < 1e-3f ? 1.0f - 0.5f * x : (1.0f - exp(-x)) / x;
		mean = HeightRho(zc, k, base, 0.0f) * f;
	}
	return clamp(abs(dz) < 1.0f ? mid : mean, 0.0f, 1e4f);
}

// The density noise (green of NVR_FogNoise) at a camera-relative point, drifting with the wind.
float NoiseAt(float3 rel, float3 wind, float scale) {
	float3 c = (TESR_CameraPosition.xyz + rel + wind) / (NoiseTile * scale);
	return tex3Dlod(NVR_FogNoise, float4(c, 0.0f)).g;
}

// The froxel grid (the volumetric light's, effects/VolumetricLight.cpp): 64 depth slices s^2-spaced to the march distance, in an
// 8 x 8 atlas; after its prefix sums every slice holds the sums from the camera to its far side: r the lit air (x grid scale),
// g the fog's height weight, b height x noise, a height x sun visibility (g, b, a: x share of the march distance; FogVolume only).
float4 GridSums(float2 s01, float planar) {
	float march = max(TESR_VolumetricLightData.w, 1.0f);
	float s = sqrt(saturate(planar / march)) * 64.0f;
	float z = min(floor(s), 63.0f);
	float2 cells = TESR_VolumetricLightGridLayout.xy;
	float2 local = clamp(s01 * cells, 0.5f, cells - 0.5f);
	float2 tileFar = float2(z - 8.0f * floor(z / 8.0f), floor(z / 8.0f));
	float zn = max(z - 1.0f, 0.0f);
	float2 tileNear = float2(zn - 8.0f * floor(zn / 8.0f), floor(zn / 8.0f));
	float4 farSide = LOD2(NVR_FogGrid, (tileFar * cells + local) * TESR_VolumetricLightGridLayout.zw);
	float4 nearSide = z > 0.5f ? LOD2(NVR_FogGrid, (tileNear * cells + local) * TESR_VolumetricLightGridLayout.zw) : 0.0f;
	return lerp(nearSide, farSide, saturate(s - z));
}

#if PHYSICAL_SKY_ON
// The physical sky table (SkyView.fx: 128 x 64, u = bearing from the sun / pi, v = sqrt(elevation / (pi / 2)), x 1000) in a direction.
float3 PhysicalSky(float3 d) {
	float2 sunH = TESR_PhysSkySun.xy / max(length(TESR_PhysSkySun.xy), 1e-5f);
	float2 dirH = d.xy / max(length(d.xy), 1e-5f);
	float bearing = acos(clamp(dot(dirH, sunH), -1.0f, 1.0f));
	float elevation = asin(saturate(d.z));
	float2 coord = float2(bearing / PI, sqrt(elevation / (0.5f * PI)));
	return LOD2(NVR_FogSkyView, coord).rgb * (0.001f * TESR_PhysSky.y);
}
// The sky's own blend of a game colour with the physical one (Sky.cpp PhysAtmosphere::Blend).
float3 PhysicalBlend(float3 game, float3 phys) {
	float3 tinted = phys + (game * (luma(phys) / max(luma(game), 1e-6f)) - phys) * TESR_PhysSky.z;
	return lerp(game, tinted, TESR_PhysSky.x);
}
#endif

// The sun's visibility at a camera-relative point from the cascades (Includes/SunCascades.hlsl's lookup and normal offset), with one
// lookup in the first cascade that holds the point (the fog averages it anyway; the include's cross-cascade blend would inline the
// lookup eight times, past ps_3_0's constant registers once the effect compiler adds its preshader), fading to lit at the last one's edge.
float FogSunVisibility(float3 p, float3 n) {
	float4 radii = float4(TESR_ShadowNearCenter.w, TESR_ShadowMiddleCenter.w, TESR_ShadowFarCenter.w, TESR_ShadowLodCenter.w);
	float4 d = float4(length(p - TESR_ShadowNearCenter.xyz), length(p - TESR_ShadowMiddleCenter.xyz),
	                  length(p - TESR_ShadowFarCenter.xyz), length(p - TESR_ShadowLodCenter.xyz));
	float4x4 m = TESR_ShadowCameraToLightTransformLod;
	float4 pick = float4(0.5f, 0.5f, 0.8f, radii.w);   // quadrant, bleed reduction, radius
	[branch] if (d.x < radii.x) { m = TESR_ShadowCameraToLightTransformNear; pick = float4(0.0f, 0.0f, 0.1f, radii.x); }
	else if (d.y < radii.y) { m = TESR_ShadowCameraToLightTransformMiddle; pick = float4(0.5f, 0.0f, 0.2f, radii.y); }
	else if (d.z < radii.z) { m = TESR_ShadowCameraToLightTransformFar; pick = float4(0.0f, 0.5f, 0.6f, radii.z); }
	else if (d.w >= radii.w) return 1.0f;
	float offsetScale = saturate(1.0f - dot(n, TESR_SmoothedSunDir.xyz));
	float offset = offsetScale * 10.0f * pick.w * max(TESR_ShadowBlur.x, 1.0f / 16384.0f);
	float bias = (TESR_ShadowFormatData.x == 0.0f ? 0.00001f : 0.01f) * (1.0f + offsetScale);
	float v = SunCascadeValue(m, float4(p + offset * n, 1.0f), pick.x, pick.y, bias, pick.z);
	return lerp(v, 1.0f, smoothstep(radii.w * 0.9f, radii.w, d.w) * step(radii.z, d.z));
}

// A float rounded to the nearest 16-bit float as a render-target store does it: mode 0 to nearest even, 2 toward zero, 1 not at all.
float3 RoundToHalf(float3 c, float mode) {
	float3 a = min(abs(c), 65504.0f);
	float3 m = max(a, 6.103515625e-05f);                     // below 2^-14 the step is the subnormal one, 2^-24
	float3 e = floor(log2(m));
	e = exp2(e) > m ? e - 1.0f : e;                           // log2 may be off by one next to a power of two
	e = exp2(e + 1.0f) <= m ? e + 1.0f : e;
	float3 ulp = exp2(e - 10.0f);
	float3 q = a / ulp;                                       // exact: ulp is a power of two
	float3 lo = floor(q);
	float3 rem = q - lo;
	float3 odd = lo - 2.0f * floor(0.5f * lo);
	float3 even = lo + ((rem > 0.5f || (rem == 0.5f && odd > 0.5f)) ? 1.0f : 0.0f);
	float3 r = (mode > 1.5f ? lo : even) * ulp;
	return (mode > 0.5f && mode < 1.5f) ? c : (c < 0.0f ? -r : r);
}

// ==== the fog of one pixel: linear multiply and add, applied as scene * mul + add ====
// look: LOOK_MODERN / LOOK_LEGACY / LOOK_CLASSIC. inside: the interior estimate (everything outdoor-only compiled out).
void FogTerms(float2 uv, float dN, uniform int look, uniform bool inside, out float3 mulOut, out float3 addOut) {
	const bool modern = look == LOOK_MODERN;
	const bool classic = look == LOOK_CLASSIC;
	const bool legacy = look == LOOK_LEGACY;
	float isExt = inside ? 0.0f : TESR_VolumetricFogWeather.y;
	float nf = NightShare(inside);
	float sunAmt = TESR_SunAmount.x;
	float dayFade = smoothstep(0.1f, 0.6f, sunAmt);           // the sunlight ramp through dawn and dusk

	// Where the pixel is: camera-relative position, distance, direction and absolute height.
	float viewZ = max(dN * farZ, 1.0f);
	float3 rel = toWorld(uv) * viewZ;
	float dist = max(length(rel), 1.0f);
	float3 dir = rel / dist;
	float zc = TESR_CameraPosition.z;
	float zp = zc + rel.z;
	float sky = dN >= SkyDepth ? 1.0f : 0.0f;

	// Density of NVR's own fog: the base (time of day, night), the weather's share, the dawn / dusk bump.
	float fogPow = max(lerp(1.0f, TESR_FogData.w, saturate(TESR_VolumetricFogDensity.y)), 0.05f);
	float3 sunPos = TESR_SunPosition.xyz / max(length(TESR_SunPosition.xyz), 1e-5f);
	float sunH = saturate(sunPos.z);
	float2 weatherNF = saturate(1.0f - TESR_FogData.xy / max(farZ, 1.0f));
	float anchor = sqr(0.5f * (weatherNF.x + weatherNF.y)) / (fogPow + 1.0f);

	// The colours the fog takes its light from.
	float3 skyTop = linearize(TESR_FogSkyColor.rgb);
	float3 horizon = linearize(TESR_FogHorizonColor.rgb);
	float3 skyHaze = lerp(horizon, linearize(TESR_FogSkyLowColor.rgb), smoothstep(0.0f, 0.35f, saturate(dir.z)));
	skyHaze = lerp(skyTop, skyHaze, isExt);                   // indoors the fog's sky colour is the cell's fog colour
	horizon = lerp(skyTop, horizon, isExt);
	float3 weatherCol = linearize(TESR_FogWeatherColor.rgb);
	float3 sunLight = GetSunColor(sunH, TESR_SkyData.x, sunAmt, TESR_SunColor.rgb, TESR_SunsetColor.rgb) * TESR_PBRData.z;
#if PHYSICAL_SKY_ON
	if (modern && !inside) {
		[branch] if (TESR_PhysSky.x > 0.0f && isExt > 0.5f) {
			skyHaze = PhysicalBlend(skyHaze, PhysicalSky(dir));
			horizon = PhysicalBlend(horizon, PhysicalSky(float3(dir.xy, 0.0f)));
			float3 through = exp(-AtmSunDepth(AtmGround + 300.0f, TESR_PhysSkySun.z, TESR_PhysSky.w));
			float3 physSun = luma(sunLight) * through / max(luma(through), 1e-4f);
			sunLight = lerp(sunLight, lerp(physSun, sunLight, TESR_PhysSky.z), TESR_PhysSky.x);
		}
	}
#endif
	float3 ambient = skyTop;
	[branch] if (TESR_VolumetricFogWeather.z * isExt > 0.5f) ambient = classic ? SkyUp(TESR_SkyIrradianceGame) : SkyUp(TESR_SkyIrradiance);
	ambient *= TESR_PBRData.w;
	ambient = max(ambient, linearize(TESR_SunAmbient.rgb) * TESR_VolumetricFogGlobal.z * TESR_VolumetricFogNightTint.w * nf);

	// Sun visibility for the light the fog scatters (1 = lit).
	float shadowUse = saturate(TESR_VolumetricFogScatter.y) * isExt * step(0.5f, TESR_ShadowFade.y) * (1.0f - saturate(TESR_ShadowFade.x)) * dayFade;
	float sunVis = 1.0f;

	float T;              // transmittance through the fog
	float3 inscatter;     // the light the fog adds where it is opaque, before (1 - T)
	float3 hazeCol;       // the fog's colour without the sun's glow (for the game fog floor)
	float amount;
	float4 distant;       // the horizon fog's range, blend and height, and EdgeAA

	if (legacy) {
		// ---- LegacyFog: the Simple + Height fog keys, rebuilt (an approximation of the pre-rewrite look) ----
		float nightDist = lerp(1.0f, TESR_VolumetricFogSimple.z, nf);                          // FogNight
		float nearL = TESR_FogData.x * nightDist;
		float farL = max(TESR_FogData.y * nightDist, max(nearL + 1.0f, 1.0f));
		float powL = max(TESR_FogData.w, 0.05f);
		float weatherCover = pow(saturate((dist - nearL) / (farL - nearL)), powL) * saturate(TESR_VolumetricFogLow.y);
		float extL = max(TESR_VolumetricFogSimple.x, 0.0f) * 0.1f;
		float base = max(TESR_VolumetricFogData.x, 0.0f);                                      // MinimumBaseFog
		// Simple fog: base density over distance, thinning with height above the camera (SimpleFogHeight), plus the weather's curve.
		float simpleH = HeightPathMean(zc, zp, max(TESR_VolumetricFogHeight.z, 0.0f) * 1e-3f, zc, 0.0f);
		float odS = base * extL * dist * 1e-4f * simpleH;
		float coverS = (1.0f - exp(-odS) * (1.0f - weatherCover)) * saturate(TESR_VolumetricFogBlend.w);
		// Height fog: from its sea level (HeightFogHeight), its falloff, pushed back by HeightFogDist and HeightFogRolloff.
		float kL = 1.5e-4f / (powL * max(TESR_VolumetricFogHigh.y, 1e-4f));
		float seaL = -10000.0f + 1000.0f * TESR_VolumetricFogHeight.y;
		float pushed = farL * pow(max(dist * TESR_VolumetricFogHigh.z / farL, 0.0f), max(TESR_VolumetricFogBlend.z, 0.05f));
		float odH = max(TESR_VolumetricFogHigh.x, 0.0f) * extL * pushed * 1e-4f * HeightPathMean(zc, zp, kL, seaL, 0.0f);
		float coverH = (1.0f - exp(-odH)) * saturate(TESR_VolumetricFogBlend.y);
		T = (1.0f - coverS) * (1.0f - coverH);
		// Colours: strong fog takes the weather's colour instead of the sky's (SimpleFogSkyColor, HeightFogSkyColor).
		float wS = saturate(coverS * 10.0f / max(TESR_VolumetricFogSimple.w, 0.01f));
		float wH = saturate(coverH * 10.0f / max(TESR_VolumetricFogHigh.w, 0.01f));
		float3 colS = lerp(skyHaze, weatherCol, inside ? 1.0f : wS);
		float3 colH = lerp(skyHaze, weatherCol, inside ? 1.0f : wH);
		colS = lerp(luma(colS).xxx, colS, max(TESR_VolumetricFogLow.x, 0.0f));
		colH = lerp(luma(colH).xxx, colH, max(TESR_VolumetricFogData.y, 0.0f));
		colS = max(lerp(colS, ambient, 0.5f), 0.0f) * lerp(1.0f, TESR_VolumetricFogNightTint.rgb, nf);
		colH = max(lerp(colH, ambient, 0.5f), 0.0f) * lerp(1.0f, TESR_VolumetricFogNightTint.rgb, nf);
		float albS = saturate(max(TESR_VolumetricFogSimple.y, 0.0f) * 0.1f / max(extL, 1e-4f));
		float albH = saturate(max(TESR_VolumetricFogData.w, 0.0f) * 0.1f / max(extL, 1e-4f));
		// Sun through the fog (SunPower), a soft forward lobe.
		float cosS = dot(dir, TESR_SmoothedSunDir.xyz);
		float3 sunL = sunLight * max(TESR_VolumetricFogLow.w, 0.0f) * pow(saturate(0.5f + 0.5f * cosS), 4.0f) * dayFade * isExt;
		float coverAll = max(1.0f - T, 1e-4f);
		inscatter = ((colS * albS * coverS * (1.0f - coverH) + colH * albH * coverH) / coverAll) + sunL;
		hazeCol = colS;
		amount = saturate(TESR_VolumetricFogData.z * lerp(1.0f, TESR_VolumetricFogNight.y, nf));
		distant = float4(TESR_VolumetricFogLow.z, TESR_VolumetricFogBlend.x, TESR_VolumetricFogHeight.x, 0.0f);
	}
	else {
		// ---- the current fog (modern) and ClassicLook ----
		float timeScale = 1.0f - saturate(TESR_VolumetricFogDensity.z) * isExt * sunH * dayFade;      // MorningFogDip: thinnest at noon
		float nightDensity = lerp(1.0f, TESR_VolumetricFogNight.x, nf);
		float transition = 4.0f * saturate(sunAmt) * (1.0f - saturate(sunAmt)) * isExt;             // SunriseSunsetBoost's bump
		float S = max(TESR_VolumetricFogDensity.x * timeScale * nightDensity + TESR_VolumetricFogDensity.y * anchor +
		              TESR_VolumetricFogDensity.w * transition, 0.0f);

		// Height profile: HeightLayer (Shape.x < 0, modern outdoors only, set by the C++) or the HeightFalloff / MaxHeight curve.
		float layer = TESR_VolumetricFogShape.x < 0.0f ? 1.0f : 0.0f;
		float hk = layer > 0.5f ? -TESR_VolumetricFogShape.x
			: 1.5e-4f / (fogPow * max(TESR_VolumetricFogShape.x, 1e-4f) * lerp(1.0f, max(TESR_VolumetricFogNight.z, 0.0f), nf));
		float hbase = layer > 0.5f ? TESR_VolumetricFogShape.y
			: -10000.0f + 1000.0f * (TESR_VolumetricFogShape.y + TESR_VolumetricFogNight.w * nf);
		float influence = saturate(TESR_VolumetricFogScatter.w);
		float shape = lerp(1.0f, HeightPathMean(zc, zp, hk, hbase, layer), influence);

		float ext = max(TESR_VolumetricFogShape.z, 0.0f) * 0.1f * lerp(1.0f, TESR_VolumetricFogNightScatter.z, nf);
		float ins = max(TESR_VolumetricFogShape.w, 0.0f) * 0.1f * lerp(1.0f, TESR_VolumetricFogNightScatter.w, nf);
		float ref = max(TESR_FogData.y, 1000.0f);
		float flatOD = S * ext * ref * pow(dist / ref, fogPow) * 1e-4f;      // distance warped by the fog power about the weather's far
		float floorOD = saturate(TESR_VolumetricFogGlobal.w) * flatOD;       // MinDensityFloor
		float plainOD = max(flatOD * shape, floorOD);

		// The froxel grid's air averages (FogVolume): the share of the optical depth inside the grid's reach.
		float gridOn = 0.0f;
		float4 grid = 0.0f;
		float gridCover = 0.0f;
		if (modern && !inside) {
			gridOn = NVR_FogGridFlags.x * TESR_VolumetricLightGridData.x * isExt;
			[branch] if (gridOn > 0.5f) {
				float march = max(TESR_VolumetricLightData.w, 1.0f);
				grid = GridSums(rectToScreen(uv), sky > 0.5f ? march : min(viewZ, march));
				float c = saturate(march / viewZ);                            // share of the view inside the grid's reach
				gridCover = plainOD < 1e-4f ? c : (1.0f - exp(-plainOD * c)) / max(1.0f - exp(-plainOD), 1e-5f);
				gridCover *= step(1e-6f, grid.g);
			}
		}

		// Density noise: drifting patches. Modern: the grid's air average, else four points along the view weighed by the
		// height profile; classic: three octaves at the surface point. Neutral on the sky.
		float noiseK = saturate(TESR_VolumetricFogScatter.z * lerp(1.0f, TESR_VolumetricFogNightScatter.x, nf));
		float noise = 0.5f;
		[branch] if (noiseK > 0.001f && sky < 0.5f) {
			float scale = max(TESR_VolumetricFogWind.w, 1e-4f);
			float3 wind = float3(TESR_VolumetricFogWind.xy * (TESR_VolumetricFogWind.z * lerp(1.0f, max(TESR_VolumetricFogNightScatter.y, 0.0f), nf) *
				TESR_GameTime.x * 0.002f), 0.0f);
			if (classic) {
				float n = 0.5f * NoiseAt(rel, wind, scale) + 0.3f * NoiseAt(rel * 2.03f, wind, scale) + 0.2f * NoiseAt(rel * 4.01f, wind, scale);
				noise = saturate((n - 0.5f) * 1.8f + 0.5f);
			}
			else {
				float marched = 0.5f;
				[branch] if (gridCover < 0.999f) {
					float tMax = min(dist, 12000.0f);
					float sum = 0.0f, weights = 0.0f;
					[unroll] for (int i = 0; i < 4; i++) {
						float t = tMax * (i + 0.5f) * 0.25f;
						float w = lerp(1.0f, HeightRho(zc + dir.z * t, hk, hbase, layer), influence) + 1e-4f;
						sum += w * NoiseAt(dir * t, wind, scale);
						weights += w;
					}
					marched = sum / weights;
				}
				float gridNoise = grid.b / max(grid.g, 1e-6f);
				noise = saturate((lerp(marched, gridNoise, gridCover) - 0.5f) * 1.8f + 0.5f);
			}
		}
		float od = max(plainOD * max(1.0f + noiseK * (2.0f * noise - 1.0f), 0.0f), floorOD);
		T = exp(-od);

		// Fog colour: the sky's light mixed with the weather's colour (WeatherImpact, WeatherFogColor; the sky keeps its own
		// colour), toward the weather's colour as it thickens, saturation and the night tint.
		float useWeather = modern ? saturate(TESR_VolumetricFogSun.y) * (1.0f - sky) : 0.0f;
		float3 weatherSide = lerp(skyHaze, weatherCol, useWeather);
		float3 col = lerp(ambient, weatherSide, saturate(TESR_VolumetricFogDensity.y));
		col = lerp(col, lerp(weatherCol, skyHaze, sky), 1.0f - exp(-6.0f * S));
		col = lerp(luma(col).xxx, col, max(TESR_VolumetricFogWeather.w, 0.0f) / (1.0f + S));
		col = max(col, 0.0f) * lerp(1.0f, TESR_VolumetricFogNightTint.rgb, nf);

		// The sun: its shadow over the fog's light, and the forward glow round it (SunGlow; 1 in ClassicLook).
		[branch] if (shadowUse > 0.001f) {
			if (!classic && gridOn > 0.5f) {
				sunVis = lerp(1.0f, grid.a / max(grid.g, 1e-6f), gridCover);
			}
			else {
				[branch] if (sky < 0.5f) sunVis = FogSunVisibility(rel, normalize(GetWorldNormalLod(uv)));
			}
			sunVis = lerp(1.0f, saturate(sunVis), shadowUse);
		}
		float g = saturate(TESR_VolumetricFogScatter.x);
		float cosS = dot(dir, TESR_SmoothedSunDir.xyz);
		float hg = min((1.0f - g * g) / pow(max(1.0f + g * g - 2.0f * g * cosS, 1e-4f), 1.5f), 8.0f);
		float glow = (classic ? 1.0f : TESR_VolumetricFogSun.x) * 2.0f * max(hg - 1.0f, 0.0f) * sqr(1.0f - sunH) *
		             (1.0f + TESR_FogData.z * dN) * dayFade * isExt;
		float sunShare = 0.35f * dayFade * isExt;                               // the part of the fog's light that is direct sun
		hazeCol = col * (1.0f - sunShare + sunShare * sunVis);
		inscatter = (hazeCol + sunLight * (glow * sunVis)) * saturate(ins / max(ext, 1e-4f));
		amount = saturate(TESR_VolumetricFogGlobal.x * lerp(1.0f, TESR_VolumetricFogNight.y, nf));
		distant = TESR_VolumetricFogDistant;
	}

	// Compose; the sky dome is masked out unless the weather (WeatherFilterBlend) or the night (DisableSkyMask) lets it in.
	float skyMask = lerp(1.0f - sky * saturate(TESR_VolumetricFogWeather.x), 1.0f, nf * saturate(TESR_VolumetricFogGlobal.y));
	float3 mul = lerp(1.0f, T, skyMask).xxx;
	float3 add = inscatter * ((1.0f - T) * skyMask);

	if (!legacy && !inside) {
		// Aerial perspective: distant terrain tinted toward the sky's hue (or the manual tint) by day.
		float aer = saturate(TESR_VolumetricFogAerial.x * smoothstep(min(saturate(TESR_VolumetricFogAerial.y), 0.999f), 1.0f, dN) * (1.0f - sky) *
			smoothstep(0.1f, max(TESR_VolumetricFogAerial.w, 0.11f), sunAmt) * isExt);
		float3 tint = saturate(lerp(TintOf(skyTop), TESR_VolumetricFogAerialTint.rgb, saturate(TESR_VolumetricFogAerial.z)));
		mul *= lerp(1.0f, tint, aer);
	}

	if (!inside) {
		// Horizon fog: distant land fades into the horizon colour, limited in height (or masked by the sky with height 0); EdgeAA
		// feathers land pixels that touch the sky toward the same colour.
		[branch] if (isExt > 0.5f && (distant.y > 0.0f || distant.w > 0.0f)) {
			float start = min(max(exp(-4.0f * distant.x), 1e-8f), 0.999f);
			float reach = sqrt(smoothstep(start, 1.0f, dN));
			float heightFade = distant.z > 0.0f ? min(1.0f, exp(-zp / (80000.0f * distant.z))) : 1.0f - sky;
			float hf = saturate(reach * distant.y * heightFade);
			float edge = 0.0f;
			[branch] if (distant.w > 0.0f && sky < 0.5f) edge = saturate(distant.w * 0.5f) * SkyNeighbour(uv) * reach;
			float k = saturate(hf + edge - hf * edge);
			mul *= 1.0f - k;
			add = add * (1.0f - k) + horizon * k;
		}
	}

	mul = lerp(1.0f, mul, amount);
	add *= amount;

	if (modern && !inside) {
		// GameFogFloor: at least the game's own fog curve for the weather (its raw near, far and power) covers the land.
		[branch] if (TESR_VolumetricFogSun.z > 0.0f && isExt > 0.5f && sky < 0.5f) {
			float nearG = TESR_VolumetricFogSun.w;
			float gameFog = saturate(TESR_VolumetricFogSun.z * pow(saturate((dist - nearG) / max(TESR_FogData.y - nearG, 1.0f)), max(TESR_FogData.w, 0.01f)));
			float covered = luma(mul);
			float top = saturate(1.0f - (1.0f - gameFog) / max(covered, 1e-4f));
			mul *= 1.0f - top;
			add = add * (1.0f - top) + hazeCol * top;
		}
	}

	mulOut = saturate(mul);
	addOut = max(add, 0.0f);
}

// ==== the composites the fog pass can take over (applied to the stored scene before the fog) ====

// The exterior sun-shadow apply (ShadowsExteriors.fx): SunFar first, then the shadow buffer's sun (red, faded by ShadowFade.x) and lamp
// (green) terms mapped from the darkness floor to 1, the shade tinted toward the sky. Gamma 2.2 in and out.
float3 SunShadowComposite(float2 uv, float3 frame) {
	[branch] if (NVR_SunFarFlags.x > 0.5f) frame *= LOD2(NVR_SunFar, uv).rgb;
	float2 terms = LOD2(TESR_PointShadowBuffer, uv).rg;
	float sun = lerp(terms.r, 1.0f, saturate(TESR_ShadowFade.x));
	float lit = saturate(sun + terms.g * saturate(TESR_ShadowFade.z));
	float f = lerp(max(0.0f, 1.0f - TESR_ShadowData.y), 1.0f, lit);
	[branch] if (TESR_WaterSettings.z > 0.5f) {
		// (underwater only, which the fog never draws: kept so the fold equals the separate pass) the water plane keeps its light
		float viewZ = readDepthLod(uv);
		float h = TESR_CameraPosition.z + toWorld(uv).z * viewZ;
		float3 n = normalize(GetWorldNormalLod(uv));
		f = (abs(h - TESR_WaterSettings.x) <= 2.0f && dot(n, float3(0.0f, 0.0f, -1.0f)) >= 0.999f) ? 1.0f : f;
	}
	float3 lin = pow(max(frame, 0.0f), 2.2f);
	float3 skyLin = pow(max(TESR_FogSkyColor.rgb, 0.0f), 2.2f);
	float3 shaded = lerp(luma(lin) * f * skyLin, lin * f, saturate(f + 0.5f));
	return pow(max(shaded, 0.0f), 1.0f / 2.2f);
}

// The AO combine (AmbientOcclusion's dedicated combine): the AO target's four nearest texels, bilinear and depth weighted, the AO
// floor, lighter on bright pixels, then the Skylighting v2 sky take with the same weights. Gamma 2.2 in and out; past 8000 nothing.
float3 AOComposite(float2 uv, float3 frame) {
	float viewZ = readDepthLod(uv);
	[branch] if (viewZ >= 8000.0f) return frame;
	float2 texel = NVR_CompositeAOTexel.xy;
	float2 p = uv / texel - 0.5f;
	float2 cell = floor(p);
	float2 f = p - cell;
	float drop = max(TESR_AmbientOcclusionData.z, 0.001f);
	float sum = 0.0f, weights = 0.0f;
	float2 skySum = 0.0f;
	float4 w4;
	float2 taps[4];
	[unroll] for (int i = 0; i < 4; i++) {
		float2 o = Quad[i];
		taps[i] = clampToRect((cell + o + 0.5f) * texel, texel);
		float2 ao = LOD2(NVR_AOBuffer, taps[i]).rg;
		float w = (o.x > 0.5f ? f.x : 1.0f - f.x) * (o.y > 0.5f ? f.y : 1.0f - f.y);
		w /= 1.0f + abs(ao.y - viewZ) / drop;
		w4[i] = w;
		sum += ao.x * w;
		weights += w;
	}
	float ao = lerp(TESR_AmbientOcclusionAOData.z, 1.0f, sum / max(weights, 1e-6f));
	float3 lin = pows(frame, 2.2f);
	ao = lerp(ao, 1.0f, saturate((luma(lin) - TESR_AmbientOcclusionData.y) * 3.0f));
	float3 result = frame * pow(abs(ao), 1.0f / 2.2f);
	[branch] if (NVR_CompositeFlags.z > 0.5f) {
		[unroll] for (int j = 0; j < 4; j++) skySum += LOD2(NVR_SkyVis, taps[j]).rb * w4[j];
		float2 skyV = saturate(skySum / max(weights, 1e-6f));
		result = pows(SkyTake(pows(result, 2.2f), skyV.x, skyV.y, NVR_CompositeAOTexel.z, NVR_CompositeAOTexel.w), 1.0f / 2.2f);
	}
	return result;
}

// Shadows (sun outdoors, or the interior darkening rounded as its own pass's 16-bit target stores it), then AO. alpha: 1 once any
// composite ran (the passes it replaces write alpha 1).
float3 CompositeScene(float2 uv, float3 frame, uniform int comp, inout float alpha) {
	if (comp == COMP_INTERIOR) {
		float lamps = LOD2(TESR_PointShadowBuffer, uv).r;
		frame = RoundToHalf(ApplyInteriorShadowLod(uv, float4(frame, 1.0f), lamps).rgb, NVR_CompositeFlags.w);
		alpha = 1.0f;
	}
	else {
		[branch] if (NVR_CompositeFlags.x > 0.5f) { frame = SunShadowComposite(uv, frame); alpha = 1.0f; }
	}
	[branch] if (NVR_CompositeFlags.y > 0.5f) { frame = AOComposite(uv, frame); alpha = 1.0f; }
	return frame;
}

// The volumetric light's composite (its froxel-grid technique) on the fogged linear colour, when the C++ folds it in here.
float3 LightAirComposite(float2 uv, float dN, float3 lin) {
	float march = max(TESR_VolumetricLightData.w, 1.0f);
	float viewZ = dN * farZ;
	float planar = dN >= 0.996f ? march : min(viewZ, march);
	float3 ray = toWorld(uv);
	float lit = GridSums(rectToScreen(uv), planar).r / max(TESR_VolumetricLightGridData.y, 1e-6f);
	float air = 1.0f - exp(-TESR_VolumetricLightData.z * length(ray) * planar);
	float g = TESR_VolumetricLightData2.y;
	float cosT = dot(normalize(ray), TESR_SmoothedSunDir.xyz);
	float phase = min((1.0f - g * g) / pow(max(1.0f + g * g - 2.0f * g * cosT, 1e-4f), 1.5f), 8.0f);
	float fade = TESR_VolumetricLightData2.z;
	return lin * (1.0f - TESR_VolumetricLightData.y * fade * saturate(air - lit)) +
	       linearize(TESR_SunColor.rgb) * (TESR_VolumetricLightData.x * fade * phase * lit);
}

// ==== passes ====

struct EstimateOut { float4 mulDepth : COLOR0; float4 add : COLOR1; };

// The full-resolution pixel a half texel stands for: the nearest (smallest depth) of the pixels it covers, so thin near objects
// keep their own fog. per: full pixels per half texel on each axis (2; 1 with MaxQuality).
float2 NearestCovered(float2 halfPixel, float2 per, out float depth) {
	float2 base = halfPixel * per;
	float2 bestUV = 0.0f;
	depth = 2.0f;
	[unroll] for (int i = 0; i < 4; i++) {
		float2 c = clampToRectIfOn((base + Quad[i] * (per - 1.0f) + 0.5f) * TESR_ReciprocalResolution.xy, TESR_ReciprocalResolution.xy);
		float d = LOD2(TESR_DepthBuffer, c).x;
		bool nearer = d < depth;
		bestUV = nearer ? c : bestUV;
		depth = nearer ? d : depth;
	}
	return bestUV;
}

// Estimate: the fog of each half texel into the two half-size targets (multiply + depth, add).
EstimateOut EstimatePS(float2 vpos : VPOS, uniform int look, uniform bool inside) {
	float2 per = max(floor(1.0f / max(NVR_FogLayout.xy, 0.01f) + 0.5f), 1.0f);
	float dN;
	float2 uv = NearestCovered(vpos, per, dN);
	float3 mul, add;
	FogTerms(uv, dN, look, inside, mul, add);
	EstimateOut o;
	o.mulDepth = float4(mul, dN);
	o.add = float4(add, 1.0f);
	return o;
}

// The fog terms at a full-resolution pixel from the half-size estimate: bilinear over the four nearest half texels, each weighed by
// how close its depth is; where none is close (edges against the sky or a near object) the pixel's own fog is worked out instead.
void UpsampleFog(float2 uv, float2 vpos, float dP, uniform int look, uniform bool inside, out float3 mul, out float3 add) {
	float2 hp = (vpos + 0.5f) * NVR_FogLayout.xy - 0.5f;
	float2 h0 = floor(hp);
	float2 t = hp - h0;
	float tol = max(0.02f * dP, 0.0005f);
	float3 sumMul = 0.0f, sumAdd = 0.0f;
	float weights = 0.0f;
	[unroll] for (int i = 0; i < 4; i++) {
		float2 o = Quad[i];
		float2 huv = clampToRectIfOn((h0 + o + 0.5f) * NVR_FogLayout.zw, NVR_FogLayout.zw);
		float4 m = LOD2(NVR_FogBuffer, huv);
		float3 a = LOD2(NVR_FogBufferAdd, huv).rgb;
		float w = (o.x > 0.5f ? t.x : 1.0f - t.x) * (o.y > 0.5f ? t.y : 1.0f - t.y) * saturate(2.0f - abs(m.a - dP) / tol);
		sumMul += w * m.rgb;
		sumAdd += w * a;
		weights += w;
	}
	mul = sumMul / max(weights, 1e-6f);
	add = sumAdd / max(weights, 1e-6f);
	[branch] if (weights < 0.25f) {
		float3 ownMul, ownAdd;
		FogTerms(uv, dP, look, inside, ownMul, ownAdd);
		float k = saturate(weights * 4.0f);
		mul = lerp(ownMul, mul, k);
		add = lerp(ownAdd, add, k);
	}
}

// Reconstruct: the composites (if any), the fog, then the volumetric light's composite when the C++ folded it in.
float4 ReconstructPS(float2 uv : TEXCOORD0, float2 vpos : VPOS, uniform int look, uniform int comp) : COLOR0 {
	float4 src = LOD2(TESR_SourceBuffer, uv);
	float alpha = src.a;
	float3 stored = src.rgb;
	if (comp != COMP_NONE) stored = CompositeScene(uv, stored, comp, alpha);
	float dP = LOD2(TESR_DepthBuffer, uv).x;
	float3 mul, add;
	UpsampleFog(uv, vpos, dP, look, comp == COMP_INTERIOR, mul, add);
	float3 lin = linearize(stored) * mul + add;
	[branch] if (NVR_FogGridFlags.y > 0.5f) lin = LightAirComposite(uv, dP, lin);
	return float4(delinearize(max(lin, 0.0f)), lerp(alpha, 1.0f, NightShare(comp == COMP_INTERIOR)));
}

// The fog at full resolution in one pass (the unnamed, Legacy and Classic techniques).
float4 FullPS(float2 uv : TEXCOORD0, uniform int look) : COLOR0 {
	float4 src = LOD2(TESR_SourceBuffer, uv);
	float dN = LOD2(TESR_DepthBuffer, uv).x;
	float3 mul, add;
	FogTerms(uv, dN, look, false, mul, add);
	float3 lin = linearize(src.rgb) * mul + add;
	return float4(delinearize(max(lin, 0.0f)), lerp(src.a, 1.0f, NightShare(false)));
}

// Sun shadow and AO only (ApplyShadowAO).
float4 ShadowAOPS(float2 uv : TEXCOORD0) : COLOR0 {
	float4 src = LOD2(TESR_SourceBuffer, uv);
	float alpha = src.a;
	float3 c = CompositeScene(uv, src.rgb, COMP_EXTERIOR, alpha);
	return float4(c, alpha);
}

// PackedFog pass 1, drawn into the top-left quarter of the target: the add term and the multiply's luminance (one target only).
float4 PackEstimatePS(float2 vpos : VPOS, uniform int look) : COLOR0 {
	float dN;
	float2 uv = NearestCovered(vpos, 2.0f, dN);
	float3 mul, add;
	FogTerms(uv, dN, look, false, mul, add);
	return float4(add, luma(mul));
}

// PackedFog pass 2: the scene (TESR_SourceBuffer, the copy made before pass 1) with the packed coefficients (TESR_RenderedBuffer's
// top-left quarter), each half texel's depth found again as its nearest covered pixel. The multiply is grey here (the aerial tint's
// hue is lost in this fallback).
float4 PackApplyPS(float2 uv : TEXCOORD0, float2 vpos : VPOS, uniform int look) : COLOR0 {
	float4 src = LOD2(TESR_SourceBuffer, uv);
	float dP = LOD2(TESR_DepthBuffer, uv).x;
	float2 halfSize = floor(0.5f / TESR_ReciprocalResolution.xy);
	float2 hp = (vpos + 0.5f) * 0.5f - 0.5f;
	float2 h0 = floor(hp);
	float2 t = hp - h0;
	float tol = max(0.02f * dP, 0.0005f);
	float4 sum = 0.0f;
	float weights = 0.0f;
	[unroll] for (int i = 0; i < 4; i++) {
		float2 o = Quad[i];
		float2 h = clamp(h0 + o, 0.0f, halfSize - 1.0f);
		float d;
		NearestCovered(h, 2.0f, d);
		float4 packed = LOD2(TESR_RenderedBuffer, (h + 0.5f) * TESR_ReciprocalResolution.xy);
		float w = (o.x > 0.5f ? t.x : 1.0f - t.x) * (o.y > 0.5f ? t.y : 1.0f - t.y) * saturate(2.0f - abs(d - dP) / tol);
		sum += w * packed;
		weights += w;
	}
	float3 mul = (sum.a / max(weights, 1e-6f)).xxx;
	float3 add = sum.rgb / max(weights, 1e-6f);
	[branch] if (weights < 0.25f) {
		float3 ownMul, ownAdd;
		FogTerms(uv, dP, look, false, ownMul, ownAdd);
		float k = saturate(weights * 4.0f);
		mul = lerp(ownMul, mul, k);
		add = lerp(ownAdd, add, k);
	}
	float3 lin = linearize(src.rgb) * mul + add;
	return float4(delinearize(max(lin, 0.0f)), lerp(src.a, 1.0f, NightShare(false)));
}

// HalfStoreProbe (64 x 16, A16B16G16R16F): r a value between two 16-bit floats (the store rounds it), g RoundToHalf to nearest
// even, b toward zero. The C++ reads back which of g and b equals r for every texel (VolumetricFogEffect::HalfStoreMode).
float4 HalfProbePS(float2 vpos : VPOS) : COLOR0 {
	float i = vpos.y * 64.0f + vpos.x;                            // 0 .. 1023
	float quarter = 0.25f + 0.25f * (i - 3.0f * floor(i / 3.0f));   // 0.25, 0.5 (a tie) or 0.75 of a 16-bit step
	float octave = i - 7.0f * floor(i / 7.0f) - 3.0f;               // 2^-3 .. 2^3
	float v = (1024.0f + i + quarter) / 1024.0f * exp2(octave);
	return float4(v, RoundToHalf(v.xxx, 0.0f).x, RoundToHalf(v.xxx, 2.0f).x, 1.0f);
}

// ==== techniques (the unnamed one first: EffectRecord::Render draws technique 0) ====

#define FULL_PASS(look) pass Fog { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 FullPS(look); }
#define TWO_PASSES(look, inside, comp) \
	pass Estimate { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 EstimatePS(look, inside); } \
	pass Reconstruct { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 ReconstructPS(look, comp); }
#define PACKED_PASSES(look) \
	pass Estimate { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PackEstimatePS(look); } \
	pass Apply { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PackApplyPS(look); }

technique { FULL_PASS(LOOK_MODERN) }
technique DedicatedFog { TWO_PASSES(LOOK_MODERN, false, COMP_NONE) }
technique CompositeFog { TWO_PASSES(LOOK_MODERN, false, COMP_EXTERIOR) }
technique CompositeFogInterior { TWO_PASSES(LOOK_MODERN, true, COMP_INTERIOR) }
technique ApplyShadowAO { pass Apply { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 ShadowAOPS(); } }
technique PackedFog { PACKED_PASSES(LOOK_MODERN) }
technique HalfStoreProbe { pass Probe { VertexShader = compile vs_3_0 ProbeVS(); PixelShader = compile ps_3_0 HalfProbePS(); } }

technique VolumetricFogLegacy { FULL_PASS(LOOK_LEGACY) }
technique DedicatedFogLegacy { TWO_PASSES(LOOK_LEGACY, false, COMP_NONE) }
technique CompositeFogLegacy { TWO_PASSES(LOOK_LEGACY, false, COMP_EXTERIOR) }
technique CompositeFogInteriorLegacy { TWO_PASSES(LOOK_LEGACY, true, COMP_INTERIOR) }
technique PackedFogLegacy { PACKED_PASSES(LOOK_LEGACY) }

technique VolumetricFogClassic { FULL_PASS(LOOK_CLASSIC) }
technique DedicatedFogClassic { TWO_PASSES(LOOK_CLASSIC, false, COMP_NONE) }
technique CompositeFogClassic { TWO_PASSES(LOOK_CLASSIC, false, COMP_EXTERIOR) }
technique CompositeFogInteriorClassic { TWO_PASSES(LOOK_CLASSIC, true, COMP_INTERIOR) }
