// VolumetricFog as Vulkan passes (needs NVR DXVK; effects/VolumetricFog.cpp ExtSetup, ExtDedicated, ExtShadowAO, ExtRender): what
// VolumetricFogEstimate.frag and VolumetricFogApply.frag share, a port of Effects/VolumetricFog.fx.hlsl (same maths, same names).
// The Vulkan path never runs in RenderScale rect frames (DxvkCompute::PassesOn), so the rect clamps of the D3D9 file are not needed.
// Matrices arrive as the D3D9 effect uploads them (SetMatrix keeps the logical matrix): HLSL mul(v, M) is M * v here, mul(M, v) is
// v * M, and M[i][j] is the same element.
//
// Compile-time variants (Compute/variants.txt, FOG_VARIANT): FOG_MODE (the Apply pass: 0 reconstruct, 1 composite, 2 interior
// composite, 3 shadow + AO only, 4 the fog at full resolution), FOG_LEGACY, FOG_CLASSIC, FOG_INTERIOR (the Estimate of the interior
// composite), FOG_PHYSICAL (the effect was compiled with PHYSICAL_SKY_ON). Without FOG_VARIANT the push block's fields decide.
// Tests (NvrDxvkTestSuffix): FOG_TEST_NO_NOISE, FOG_TEST_NO_SHADOW, FOG_TEST_NO_SKY (no sky table / SH reads: the flat sky colour).

#include "Common.glsl"
#include "SkyAtmosphere.glsl"

// Set 1: binding i = input i of VolumetricFogEffect::ExtSetup (the Estimate declares the first 8).
layout(set = 1, binding = 0) uniform texture2D t_scene;      // TESR_SourceBuffer slot: the scene this pass reads
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer
layout(set = 1, binding = 2) uniform texture2D t_atlas;      // TESR_ShadowAtlas
layout(set = 1, binding = 3) uniform texture2D t_normals;    // TESR_NormalsBuffer
layout(set = 1, binding = 4) uniform texture2D t_pointShadow; // TESR_PointShadowBuffer
layout(set = 1, binding = 5) uniform texture3D t_noise;      // NVR_FogNoise
layout(set = 1, binding = 6) uniform texture2D t_skyView;    // NVR_FogSkyView (a stand-in unless the physical sky is drawn)
layout(set = 1, binding = 7) uniform texture2D t_grid;       // NVR_FogGrid (a stand-in unless gridFlags.x)
#ifdef FOG_APPLY
layout(set = 1, binding = 8) uniform texture2D t_fog;        // NVR_FogBuffer (multiply + depth)
layout(set = 1, binding = 9) uniform texture2D t_fogAdd;     // NVR_FogBufferAdd
layout(set = 1, binding = 10) uniform texture2D t_ao;        // NVR_AOBuffer
layout(set = 1, binding = 11) uniform texture2D t_skyVis;    // NVR_SkyVis
layout(set = 1, binding = 12) uniform texture2D t_sunFar;    // NVR_SunFar
#endif

// Set 2: FogExtConstants (VolumetricFog.cpp), in its order.
layout(set = 2, binding = 0, std140) uniform FogConstants {
    mat4 shadowNear;               // TESR_ShadowCameraToLightTransformNear
    mat4 shadowMiddle;             // ...Middle
    mat4 shadowFar;                // ...Far
    mat4 shadowLod;                // ...Lod
    mat4 viewTransform;            // TESR_ViewTransform
    mat4 projectionTransform;      // TESR_ProjectionTransform
    vec4 fogWeatherColor;          // TESR_FogWeatherColor
    vec4 fogData;                  // TESR_FogData
    vec4 reciprocalResolution;     // TESR_ReciprocalResolution
    vec4 gameTime;                 // TESR_GameTime
    vec4 smoothedSunDir;           // TESR_SmoothedSunDir
    vec4 sunPosition;              // TESR_SunPosition
    vec4 sunColor;                 // TESR_SunColor
    vec4 sunDiskColor;             // TESR_SunDiskColor
    vec4 sunAmbient;               // TESR_SunAmbient
    vec4 horizonColor;             // TESR_FogHorizonColor
    vec4 skyLowColor;              // TESR_FogSkyLowColor
    vec4 skyColor;                 // TESR_FogSkyColor
    vec4 skyData;                  // TESR_SkyData
    vec4 sunsetColor;              // TESR_SunsetColor
    vec4 sunAmount;                // TESR_SunAmount
    vec4 pbrData;                  // TESR_PBRData
    vec4 shadowNearCenter;         // TESR_ShadowNearCenter
    vec4 shadowMiddleCenter;       // TESR_ShadowMiddleCenter
    vec4 shadowFarCenter;          // TESR_ShadowFarCenter
    vec4 shadowLodCenter;          // TESR_ShadowLodCenter
    vec4 shadowFormatData;         // TESR_ShadowFormatData
    vec4 shadowBlur;               // TESR_ShadowBlur
    vec4 shadowFade;               // TESR_ShadowFade
    vec4 fDensity;                 // TESR_VolumetricFogDensity
    vec4 fShape;                   // TESR_VolumetricFogShape
    vec4 fWind;                    // TESR_VolumetricFogWind
    vec4 fScatter;                 // TESR_VolumetricFogScatter
    vec4 fWeather;                 // TESR_VolumetricFogWeather
    vec4 fAerial;                  // TESR_VolumetricFogAerial
    vec4 fAerialTint;              // TESR_VolumetricFogAerialTint
    vec4 fDistant;                 // TESR_VolumetricFogDistant
    vec4 fGlobal;                  // TESR_VolumetricFogGlobal
    vec4 fNight;                   // TESR_VolumetricFogNight
    vec4 fNightScatter;            // TESR_VolumetricFogNightScatter
    vec4 fSun;                     // TESR_VolumetricFogSun
    vec4 fNightTint;               // TESR_VolumetricFogNightTint
    vec4 physSky;                  // TESR_PhysSky
    vec4 physSkySun;               // TESR_PhysSkySun
    vec4 vlData;                   // TESR_VolumetricLightData
    vec4 vlData2;                  // TESR_VolumetricLightData2
    vec4 gridLayout;               // TESR_VolumetricLightGridLayout
    vec4 gridData;                 // TESR_VolumetricLightGridData
    vec4 lLow;                     // TESR_VolumetricFogLow
    vec4 lHigh;                    // TESR_VolumetricFogHigh
    vec4 lSimple;                  // TESR_VolumetricFogSimple
    vec4 lBlend;                   // TESR_VolumetricFogBlend
    vec4 lHeight;                  // TESR_VolumetricFogHeight
    vec4 lData;                    // TESR_VolumetricFogData
    vec4 shadowData;               // TESR_ShadowData
    vec4 waterSettings;            // TESR_WaterSettings
    vec4 aoAOData;                 // TESR_AmbientOcclusionAOData
    vec4 aoData;                   // TESR_AmbientOcclusionData
    vec4 cameraData;               // TESR_CameraData
    vec4 cameraPosition;           // TESR_CameraPosition
    vec4 skyIrradiance[9];         // TESR_SkyIrradiance
    vec4 skyIrradianceGame[9];     // TESR_SkyIrradianceGame
    vec4 lin[9];                   // linearized on the CPU: FogExtLinearNames
} cb;

#define LIN_WEATHER (cb.lin[0].rgb)
#define LIN_SKY (cb.lin[1].rgb)
#define LIN_SKYLOW (cb.lin[2].rgb)
#define LIN_HORIZON (cb.lin[3].rgb)
#define LIN_SUNSET (cb.lin[5].rgb)
#define LIN_AMBIENT (cb.lin[6].rgb)
#define LIN_SUN (cb.lin[8].rgb)

// VolumetricFogEffect::ExtFogPush
layout(push_constant) uniform Push {
    uint pointIndex;               // 0: POINT, CLAMP
    uint linearIndex;              // 4: LINEAR, CLAMP
    uint wrapIndex;                // 8: LINEAR, WRAP (the noise)
    uint mode;                     // 12: the Apply pass (0 reconstruct, 1 composite, 2 interior composite, 3 shadow + AO, 4 full)
    uint legacy;                   // 16
    uint classic;                  // 20
    uint interior;                 // 24: the Estimate of the interior composite
    uint physicalSky;              // 28
    vec4 fogLayout;                // 32: NVR_FogLayout
    vec4 compositeFlags;           // 48: NVR_CompositeFlags
    vec4 compositeAOTexel;         // 64: NVR_CompositeAOTexel
    vec4 gridFlags;                // 80: NVR_FogGridFlags
    vec4 sunFarFlags;              // 96: NVR_SunFarFlags
} pc;

#ifdef FOG_VARIANT
    #define V_MODE FOG_MODE
    #define V_LEGACY (FOG_LEGACY != 0)
    #define V_CLASSIC (FOG_CLASSIC != 0)
    #define V_INTERIOR (FOG_INTERIOR != 0)
    #define V_PHYSICAL (FOG_PHYSICAL != 0)
#else
    #define V_MODE int(pc.mode)
    #define V_LEGACY (pc.legacy != 0u)
    #define V_CLASSIC (pc.classic != 0u)
    #define V_INTERIOR (pc.interior != 0u)
    #define V_PHYSICAL (pc.physicalSky != 0u)
#endif
#define LOOK_MODERN 0
#define LOOK_LEGACY 1
#define LOOK_CLASSIC 2
#define COMP_NONE 0
#define COMP_EXTERIOR 1
#define COMP_INTERIOR 2
#define V_LOOK (V_LEGACY ? LOOK_LEGACY : (V_CLASSIC ? LOOK_CLASSIC : LOOK_MODERN))

layout(location = 0) in vec2 uv;

#define POINT_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.pointIndex)), (coord), 0.0)
#define LINEAR_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.linearIndex)), (coord), 0.0)

#define farZ (cb.cameraData.y)
const float SkyDepth = 0.9961;
const float NoiseTile = 6000.0;
const float FogPi = 3.1415926538;
const vec2 Quad[4] = vec2[4](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));

vec2 saturate(vec2 x) { return clamp(x, vec2(0.0), vec2(1.0)); }
vec4 saturate(vec4 x) { return clamp(x, vec4(0.0), vec4(1.0)); }
float sqr(float x) { return x * x; }

// Includes/Depth.hlsl toWorld
vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}
float readDepthLod(vec2 coord) { return POINT_LOD(t_depth, coord).x * farZ; }
// Includes/Normals.hlsl GetWorldNormalLod (HLSL mul(View, n))
vec3 GetWorldNormalLod(vec2 coord) {
    vec4 n = vec4(LINEAR_LOD(t_normals, coord).xyz * 2.0 - 1.0, 1.0);
    return (n * cb.viewTransform).xyz;
}

float NightShare(bool inside) {
    return inside ? 0.0 : cb.fWeather.y * (1.0 - smoothstep(0.4, 0.8, cb.sunAmount.x));
}

vec3 SkyUp(bool game) {
    return game ? max(cb.skyIrradianceGame[0].rgb + cb.skyIrradianceGame[2].rgb + 2.0 * cb.skyIrradianceGame[6].rgb, vec3(0.0))
                : max(cb.skyIrradiance[0].rgb + cb.skyIrradiance[2].rgb + 2.0 * cb.skyIrradiance[6].rgb, vec3(0.0));
}

vec3 TintOf(vec3 c) { return c / max(max(c.r, max(c.g, c.b)), 1e-4); }

float SkyNeighbour(vec2 coord) {
    vec2 t = cb.reciprocalResolution.xy;
    vec4 d = vec4(POINT_LOD(t_depth, coord + vec2(t.x, 0.0)).x, POINT_LOD(t_depth, coord - vec2(t.x, 0.0)).x,
                  POINT_LOD(t_depth, coord + vec2(0.0, t.y)).x, POINT_LOD(t_depth, coord - vec2(0.0, t.y)).x);
    return step(SkyDepth, max(max(d.x, d.y), max(d.z, d.w)));
}

float HeightRho(float z, float k, float base, float layer) {
    float h = z - base;
    return exp(clamp(-k * (layer > 0.5 ? max(h, 0.0) : h), -30.0, 30.0));
}

float HeightPathMean(float zc, float zp, float k, float base, float layer) {
    float dz = zp - zc;
    float mid = HeightRho(0.5 * (zc + zp), k, base, layer);
    float mean;
    if (layer > 0.5) {
        float hc = zc - base, hp = zp - base;
        float fc = hc <= 0.0 ? hc : (1.0 - exp(-k * hc)) / k;
        float fp = hp <= 0.0 ? hp : (1.0 - exp(-k * hp)) / k;
        mean = (fp - fc) / (abs(dz) < 1.0 ? 1.0 : dz);
    }
    else {
        float x = clamp(k * dz, -30.0, 30.0);
        float f = abs(x) < 1e-3 ? 1.0 - 0.5 * x : (1.0 - exp(-x)) / x;
        mean = HeightRho(zc, k, base, 0.0) * f;
    }
    return clamp(abs(dz) < 1.0 ? mid : mean, 0.0, 1e4);
}

float NoiseAt(vec3 rel, vec3 wind, float scale) {
#ifdef FOG_TEST_NO_NOISE
    return 0.5;
#else
    vec3 c = (cb.cameraPosition.xyz + rel + wind) / (NoiseTile * scale);
    return textureLod(sampler3D(t_noise, NVR_SAMPLER(pc.wrapIndex)), c, 0.0).g;
#endif
}

// The froxel grid's sums from the camera to planar depth (see the .fx GridSums).
vec4 GridSums(vec2 s01, float planar) {
    float march = max(cb.vlData.w, 1.0);
    float s = sqrt(saturate(planar / march)) * 64.0;
    float z = min(floor(s), 63.0);
    vec2 cells = cb.gridLayout.xy;
    vec2 local = clamp(s01 * cells, vec2(0.5), cells - 0.5);
    vec2 tileFar = vec2(z - 8.0 * floor(z / 8.0), floor(z / 8.0));
    float zn = max(z - 1.0, 0.0);
    vec2 tileNear = vec2(zn - 8.0 * floor(zn / 8.0), floor(zn / 8.0));
    vec4 farSide = LINEAR_LOD(t_grid, (tileFar * cells + local) * cb.gridLayout.zw);
    vec4 nearSide = z > 0.5 ? LINEAR_LOD(t_grid, (tileNear * cells + local) * cb.gridLayout.zw) : vec4(0.0);
    return mix(nearSide, farSide, saturate(s - z));
}

vec3 PhysicalSky(vec3 d) {
    vec2 sunH = cb.physSkySun.xy / max(length(cb.physSkySun.xy), 1e-5);
    vec2 dirH = d.xy / max(length(d.xy), 1e-5);
    float bearing = acos(clamp(dot(dirH, sunH), -1.0, 1.0));
    float elevation = asin(saturate(d.z));
    vec2 coord = vec2(bearing / FogPi, sqrt(elevation / (0.5 * FogPi)));
    return LINEAR_LOD(t_skyView, coord).rgb * (0.001 * cb.physSky.y);
}
vec3 PhysicalBlend(vec3 game, vec3 phys) {
    vec3 tinted = phys + (game * (luma(phys) / max(luma(game), 1e-6)) - phys) * cb.physSky.z;
    return mix(game, tinted, cb.physSky.x);
}

// Includes/Shadows.hlsl (the parts the cascade lookup uses) and Includes/SunCascades.hlsl SunCascadeValue
float Linstep(float a, float b, float v) { return saturate((v - a) / (b - a)); }
float ChebyshevUpperBound(vec2 moments, float mean, float minVariance, float bleed) {
    float variance = max(moments.y - moments.x * moments.x, minVariance);
    float d = mean - moments.x;
    float pMax = Linstep(bleed, 1.0, variance / (variance + d * d));
    return mean <= moments.x ? 1.0 : pMax;
}
vec2 EVSMExponents(float formatBits) { return min(vec2(40.0, 5.0), vec2(formatBits == 0.0 ? 5.54 : 42.0)); }
vec2 WarpDepth(float depth, vec2 exponents) {
    depth = 2.0 * depth - 1.0;
    return vec2(exp(exponents.x * depth), -exp(-exponents.y * depth));
}
float ShadowPCF2x2(vec2 coord, float depth, float texel) {
    vec2 p = coord / texel - 0.5;
    vec2 base = floor(p);
    vec2 f = p - base;
    vec2 uv0 = (base + 0.5) * texel;
    vec4 d = vec4(LINEAR_LOD(t_atlas, uv0).x, LINEAR_LOD(t_atlas, uv0 + vec2(texel, 0.0)).x,
                  LINEAR_LOD(t_atlas, uv0 + vec2(0.0, texel)).x, LINEAR_LOD(t_atlas, uv0 + vec2(texel)).x);
    vec4 lit = vec4(lessThanEqual(vec4(depth), d));
    return mix(mix(lit.x, lit.y, f.x), mix(lit.z, lit.w, f.x), f.y);
}
float SunCascadeValue(mat4 lightTransform, vec4 coord, vec2 quadrant, float bias, float bleed) {
    vec4 ls = lightTransform * coord;
    ls.xyz /= ls.w;
    ls.x = ls.x * 0.5 + 0.5;
    ls.y = ls.y * -0.5 + 0.5;
    ls.xy = ls.xy * 0.5 + quadrant;
    if (cb.shadowFormatData.x == 3.0) return ShadowPCF2x2(ls.xy, ls.z - 0.0002, cb.shadowBlur.x);
    vec4 moments = LINEAR_LOD(t_atlas, ls.xy);
    if (cb.shadowFormatData.x == 0.0) return ChebyshevUpperBound(moments.xy, ls.z, bias, bleed);
    vec2 e = EVSMExponents(cb.shadowFormatData.y);
    vec2 warped = WarpDepth(ls.z, e);
    vec2 minVariance = sqr(bias) * vec2(sqr(e.x * warped.x), sqr(e.y * warped.y));
    float positive = ChebyshevUpperBound(moments.xy, warped.x, minVariance.x, bleed);
    if (cb.shadowFormatData.x == 1.0) return positive;
    return min(ChebyshevUpperBound(moments.xz, warped.x, minVariance.x, bleed), ChebyshevUpperBound(moments.yw, warped.y, minVariance.y, bleed));
}

// The .fx FogSunVisibility: one lookup in the first cascade holding the point, lit past the last one.
float FogSunVisibility(vec3 p, vec3 n) {
    vec4 radii = vec4(cb.shadowNearCenter.w, cb.shadowMiddleCenter.w, cb.shadowFarCenter.w, cb.shadowLodCenter.w);
    vec4 d = vec4(length(p - cb.shadowNearCenter.xyz), length(p - cb.shadowMiddleCenter.xyz),
                  length(p - cb.shadowFarCenter.xyz), length(p - cb.shadowLodCenter.xyz));
    mat4 m = cb.shadowLod;
    vec4 pick = vec4(0.5, 0.5, 0.8, radii.w);
    if (d.x < radii.x) { m = cb.shadowNear; pick = vec4(0.0, 0.0, 0.1, radii.x); }
    else if (d.y < radii.y) { m = cb.shadowMiddle; pick = vec4(0.5, 0.0, 0.2, radii.y); }
    else if (d.z < radii.z) { m = cb.shadowFar; pick = vec4(0.0, 0.5, 0.6, radii.z); }
    else if (d.w >= radii.w) return 1.0;
    float offsetScale = saturate(1.0 - dot(n, cb.smoothedSunDir.xyz));
    float offset = offsetScale * 10.0 * pick.w * max(cb.shadowBlur.x, 1.0 / 16384.0);
    float bias = (cb.shadowFormatData.x == 0.0 ? 0.00001 : 0.01) * (1.0 + offsetScale);
    float v = SunCascadeValue(m, vec4(p + offset * n, 1.0), pick.xy, bias, pick.z);
    return mix(v, 1.0, smoothstep(radii.w * 0.9, radii.w, d.w) * step(radii.z, d.z));
}

// The .fx RoundToHalf: a 16-bit render-target store's rounding (mode 0 nearest even, 2 toward zero, 1 none).
vec3 RoundToHalf(vec3 c, float mode) {
    if (mode > 0.5 && mode < 1.5) return c;
    vec3 a = min(abs(c), vec3(65504.0));
    vec3 m = max(a, vec3(6.103515625e-05));
    vec3 e = floor(log2(m));
    e = mix(e, e - 1.0, greaterThan(exp2(e), m));
    e = mix(e, e + 1.0, lessThanEqual(exp2(e + 1.0), m));
    vec3 ulp = exp2(e - 10.0);
    vec3 q = a / ulp;
    vec3 lo = floor(q);
    vec3 rem = q - lo;
    vec3 odd = lo - 2.0 * floor(0.5 * lo);
    vec3 up = vec3(greaterThan(rem, vec3(0.5))) + vec3(equal(rem, vec3(0.5))) * vec3(greaterThan(odd, vec3(0.5)));
    vec3 r = (mode > 1.5 ? lo : lo + min(up, vec3(1.0))) * ulp;
    return mix(r, -r, lessThan(c, vec3(0.0)));
}

// Includes/Sky.hlsl GetSunColor, on the CPU-linearized colours
vec3 SunColorAt(float sunHeight, float atmosphere, float dayTime) {
    return (1.0 + sunHeight) * LIN_SUN + LIN_SUNSET * (saturate(pows(1.0 - sunHeight, 8.0)) * dayTime * atmosphere);
}

// ==== the .fx FogTerms ====
void FogTerms(vec2 tex, float dN, int look, bool inside, out vec3 mulOut, out vec3 addOut) {
    bool modern = look == LOOK_MODERN;
    bool classic = look == LOOK_CLASSIC;
    bool legacy = look == LOOK_LEGACY;
    float isExt = inside ? 0.0 : cb.fWeather.y;
    float nf = NightShare(inside);
    float sunAmt = cb.sunAmount.x;
    float dayFade = smoothstep(0.1, 0.6, sunAmt);

    float viewZ = max(dN * farZ, 1.0);
    vec3 rel = toWorld(tex) * viewZ;
    float dist = max(length(rel), 1.0);
    vec3 dir = rel / dist;
    float zc = cb.cameraPosition.z;
    float zp = zc + rel.z;
    float sky = dN >= SkyDepth ? 1.0 : 0.0;

    float fogPow = max(mix(1.0, cb.fogData.w, saturate(cb.fDensity.y)), 0.05);
    vec3 sunPos = cb.sunPosition.xyz / max(length(cb.sunPosition.xyz), 1e-5);
    float sunH = saturate(sunPos.z);
    vec2 weatherNF = saturate(1.0 - cb.fogData.xy / max(farZ, 1.0));
    float anchor = sqr(0.5 * (weatherNF.x + weatherNF.y)) / (fogPow + 1.0);

    vec3 skyTop = LIN_SKY;
    vec3 horizon = LIN_HORIZON;
    vec3 skyHaze = mix(horizon, LIN_SKYLOW, smoothstep(0.0, 0.35, saturate(dir.z)));
    skyHaze = mix(skyTop, skyHaze, isExt);
    horizon = mix(skyTop, horizon, isExt);
    vec3 weatherCol = LIN_WEATHER;
    vec3 sunLight = SunColorAt(sunH, cb.skyData.x, sunAmt) * cb.pbrData.z;
#ifndef FOG_TEST_NO_SKY
    if (V_PHYSICAL && modern && !inside && cb.physSky.x > 0.0 && isExt > 0.5) {
        skyHaze = PhysicalBlend(skyHaze, PhysicalSky(dir));
        horizon = PhysicalBlend(horizon, PhysicalSky(vec3(dir.xy, 0.0)));
        vec3 through = exp(-AtmSunDepth(AtmGround + 300.0, cb.physSkySun.z, cb.physSky.w));
        vec3 physSun = luma(sunLight) * through / max(luma(through), 1e-4);
        sunLight = mix(sunLight, mix(physSun, sunLight, cb.physSky.z), cb.physSky.x);
    }
#endif
    vec3 ambient = skyTop;
#ifndef FOG_TEST_NO_SKY
    if (cb.fWeather.z * isExt > 0.5) ambient = SkyUp(classic);
#endif
    ambient *= cb.pbrData.w;
    ambient = max(ambient, LIN_AMBIENT * (cb.fGlobal.z * cb.fNightTint.w * nf));

    float shadowUse = saturate(cb.fScatter.y) * isExt * step(0.5, cb.shadowFade.y) * (1.0 - saturate(cb.shadowFade.x)) * dayFade;
    float sunVis = 1.0;

    float T;
    vec3 inscatter;
    vec3 hazeCol;
    float amount;
    vec4 distant;

    if (legacy) {
        float nightDist = mix(1.0, cb.lSimple.z, nf);
        float nearL = cb.fogData.x * nightDist;
        float farL = max(cb.fogData.y * nightDist, max(nearL + 1.0, 1.0));
        float powL = max(cb.fogData.w, 0.05);
        float weatherCover = pow(saturate((dist - nearL) / (farL - nearL)), powL) * saturate(cb.lLow.y);
        float extL = max(cb.lSimple.x, 0.0) * 0.1;
        float base = max(cb.lData.x, 0.0);
        float simpleH = HeightPathMean(zc, zp, max(cb.lHeight.z, 0.0) * 1e-3, zc, 0.0);
        float odS = base * extL * dist * 1e-4 * simpleH;
        float coverS = (1.0 - exp(-odS) * (1.0 - weatherCover)) * saturate(cb.lBlend.w);
        float kL = 1.5e-4 / (powL * max(cb.lHigh.y, 1e-4));
        float seaL = -10000.0 + 1000.0 * cb.lHeight.y;
        float pushed = farL * pow(max(dist * cb.lHigh.z / farL, 0.0), max(cb.lBlend.z, 0.05));
        float odH = max(cb.lHigh.x, 0.0) * extL * pushed * 1e-4 * HeightPathMean(zc, zp, kL, seaL, 0.0);
        float coverH = (1.0 - exp(-odH)) * saturate(cb.lBlend.y);
        T = (1.0 - coverS) * (1.0 - coverH);
        float wS = saturate(coverS * 10.0 / max(cb.lSimple.w, 0.01));
        float wH = saturate(coverH * 10.0 / max(cb.lHigh.w, 0.01));
        vec3 colS = mix(skyHaze, weatherCol, inside ? 1.0 : wS);
        vec3 colH = mix(skyHaze, weatherCol, inside ? 1.0 : wH);
        colS = mix(vec3(luma(colS)), colS, max(cb.lLow.x, 0.0));
        colH = mix(vec3(luma(colH)), colH, max(cb.lData.y, 0.0));
        colS = max(mix(colS, ambient, 0.5), vec3(0.0)) * mix(vec3(1.0), cb.fNightTint.rgb, nf);
        colH = max(mix(colH, ambient, 0.5), vec3(0.0)) * mix(vec3(1.0), cb.fNightTint.rgb, nf);
        float albS = saturate(max(cb.lSimple.y, 0.0) * 0.1 / max(extL, 1e-4));
        float albH = saturate(max(cb.lData.w, 0.0) * 0.1 / max(extL, 1e-4));
        float cosS = dot(dir, cb.smoothedSunDir.xyz);
        vec3 sunL = sunLight * (max(cb.lLow.w, 0.0) * pow(saturate(0.5 + 0.5 * cosS), 4.0) * dayFade * isExt);
        float coverAll = max(1.0 - T, 1e-4);
        inscatter = ((colS * (albS * coverS * (1.0 - coverH)) + colH * (albH * coverH)) / coverAll) + sunL;
        hazeCol = colS;
        amount = saturate(cb.lData.z * mix(1.0, cb.fNight.y, nf));
        distant = vec4(cb.lLow.z, cb.lBlend.x, cb.lHeight.x, 0.0);
    }
    else {
        float timeScale = 1.0 - saturate(cb.fDensity.z) * isExt * sunH * dayFade;
        float nightDensity = mix(1.0, cb.fNight.x, nf);
        float transition = 4.0 * saturate(sunAmt) * (1.0 - saturate(sunAmt)) * isExt;
        float S = max(cb.fDensity.x * timeScale * nightDensity + cb.fDensity.y * anchor + cb.fDensity.w * transition, 0.0);

        float layer = cb.fShape.x < 0.0 ? 1.0 : 0.0;
        float hk = layer > 0.5 ? -cb.fShape.x
            : 1.5e-4 / (fogPow * max(cb.fShape.x, 1e-4) * mix(1.0, max(cb.fNight.z, 0.0), nf));
        float hbase = layer > 0.5 ? cb.fShape.y : -10000.0 + 1000.0 * (cb.fShape.y + cb.fNight.w * nf);
        float influence = saturate(cb.fScatter.w);
        float shape = mix(1.0, HeightPathMean(zc, zp, hk, hbase, layer), influence);

        float ext = max(cb.fShape.z, 0.0) * 0.1 * mix(1.0, cb.fNightScatter.z, nf);
        float ins = max(cb.fShape.w, 0.0) * 0.1 * mix(1.0, cb.fNightScatter.w, nf);
        float ref = max(cb.fogData.y, 1000.0);
        float flatOD = S * ext * ref * pow(dist / ref, fogPow) * 1e-4;
        float floorOD = saturate(cb.fGlobal.w) * flatOD;
        float plainOD = max(flatOD * shape, floorOD);

        float gridOn = 0.0;
        vec4 grid = vec4(0.0);
        float gridCover = 0.0;
        if (modern && !inside) {
            gridOn = pc.gridFlags.x * cb.gridData.x * isExt;
            if (gridOn > 0.5) {
                float march = max(cb.vlData.w, 1.0);
                grid = GridSums(tex, sky > 0.5 ? march : min(viewZ, march));
                float c = saturate(march / viewZ);
                gridCover = plainOD < 1e-4 ? c : (1.0 - exp(-plainOD * c)) / max(1.0 - exp(-plainOD), 1e-5);
                gridCover *= step(1e-6, grid.g);
            }
        }

        float noiseK = saturate(cb.fScatter.z * mix(1.0, cb.fNightScatter.x, nf));
        float noise = 0.5;
        if (noiseK > 0.001 && sky < 0.5) {
            float scale = max(cb.fWind.w, 1e-4);
            vec3 wind = vec3(cb.fWind.xy * (cb.fWind.z * mix(1.0, max(cb.fNightScatter.y, 0.0), nf) * cb.gameTime.x * 0.002), 0.0);
            if (classic) {
                float n = 0.5 * NoiseAt(rel, wind, scale) + 0.3 * NoiseAt(rel * 2.03, wind, scale) + 0.2 * NoiseAt(rel * 4.01, wind, scale);
                noise = saturate((n - 0.5) * 1.8 + 0.5);
            }
            else {
                float marched = 0.5;
                if (gridCover < 0.999) {
                    float tMax = min(dist, 12000.0);
                    float sum = 0.0, weights = 0.0;
                    for (int i = 0; i < 4; i++) {
                        float t = tMax * (float(i) + 0.5) * 0.25;
                        float w = mix(1.0, HeightRho(zc + dir.z * t, hk, hbase, layer), influence) + 1e-4;
                        sum += w * NoiseAt(dir * t, wind, scale);
                        weights += w;
                    }
                    marched = sum / weights;
                }
                float gridNoise = grid.b / max(grid.g, 1e-6);
                noise = saturate((mix(marched, gridNoise, gridCover) - 0.5) * 1.8 + 0.5);
            }
        }
        float od = max(plainOD * max(1.0 + noiseK * (2.0 * noise - 1.0), 0.0), floorOD);
        T = exp(-od);

        float useWeather = modern ? saturate(cb.fSun.y) * (1.0 - sky) : 0.0;
        vec3 weatherSide = mix(skyHaze, weatherCol, useWeather);
        vec3 col = mix(ambient, weatherSide, saturate(cb.fDensity.y));
        col = mix(col, mix(weatherCol, skyHaze, sky), 1.0 - exp(-6.0 * S));
        col = mix(vec3(luma(col)), col, max(cb.fWeather.w, 0.0) / (1.0 + S));
        col = max(col, vec3(0.0)) * mix(vec3(1.0), cb.fNightTint.rgb, nf);

#ifndef FOG_TEST_NO_SHADOW
        if (shadowUse > 0.001) {
            if (!classic && gridOn > 0.5) sunVis = mix(1.0, grid.a / max(grid.g, 1e-6), gridCover);
            else if (sky < 0.5) sunVis = FogSunVisibility(rel, normalize(GetWorldNormalLod(tex)));
            sunVis = mix(1.0, saturate(sunVis), shadowUse);
        }
#endif
        float g = saturate(cb.fScatter.x);
        float cosS = dot(dir, cb.smoothedSunDir.xyz);
        float hg = min((1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosS, 1e-4), 1.5), 8.0);
        float glow = (classic ? 1.0 : cb.fSun.x) * 2.0 * max(hg - 1.0, 0.0) * sqr(1.0 - sunH) * (1.0 + cb.fogData.z * dN) * dayFade * isExt;
        float sunShare = 0.35 * dayFade * isExt;
        hazeCol = col * (1.0 - sunShare + sunShare * sunVis);
        inscatter = (hazeCol + sunLight * (glow * sunVis)) * saturate(ins / max(ext, 1e-4));
        amount = saturate(cb.fGlobal.x * mix(1.0, cb.fNight.y, nf));
        distant = cb.fDistant;
    }

    float skyMask = mix(1.0 - sky * saturate(cb.fWeather.x), 1.0, nf * saturate(cb.fGlobal.y));
    vec3 mulV = vec3(mix(1.0, T, skyMask));
    vec3 addV = inscatter * ((1.0 - T) * skyMask);

    if (!legacy && !inside) {
        float aer = saturate(cb.fAerial.x * smoothstep(min(saturate(cb.fAerial.y), 0.999), 1.0, dN) * (1.0 - sky) *
            smoothstep(0.1, max(cb.fAerial.w, 0.11), sunAmt) * isExt);
        vec3 tint = saturate(mix(TintOf(skyTop), cb.fAerialTint.rgb, saturate(cb.fAerial.z)));
        mulV *= mix(vec3(1.0), tint, aer);
    }

    if (!inside && isExt > 0.5 && (distant.y > 0.0 || distant.w > 0.0)) {
        float start = min(max(exp(-4.0 * distant.x), 1e-8), 0.999);
        float reach = sqrt(smoothstep(start, 1.0, dN));
        float heightFade = distant.z > 0.0 ? min(1.0, exp(-zp / (80000.0 * distant.z))) : 1.0 - sky;
        float hf = saturate(reach * distant.y * heightFade);
        float edge = 0.0;
        if (distant.w > 0.0 && sky < 0.5) edge = saturate(distant.w * 0.5) * SkyNeighbour(tex) * reach;
        float k = saturate(hf + edge - hf * edge);
        mulV *= 1.0 - k;
        addV = addV * (1.0 - k) + horizon * k;
    }

    mulV = mix(vec3(1.0), mulV, amount);
    addV *= amount;

    if (modern && !inside && cb.fSun.z > 0.0 && isExt > 0.5 && sky < 0.5) {
        float nearG = cb.fSun.w;
        float gameFog = saturate(cb.fSun.z * pow(saturate((dist - nearG) / max(cb.fogData.y - nearG, 1.0)), max(cb.fogData.w, 0.01)));
        float covered = luma(mulV);
        float top = saturate(1.0 - (1.0 - gameFog) / max(covered, 1e-4));
        mulV *= 1.0 - top;
        addV = addV * (1.0 - top) + hazeCol * top;
    }

    mulOut = saturate(mulV);
    addOut = max(addV, vec3(0.0));
}

// The full-resolution pixel a half texel stands for (the nearest of those it covers).
vec2 NearestCovered(vec2 halfPixel, vec2 per, out float depth) {
    vec2 base = halfPixel * per;
    vec2 bestUV = vec2(0.0);
    depth = 2.0;
    for (int i = 0; i < 4; i++) {
        vec2 c = (base + Quad[i] * (per - 1.0) + 0.5) * cb.reciprocalResolution.xy;
        float d = POINT_LOD(t_depth, c).x;
        if (d < depth) { bestUV = c; depth = d; }
    }
    return bestUV;
}

#ifdef FOG_APPLY
// ==== the composites (the .fx SunShadowComposite, AOComposite, CompositeScene, LightAirComposite) ====
vec3 SunShadowComposite(vec2 tex, vec3 frame) {
    if (pc.sunFarFlags.x > 0.5) frame *= POINT_LOD(t_sunFar, tex).rgb;
    vec2 terms = LINEAR_LOD(t_pointShadow, tex).rg;
    float sun = mix(terms.r, 1.0, saturate(cb.shadowFade.x));
    float lit = saturate(sun + terms.g * saturate(cb.shadowFade.z));
    float f = mix(max(0.0, 1.0 - cb.shadowData.y), 1.0, lit);
    if (cb.waterSettings.z > 0.5) {
        float viewZ = readDepthLod(tex);
        float h = cb.cameraPosition.z + toWorld(tex).z * viewZ;
        vec3 n = normalize(GetWorldNormalLod(tex));
        f = (abs(h - cb.waterSettings.x) <= 2.0 && dot(n, vec3(0.0, 0.0, -1.0)) >= 0.999) ? 1.0 : f;
    }
    vec3 linC = pow(max(frame, vec3(0.0)), vec3(2.2));
    vec3 skyLin = pow(max(cb.skyColor.rgb, vec3(0.0)), vec3(2.2));
    vec3 shaded = mix(luma(linC) * f * skyLin, linC * f, saturate(f + 0.5));
    return pow(max(shaded, vec3(0.0)), vec3(1.0 / 2.2));
}

vec3 SkyTake(vec3 linearColor, float factor, float visibility, float cap, float noLimit) {
    float take = min(1.0 - factor, ((1.0 - visibility) * cap + noLimit) / max(luma(linearColor), 1.0e-6));
    return linearColor * (1.0 - take);
}

vec3 AOComposite(vec2 tex, vec3 frame) {
    float viewZ = readDepthLod(tex);
    if (viewZ >= 8000.0) return frame;
    vec2 texel = pc.compositeAOTexel.xy;
    vec2 p = tex / texel - 0.5;
    vec2 cell = floor(p);
    vec2 f = p - cell;
    float drop = max(cb.aoData.z, 0.001);
    float sum = 0.0, weights = 0.0;
    vec2 skySum = vec2(0.0);
    vec4 w4;
    vec2 taps[4];
    for (int i = 0; i < 4; i++) {
        vec2 o = Quad[i];
        taps[i] = clamp((cell + o + 0.5) * texel, 0.5 * texel, 1.0 - 0.5 * texel);
        vec2 ao = LINEAR_LOD(t_ao, taps[i]).rg;
        float w = (o.x > 0.5 ? f.x : 1.0 - f.x) * (o.y > 0.5 ? f.y : 1.0 - f.y);
        w /= 1.0 + abs(ao.y - viewZ) / drop;
        w4[i] = w;
        sum += ao.x * w;
        weights += w;
    }
    float ao = mix(cb.aoAOData.z, 1.0, sum / max(weights, 1e-6));
    vec3 linC = pows(frame, 2.2);
    ao = mix(ao, 1.0, saturate((luma(linC) - cb.aoData.y) * 3.0));
    vec3 result = frame * pow(abs(ao), 1.0 / 2.2);
    if (pc.compositeFlags.z > 0.5) {
        for (int j = 0; j < 4; j++) skySum += POINT_LOD(t_skyVis, taps[j]).rb * w4[j];
        vec2 skyV = saturate(skySum / max(weights, 1e-6));
        result = pows(SkyTake(pows(result, 2.2), skyV.x, skyV.y, pc.compositeAOTexel.z, pc.compositeAOTexel.w), 1.0 / 2.2);
    }
    return result;
}

// Includes/InteriorShadowApply.hlsl ApplyInteriorShadowDepth (gamma-encoded in and out)
vec3 InteriorShadow(vec2 tex, vec3 color, float shadowSample) {
    vec3 c = pows(color, 2.2);
    float uniformDepth = length(readDepthLod(tex) * toWorld(tex));
    float shadow = saturate(shadowSample);
    shadow = mix(shadow, 1.0, saturate((uniformDepth - 300.0) / (cb.shadowFade.w - 300.0)));
    float darkness = (cb.shadowData.y * 5.0) / (1.0 + cb.shadowData.y * 5.0);
    darkness = saturate(1.0 - darkness * cb.shadowFade.y);
    shadow = saturate(mix(darkness, 1.0, shadow));
    float shadowPower = 1.0 - shadow;
    vec4 c4 = vec4(c, 1.0);
    vec4 fin = saturate(shadow * pow(saturate(c4), vec4(1.0 - shadowPower * 0.2)));
    fin = mix(vec4(luma(fin)), fin, 1.0 + shadowPower * 0.3);
    fin += max(c4 - 1.0, vec4(0.0));
    return pows(fin.rgb, 1.0 / 2.2);
}

vec3 CompositeScene(vec2 tex, vec3 frame, int comp, inout float alpha) {
    if (comp == COMP_INTERIOR) {
        float lamps = LINEAR_LOD(t_pointShadow, tex).r;
        frame = RoundToHalf(InteriorShadow(tex, frame, lamps), pc.compositeFlags.w);
        alpha = 1.0;
    }
    else if (pc.compositeFlags.x > 0.5) { frame = SunShadowComposite(tex, frame); alpha = 1.0; }
    if (pc.compositeFlags.y > 0.5) { frame = AOComposite(tex, frame); alpha = 1.0; }
    return frame;
}

vec3 LightAirComposite(vec2 tex, float dN, vec3 linC) {
    float march = max(cb.vlData.w, 1.0);
    float viewZ = dN * farZ;
    float planar = dN >= 0.996 ? march : min(viewZ, march);
    vec3 ray = toWorld(tex);
    float lit = GridSums(tex, planar).r / max(cb.gridData.y, 1e-6);
    float air = 1.0 - exp(-cb.vlData.z * length(ray) * planar);
    float g = cb.vlData2.y;
    float cosT = dot(normalize(ray), cb.smoothedSunDir.xyz);
    float phase = min((1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosT, 1e-4), 1.5), 8.0);
    float fade = cb.vlData2.z;
    return linC * (1.0 - cb.vlData.y * fade * saturate(air - lit)) + LIN_SUN * (cb.vlData.x * fade * phase * lit);
}

// The .fx UpsampleFog
void UpsampleFog(vec2 tex, vec2 vpos, float dP, int look, bool inside, out vec3 mulV, out vec3 addV) {
    vec2 hp = (vpos + 0.5) * pc.fogLayout.xy - 0.5;
    vec2 h0 = floor(hp);
    vec2 t = hp - h0;
    float tol = max(0.02 * dP, 0.0005);
    vec3 sumMul = vec3(0.0), sumAdd = vec3(0.0);
    float weights = 0.0;
    for (int i = 0; i < 4; i++) {
        vec2 o = Quad[i];
        vec2 huv = (h0 + o + 0.5) * pc.fogLayout.zw;
        vec4 m = POINT_LOD(t_fog, huv);
        vec3 a = POINT_LOD(t_fogAdd, huv).rgb;
        float w = (o.x > 0.5 ? t.x : 1.0 - t.x) * (o.y > 0.5 ? t.y : 1.0 - t.y) * saturate(2.0 - abs(m.a - dP) / tol);
        sumMul += w * m.rgb;
        sumAdd += w * a;
        weights += w;
    }
    mulV = sumMul / max(weights, 1e-6);
    addV = sumAdd / max(weights, 1e-6);
    if (weights < 0.25) {
        vec3 ownMul, ownAdd;
        FogTerms(tex, dP, look, inside, ownMul, ownAdd);
        float k = saturate(weights * 4.0);
        mulV = mix(ownMul, mulV, k);
        addV = mix(ownAdd, addV, k);
    }
}
#endif
