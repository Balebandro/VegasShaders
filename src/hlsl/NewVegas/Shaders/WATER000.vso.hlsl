
uniform row_major float4x4 ModelViewProj : register(c0);
uniform row_major float4x4 WorldMat : register(c4);
uniform float TexScale : register(c8);
uniform float4 QPosAdjust : register(c9);
uniform float ObjectUV : register(c10);
uniform float4 TESR_CameraPosition : register(c11);

#include "Includes/Helpers.hlsl"

struct VtxAttrs {
    float4 f_lposition : POSITION;
    float4 f_ltexcoord_0 : TEXCOORD0;
};

struct VtxVaryings {
    float4 f_position : POSITION;
    float4 f_texcoord_0 : TEXCOORD0;
    float4 f_texcoord_1 : TEXCOORD1;
    float4 f_texcoord_2 : TEXCOORD2; // modelviewproj matrix 1st row 
    float4 f_texcoord_3 : TEXCOORD3; // modelviewproj matrix 2nd row
    float4 f_texcoord_4 : TEXCOORD4; // modelviewproj matrix 3rd row
    float4 f_texcoord_5 : TEXCOORD5; // modelviewproj matrix 4th row
    float4 f_texcoord_6 : TEXCOORD6;
    float4 f_texcoord_7 : TEXCOORD7;
    float4 f_world_position : TEXCOORD8; // world position of surface point
};

VtxVaryings main(VtxAttrs va) {
    VtxVaryings vr;

    const float4 loc_const_7 = {0.5, 0.001, 1, 0};

    float4 loc_mdl0;
    float2 tmp_q4;
    float2 tmp_r0;
    float2 tmp_r1;

    vr.f_position = mul(ModelViewProj, va.f_lposition); // screen position
    tmp_r1.y = dot(WorldMat[1], va.f_lposition);
    tmp_r1.x = dot(WorldMat[0], va.f_lposition);
    vr.f_texcoord_0.w = length(WorldMat[0].xyz);
    vr.f_texcoord_0.z = dot(WorldMat[2], va.f_lposition);
    vr.f_texcoord_0.xy = tmp_r1.xy;
    tmp_r0.xy = loc_const_7.xy;
    loc_mdl0 = tmp_r0.x * ModelViewProj[3];
    vr.f_texcoord_1 = va.f_lposition;
    tmp_q4.xy = (tmp_r1.xy + QPosAdjust.xy) / TexScale.x;
    vr.f_texcoord_2 = (tmp_r0.x * ModelViewProj[0]) + loc_mdl0;
    vr.f_texcoord_3 = (tmp_r0.x * ModelViewProj[1]) + loc_mdl0;
    vr.f_texcoord_4 = (tmp_r0.x * ModelViewProj[2]) + loc_mdl0;
    vr.f_texcoord_5 = ModelViewProj[3];
    vr.f_texcoord_6 = (va.f_ltexcoord_0.xyzx * loc_const_7.zzzw) + loc_const_7.wwwz;
    tmp_r0.x = (-(sqr(ObjectUV.x)) < sqr(ObjectUV.x) ? 1.0 : 0.0);
    vr.f_texcoord_7.zw = 0;
    vr.f_texcoord_7.xy = (tmp_r0.x * ((va.f_ltexcoord_0.xy / (tmp_r0.y * TexScale.x)) - tmp_q4.xy)) + tmp_q4.xy;
    
    vr.f_world_position = mul(WorldMat, va.f_lposition) + TESR_CameraPosition;

    return vr;
	
};