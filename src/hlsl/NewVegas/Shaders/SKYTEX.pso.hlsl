// Sky cloud layer: blends the two weather cloud textures during a weather transition and
// shades them against the sky. Also draws the sun and moon discs (ObjectID picks which).

uniform float2 Params : register(c4);
uniform float ObjectID : register(c20);  // Set by hook. 0 == sun, 6 == moon.
uniform sampler2D TexMap : register(s0);
uniform sampler2D TexMapBlend : register(s1);

uniform float4 TESR_SunColor : register(c6);
uniform float4 TESR_SkyColor : register(c7);
uniform float4 TESR_SkyLowColor : register(c8);
uniform float4 TESR_HorizonColor : register(c9);
uniform float4 TESR_SunDirection : register(c10);
uniform float4 TESR_SkyData : register(c11);   // x:AthmosphereThickness y:SunInfluence z:SunStrength w:StarStrength
uniform float4 TESR_CloudData : register(c12); // x:UseNormals y:SphericalNormals z:Transparency w: cloud Brightness
uniform float4 TESR_SunAmount : register(c13); // x:isDayTime, w:GlareStrength
uniform float4 TESR_SunPosition : register(c14);
uniform float4 TESR_SunsetColor : register(c15);
uniform float4 TESR_HDRBloomData : register(c16);
uniform float4 TESR_SunDiskColor : register(c17);
uniform float4 TESR_CloudsHide : register(c23);        // UNOFFICIAL: x 1 = NVR's volumetric clouds replace these layers (VolumetricClouds.cpp)
uniform float4 TESR_SkyTint : register(c24);           // UNOFFICIAL 2026-10-25 rgb: the sky's tint for this hour (SKY.pso), for the sky the clouds blend with
uniform float4 TESR_MoonData : register(c25);          // UNOFFICIAL 2026-10-25 x: MoonBrightness (the moon disc)
uniform float4 TESR_SkyTintHorizon : register(c26);    // UNOFFICIAL 2026-10-26 rgb: the sky's horizon tint for this hour (SKY.pso)
uniform float4 TESR_SkyLook : register(c27);           // UNOFFICIAL 2026-10-26 x: the sky's Saturation, y: TintGradientHeight (SKY.pso)
// UNOFFICIAL 2026-10-26: for the true view direction (Includes/Sky.hlsl SkyViewDirection)
uniform float4 TESR_ReciprocalResolution : register(c28);
uniform row_major float4x4 TESR_InvProjectionTransform : register(c29);
uniform row_major float4x4 TESR_InvViewTransform : register(c33);
uniform float4 TESR_RenderScale : register(c37);     // UNOFFICIAL 2026-10-27 RenderScale v2: xy = the world's drawn part of the screen (1 off), zw = 1 / xy
#define SKY_VIEW_DIRECTION

static const float UseNormals = TESR_CloudData.x;
static const float SphericalNormals = 1;//TESR_CloudData.y;
static const float SUNINFLUENCE = 1/TESR_SkyData.y;
static const float SunStrength = TESR_SkyData.z;
static const float SkyMultiplier = TESR_SunsetColor.w;

// Types:

struct VtxAttrs {
    float2 f_tex_uv : TEXCOORD0;
    float2 f_position: VPOS;
    float2 f_tex_blend_uv : TEXCOORD1;
    float3 f_location: TEXCOORD2;
    float4 f_color_0 : COLOR0;
    float4 f_color_1 : COLOR1;
};

struct VtxVaryings {
    float4 f_color_0 : COLOR0; // sunglare active region/suncolor
};

// Per-pixel sun state, filled in by the vertex stage and read by the sun and cloud code.
struct SunValues {
    float f_atmo;        // how thick the air is along the view ray
    float f_height;      // sun height above the horizon
    float f_influence;   // how close the view is to the sun
    float3 f_col;        // sun colour for this hour
    float3 f_towards;    // 0..1 per axis: looking towards the sun
    float f_day;         // 1 in daytime
};

#include "Includes/Helpers.hlsl"
#include "Includes/Sky.hlsl"

static const float3 up = blue.xyz;

// Builds a cloud normal from the two stored channels and the eye direction.
float3 getNormal(float2 partial_c, float3 eye_dir){

    // Flat-map normals (spherical normals switched off): rotate the sample into the eye's horizontal frame.
    if (!SphericalNormals){
        float2 dir_v = normalize(eye_dir.xy);
        float2x2 rot_m = {{dir_v.x, dir_v.y}, { dir_v.y, -dir_v.x}};
        partial_c = compress(mul(expand(partial_c.xy), rot_m));
        partial_c.y = 1 - partial_c.y;
    }

    partial_c = expand(partial_c);

    // Z is implied by unit length.
    float z_recon = sqrt(1 - saturate(dot(partial_c, partial_c)));
    float3 loc_nrm = normalize(float3(partial_c, z_recon));

    // Tangent frame in the plane perpendicular to the eye direction.
    // http://www.opengl-tutorial.org/intermediate-tutorials/tutorial-13-normal-mapping/
    float3 view_n = eye_dir;
    float3 tangent_x = float3(view_n.y, -view_n.x, 0);  // horizontal X axis. Maybe we could rotate it to get spherical normals?
    float3 loc_bitan = cross(tangent_x, view_n);
    // float3 bitan = cross(viewN, tangentX);

    float3x3 frame_t = {tangent_x, loc_bitan, view_n};
    return normalize(mul(loc_nrm, frame_t));
}

/*
* Shades the sun, moon and sunglare textures
*/
float4 ShadeSun(SunValues S, float4 tex_color, float4 vertex_color){
    S.f_day = smoothstep(0.498, 0.502, TESR_SunAmount.x); // very short transition time to get mostly a value about wether the sun is up or not

    bool sun_on = ObjectID == 0.0f;
    bool moon_on = ObjectID == 6.0f;

    if (sun_on){
        float sunset_amt = smoothstep(0.3, 0.0, S.f_height);
        tex_color.rgb += sunset_amt * S.f_col;
        tex_color.rgb += S.f_col * TESR_SunAmount.w;
        tex_color.a *= S.f_day; // force alpha 1 for sun disk in the daytime
    }else{
        tex_color.rgb *= vertex_color.rgb * Params.y * lerp(TESR_SunAmount.w, 1, moon_on); // vertex color (animated by the engine)
        if (moon_on) tex_color.rgb *= TESR_MoonData.x; // UNOFFICIAL 2026-10-25 MoonBrightness
        tex_color.a *= vertex_color.a;
    }

    return delinearize(tex_color);
}

/*
* Shades the cloud textures
*/
float4 ShadeClouds(float4 layer_c, float4 vertex_color, float3 sky_color, SunValues S, float3 eye_dir){
    // shade clouds
    float grey_amt = lerp(luma(layer_c), 1.0, saturate(TESR_CloudData.w));
    float opacity = layer_c.w * TESR_CloudData.z;

    // sky colour to blend into the clouds
    float3 loc_scatter = pows(S.f_influence, 20) * smoothstep(0.5, 1, 1.0 - opacity) * S.f_col;
    float3 sky_base = linearize(TESR_SkyColor).rgb;

    if (!UseNormals){
        // simply tint the clouds
        grey_amt = (grey_amt - 0.5) * 1.5 + 0.5; // tests for increasing cloud contrast before shading
        float3 tint_rgb = lerp(pows(sky_base * 0.5, 5.0), lerp(sky_base, S.f_col * 5, 0.7 * S.f_towards + 0.3), (1 - saturate(S.f_influence)) * grey_amt).rgb;
        tint_rgb = lerp(white.rgb, tint_rgb * TESR_CloudData.w * 1.333, (1 - S.f_height) * S.f_day); // tint the clouds less when the sun is high in the sky and at night

        // layer.rgb *= lerp(1.0, tint * TESR_CloudData.w * 1.333, isDayTime); // cancel tint at night
        layer_c.rgb *= tint_rgb;
        layer_c.rgb += lerp(black.rgb, loc_scatter * 4.0, (1 - S.f_height) * S.f_day);
    } else {
        // normal-mapped, sun-lit clouds
        float2 packed_n = layer_c.xy; // normal x and y are in red and green, blue is reconstructed
        float3 loc_nrm = getNormal(packed_n, -eye_dir); // reconstruct world normal from red and green

        grey_amt = lerp(saturate(TESR_CloudData.w), 1.0f, layer_c.z); // greyscale is stored in blue channel
        float3 amb_v = sky_color * grey_amt * lerp(0.5, 0.7, S.f_towards); // fade ambient with sun direction
        float3 dif_v = compress(dot(loc_nrm, TESR_SunPosition.xyz)) * S.f_col * (1.0 - luma(amb_v)) * lerp(0.8, 1, S.f_towards); // scale diffuse if ambient is high
        float3 rim_f = pows(1.0 - shade(-eye_dir, loc_nrm), 4.0) * pows(saturate(expand(S.f_towards)), 2.0) * shade(loc_nrm, up) * (S.f_col + sky_color) * 0.2;
        float3 loc_bnc = shade(loc_nrm, -up) * pows(TESR_HorizonColor.rgb, 2.2) * 0.1 * S.f_height; // linearise, light from the ground bouncing up to the underside of clouds

        layer_c = float4(amb_v + dif_v + rim_f + loc_scatter + loc_bnc, opacity);
        // layer.rgb = selectColor(TESR_DebugVar.x, layer, amb, dif, rim, bnc, scatter, sunColor, skyColor, nrm, float3(IN.f_tex_uv, 1));
    }

    layer_c = float4(layer_c.rgb * vertex_color.rgb * Params.y, pows(saturate(layer_c.w * vertex_color.a), 1/TESR_CloudData.z)); // scale alpha with setting

    return delinearize(float4(layer_c.rgb * SkyMultiplier * TESR_CloudData.a, layer_c.a));
}

// UNOFFICIAL 2026-10-26: the old [Shaders.Sky.Clouds] Volumetric slab march (from Physically-Based-Reloaded) is gone: the
// Volumetric Clouds effect draws the clouds (it hides these layers) and the march had no use left.

VtxVaryings main(VtxAttrs pa) {
    VtxVaryings pr;

    float4 loc_vcol = pa.f_color_0;
    loc_vcol = linearize(loc_vcol);

    float3 eye_dir = normalize(pa.f_location);
    float loc_verticality = pows(compress(dot(eye_dir, up)), 3);

    SunValues sun;
    sun.f_height = shade(TESR_SunPosition.xyz, up);
    sun.f_towards = compress(dot(eye_dir, TESR_SunPosition.xyz)); // stores wether the camera is looking in the direction of the sun in range 0/1
    sun.f_atmo = pows(1 - loc_verticality, 8) * TESR_SkyData.x;
    sun.f_influence = pows(sun.f_towards, SUNINFLUENCE).x;	// (.x: what the implicit truncation took)
    sun.f_day = smoothstep(0, 0.5, TESR_SunAmount.x);
    sun.f_col = GetSunColor(sun.f_height, TESR_SkyData.x, TESR_SunAmount.x, TESR_SunColor.rgb, TESR_SunsetColor.rgb);

    float clouds_power = Params.x;
    float4 layer_a = linearize(tex2D(TexMap, pa.f_tex_uv.xy));
    float4 layer_b = linearize(tex2D(TexMapBlend, pa.f_tex_blend_uv.xy));
    float4 layer_mix = lerp(layer_a, layer_b, clouds_power); // weather transition

    float4 loc_picked = (weight(layer_a.xyz) == 0.0 ? layer_b : (weight(layer_b.xyz) == 0.0 ? layer_a : layer_mix)); // select either weather or blend
    loc_picked.a = layer_mix.a;

    if (pa.f_color_1.r){ // early out if this texture is sun/moon*
        pr.f_color_0 = ShadeSun(sun, loc_picked, loc_vcol);
        return pr;
    } else {
        // UNOFFICIAL: the volumetric clouds draw the clouds; the layer adds nothing (blended by its alpha)
        if (TESR_CloudsHide.x > 0.5) { pr.f_color_0 = float4(0.0, 0.0, 0.0, 0.0); return pr; }
        float3 sky_col = GetSkyColor(loc_verticality, sun.f_atmo, sun.f_height, sun.f_influence, TESR_SkyData.z, TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, sun.f_col);
        sky_col = SkyLook(sky_col, SkyViewDirection(pa.f_position).z, TESR_SkyTint.rgb, TESR_SkyTintHorizon.rgb, TESR_SkyLook);   // UNOFFICIAL 2026-10-26 as SKY.pso
        pr.f_color_0 = ShadeClouds(loc_picked, loc_vcol, sky_col, sun, eye_dir);
        return pr;
    }
};
