// Decal vertex shader -- port of vanilla SM3004.vso, plus the camera-relative world position
// the forward shadow lookup and the hemisphere skylight need.
// Pairs with SM3007.pso (traced: "Pass 410 BSSM_3XLIGHTING_VcPxoSpc, Decal (SM3004.vso
// SM3007.pso) - Vertex: vanilla - Pixel: vanilla"). This is the bullet-hole / impact decal
// path. Vertex and pixel shaders are NOT paired by name; confirm any new pairing with a trace.
// Vanilla writes TEXCOORD0/3/4/5/6/7, so TEXCOORD1 is free -- the same slot SM3003.vso uses.
// NOTE ON SPACES: tangent, binormal, normal and position are passed through UNTRANSFORMED, so
// the pixel shader receives them in OBJECT space and EyePosition is object-space to match.
// That is why the skylight cannot weight off the interpolated normal and derives a world
// normal from shadowWorldPos instead.
// UNOFFICIAL: the camera matrices Shadow.hlsl rebuilds world positions with move out of c100-c107 in skinned vertex
// shaders: the game's bone upload reaches past Bones[54] (c97) and overwrote them, so actors sampled the sun shadow
// at the wrong place. c240-c247 is beyond any bone write.
#define SHADOW_INVPROJ_REG c240
#define SHADOW_INVVIEW_REG c244
#include "includes/Shadow.hlsl"

uniform row_major float4x4 ModelViewProj : register(c0);
uniform float4 FogParam : register(c44);
uniform float4 FogColor : register(c45);

struct VtxAttrs {
    float4 f_position : POSITION;
    float3 f_tangent  : TANGENT;
    float3 f_binormal : BINORMAL;
    float3 f_normal   : NORMAL;
    float2 f_uv       : TEXCOORD0;
    float4 f_color    : COLOR0;
};

struct VtxVaryings {
    float4 f_position       : POSITION;
    float2 f_uv             : TEXCOORD0;
    float4 f_shadow_world_pos : TEXCOORD1;   // the only addition to vanilla
    float4 f_color          : COLOR0;
    float3 f_tangent        : TEXCOORD3;
    float3 f_binormal       : TEXCOORD4;
    float3 f_normal         : TEXCOORD5;
    float3 f_obj_pos         : TEXCOORD6;
    float4 f_fog            : TEXCOORD7;   // rgb: fog colour, w: fog amount
};

VtxVaryings main(VtxAttrs va) {
    VtxVaryings vr;

    vr.f_position = mul(ModelViewProj, va.f_position);

    // Fog, from the clip-space xyz distance.
    float fog_dist = length(vr.f_position.xyz);
    float fog_strength = saturate((FogParam.x - fog_dist) / FogParam.y);
    vr.f_fog.rgb = FogColor.rgb;
    vr.f_fog.w = pow(1.0f - fog_strength, FogParam.z);

    vr.f_uv       = va.f_uv;
    vr.f_color    = va.f_color;
    vr.f_tangent  = va.f_tangent;
    vr.f_binormal = va.f_binormal;
    vr.f_normal   = va.f_normal;
    vr.f_obj_pos   = va.f_position.xyz;

    vr.f_shadow_world_pos = float4(GetShadowWorldPos(vr.f_position), SHADOW_VS_SENTINEL);

    return vr;
};
