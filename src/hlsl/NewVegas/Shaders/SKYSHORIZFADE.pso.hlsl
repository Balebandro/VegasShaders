// Night sky

uniform sampler2D TexMap : register(s0);

uniform float4 TESR_GameTime;
uniform float4 TESR_DebugVar;
uniform float4 TESR_SkyData;
uniform float4 TESR_ReciprocalResolution;
uniform float4 TESR_CloudData;

struct VtxAttrs {
    float2 f_tex_uv : TEXCOORD0;
    float2 f_position : VPOS;
    float f_texcoord_2 : TEXCOORD2;
    float3 f_location : TEXCOORD1;
    float4 f_color_0 : COLOR0;
};

struct VtxVaryings {
    float4 f_color_0 : COLOR0;
};

// hash based 3d value noise
// function taken from https://www.shadertoy.com/view/XslGRr
// Created by inigo quilez - iq/2013
// License Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported License.
// ported from GLSL to HLSL by https://stackoverflow.com/questions/15628039/simplex-noise-shader
float hash( float nn )
{
    return frac(sin(nn)*43758.5453);
}

float noise( float3 xx )
{
    // The noise function returns a value in the range -1.0f -> 1.0f

    float3 pt = floor(xx);
    float3 ff = frac(xx);

    ff       = ff*ff*(3.0-2.0*ff);
    float nn = pt.x + pt.y*57.0 + 113.0*pt.z;

    return lerp(lerp(lerp( hash(nn+0.0), hash(nn+1.0),ff.x),
            lerp( hash(nn+57.0), hash(nn+58.0),ff.x),ff.y),
            lerp(lerp( hash(nn+113.0), hash(nn+114.0),ff.x),
            lerp( hash(nn+170.0), hash(nn+171.0),ff.x),ff.y),ff.z);
}

#include "includes/Helpers.hlsl"
#include "includes/Position.hlsl"

VtxVaryings main(VtxAttrs pa) {
    VtxVaryings pr;

    float4 stars_c = linearize(tex2D(TexMap, pa.f_tex_uv.xy));
    float4 loc_skytint = linearize(pa.f_color_0);

    float star_flicker = 0.05;
    float noise_scale = 4;
    float flicker_speed = 0.1 * TESR_CloudData.y;

    float3 eye_dir = normalize(pa.f_location);
  
    // create animated noise to modulate star brightness
    float stars_luma = smoothstep(0, 1, luma(stars_c.rgb)); // only pick the brightest areas of the sky for the flicker

    float nn = noise(noise_scale * eye_dir);
    nn *= noise((TESR_GameTime.x * flicker_speed).xxx + noise_scale * eye_dir) * 2;

    pr.f_color_0.a = (stars_c.a * loc_skytint.a) * pa.f_texcoord_2.x * TESR_SkyData.w ;//* (n * starsLuma + 1);
    stars_c.rgb = stars_c.rgb * (nn * 100 * stars_luma + 1) * loc_skytint.rgb;
    stars_c.rgb = delinearize(stars_c).rgb; //delinearise
    pr.f_color_0.rgb = stars_c.rgb;
    return pr;
};
