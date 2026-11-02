
uniform float2 Params : register(c4);
uniform float4 TESR_SkyColor : register(c5);
uniform float4 TESR_SkyLowColor: register(c6);
uniform float4 TESR_HorizonColor: register(c7);
uniform float4 TESR_SunColor: register(c8);
uniform float4 TESR_SunDirection: register(c9);
uniform float4 TESR_SkyData: register(c10); // x: athmosphere thickness, y: sun influence, z: sun strength w: sky strength
uniform float4 TESR_SunAmount : register(c11); // x: dayTime, y:sunGlareAmnt, z:replace sun
uniform float4 TESR_DebugVar: register(c12);
uniform float4 TESR_SunPosition: register(c13);
uniform float4 TESR_SunsetColor: register(c14); // sunsetColor.w is sky strength multiplier
uniform float4 TESR_HDRBloomData: register(c15);
uniform float4 TESR_SunDiskColor: register(c16);
uniform float4 TESR_SkyTint: register(c19);     // UNOFFICIAL 2026-10-25 rgb: [Shaders.Sky.Main] TintDay / TintNight for this hour (white = no change)
// UNOFFICIAL 2026-10-29: c32, not c20. c20 is the sky object id the SkyShader hook (Hooks/Shaders.cpp) writes for SKYTEX.pso on every sky
// draw, after NVR's constants are set: here it overwrote the horizon tint with (2, the sun light's red, green), so the horizon came out
// red x 2 (a pink band, red at dusk, in the sky and its water reflection) whatever TintHorizonDay / Night or ColorLook said.
uniform float4 TESR_SkyTintHorizon: register(c32);  // UNOFFICIAL 2026-10-26 rgb: TintHorizonDay / TintHorizonNight for this hour (white = no change)
uniform float4 TESR_SkyLook: register(c21);     // UNOFFICIAL 2026-10-26 x: Saturation for this hour (1 = no change), y: TintGradientHeight
// UNOFFICIAL 2026-10-26: for the true view direction (Includes/Sky.hlsl SkyViewDirection)
uniform float4 TESR_ReciprocalResolution: register(c22);
uniform row_major float4x4 TESR_InvProjectionTransform: register(c23);
uniform row_major float4x4 TESR_InvViewTransform: register(c27);
uniform float4 TESR_RenderScale: register(c31);      // UNOFFICIAL 2026-10-27 RenderScale v2: xy = the world's drawn part of the screen (1 off), zw = 1 / xy
#define SKY_VIEW_DIRECTION
// UNOFFICIAL 2026-10-14 physical sky ([Shaders.Sky.Physical], Includes/Atmosphere.hlsl, Sky.cpp). Compiled in only when on at
// startup (PHYSICAL_SKY_ON, ShaderRecord::LoadShader); otherwise this shader is exactly what it was.
#ifndef PHYSICAL_SKY_ON
    #define PHYSICAL_SKY_ON 0
#endif
#if PHYSICAL_SKY_ON
uniform float4 TESR_PhysSky: register(c17);     // x: share of the physical sky (0 = the game's; it fades out as the sun sets), y: its brightness
                                        // (matched to the weather's sky by Sky.cpp), z: weather tint, w: haze
uniform float4 TESR_PhysSkySun: register(c18);  // xyz: the sun's real direction (TESR_SunPosition is turned upward at night)
// The air's light, worked out once a frame into a small table (Effects/SkyView.fx.hlsl: u the bearing from the sun's, v the
// square root of the height above the horizon; stored x 1000).
uniform sampler2D TESR_SkyViewLUT : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
#endif

static const float4x4 dither_mat = { {0.0588, 0.5294, 0.1765, 0.6471},
									{ 0.7647, 0.2941, 0.8824, 0.4118 },
									{ 0.2353, 0.7059, 0.1176, 0.5882 },
									{ 0.9412, 0.4706, 0.8235, 0.3259 } };

static const float sun_infl_inv = 1/TESR_SkyData.y;

struct VtxAttrs {
    float4 f_color_0 : COLOR0;
    float2 f_screen : VPOS;
    float3 f_eye : TEXCOORD0_centroid;
};

struct VtxVaryings {
    float4 f_color_0 : COLOR0;
};

#include "includes/Helpers.hlsl"
#include "includes/Sky.hlsl"

VtxVaryings main(VtxAttrs pa) {
    VtxVaryings pr;

    float3 up_vec = float3(0, 0, 1);
    float3 eye_dir = normalize(pa.f_eye.xyz);
    float loc_verticality = pows(compress(dot(eye_dir, up_vec)), 3);
    float sun_height = shade(TESR_SunPosition.xyz, up_vec);

    float loc_athmosphere = pows(1 - loc_verticality, 8) * TESR_SkyData.x;
    float sun_vec = compress(dot(eye_dir, TESR_SunPosition.xyz));
    float sun_influence = pows(sun_vec, sun_infl_inv);

    float3 sun_color = GetSunColor(sun_height, TESR_SkyData.x, TESR_SunAmount.x, TESR_SunDiskColor.rgb, TESR_SunsetColor.rgb);
    float3 sky_color = GetSkyColor(loc_verticality, loc_athmosphere, sun_height, sun_influence, TESR_SkyData.z, TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, sun_color);

#if PHYSICAL_SKY_ON
    // UNOFFICIAL physical sky: the air's own light, at the weather's overall brightness, its colour pulled toward the
    // weather's at the same spot by WeatherTint. Below the horizon the view is held just above it (no ground in the air).
    // Sky.cpp PhysicalSkyBlend mirrors this for the sky light on objects.
    [branch] if (TESR_PhysSky.x > 0.0f) {
        float loc_horizontal = length(eye_dir.xy), sun_horizontal = length(TESR_PhysSkySun.xy);
        float cos_bearing = (loc_horizontal > 1e-4f && sun_horizontal > 1e-4f) ? dot(eye_dir.xy, TESR_PhysSkySun.xy) / (loc_horizontal * sun_horizontal) : 1.0f;
        float2 table_uv = float2(acos(clamp(cos_bearing, -1.0f, 1.0f)) / 3.14159265f, sqrt(asin(saturate(eye_dir.z)) / 1.57079633f));
        float3 loc_phys = tex2Dlod(TESR_SkyViewLUT, float4(table_uv, 0.0f, 0.0f)).rgb * (0.001f * TESR_PhysSky.y);
        float3 weather_hue = sky_color / max(luma(sky_color), 1e-6f);
        sky_color = lerp(sky_color, lerp(loc_phys, luma(loc_phys) * weather_hue, TESR_PhysSky.z), TESR_PhysSky.x);
    }
#endif
    sky_color = SkyLook(sky_color, SkyViewDirection(pa.f_screen).z, TESR_SkyTint.rgb, TESR_SkyTintHorizon.rgb, TESR_SkyLook);   // UNOFFICIAL 2026-10-25/26 tint, horizon tint, saturation (Sky.cpp does the same to the sky light on objects)

    pr.f_color_0 = delinearize(float4(sky_color * TESR_SunsetColor.w, 1)) ; // multiply sky strength for HDR

    // dithering
	pr.f_color_0.rgb += dither_mat[ (pa.f_screen.x)%4 ][ (pa.f_screen.y)%4 ] / 255;

    // OUT.f_color_0.rgb = lerp(green, red, sunDir);

    return pr;
};
