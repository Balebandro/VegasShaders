
uniform float4 AmbientColor : register(c1);
uniform sampler2D BaseMap : register(s0);
uniform sampler2D LODLandNoise : register(s7);
uniform sampler2D LODParentNormals : register(s6);
uniform sampler2D LODParentTex : register(s4);
uniform float4 LODTexParams : register(c31);
uniform sampler2D NormalMap : register(s1);
uniform float4 PSLightColor[10] : register(c3);

struct VtxAttrs {
    float2 f_normal_uv : TEXCOORD0;
    float4 f_color_1 : COLOR1;
    float3 f_texcoord_1 : TEXCOORD1_centroid;
};

struct VtxVaryings {
    float4 f_color_0 : COLOR0;
};

#include "includes/Helpers.hlsl"

VtxVaryings main(VtxAttrs pa) {
    VtxVaryings pr;

    float3 tmp_r0 = LODTexParams.xyw;

    float3 nrm_vec = tex2D(NormalMap, pa.f_normal_uv).xyz;
    float noise_v = tex2D(LODLandNoise, pa.f_normal_uv * 1.75).r;
    float3 parent_normal = tex2D(LODParentNormals, (pa.f_normal_uv * 0.5) + tmp_r0.xy).xyz;
    
    nrm_vec = tmp_r0.z >= 1 ? nrm_vec : lerp(parent_normal, nrm_vec, LODTexParams.w);

    float light_term = shades(expand(nrm_vec), pa.f_texcoord_1.xyz);
    float3 tmp_q4 = max((light_term * PSLightColor[0].rgb) + AmbientColor.rgb, 0);
    
    float2 tmp_q1 = (pa.f_normal_uv * 0.9921875) + (1.0 / 256);
    float3 tmp_r1 = tex2D(LODParentTex, (0.5 * tmp_q1) + lerp(tmp_r0.xy, 0.25, (1.0 / 128))).rgb;
    float3 tmp_r2 = tex2D(BaseMap, tmp_q1).rgb;
    
    float3 tmp_q5 = (tmp_r0.z >= 1 ? tmp_r2 : lerp(tmp_r1, tmp_r2, LODTexParams.w)) * ((noise_v * 0.8) + 0.55);

    pr.f_color_0.rgb = (pa.f_color_1.a * (pa.f_color_1.rgb - (tmp_q5 * tmp_q4))) + (tmp_q5 * tmp_q4);

    pr.f_color_0.rgb *= green.rgb;

    pr.f_color_0.a = 1;

    return pr;
};

