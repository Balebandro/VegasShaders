
uniform float4 geometryOffset : register(c0);
uniform float4 texOffset0 : register(c1);
uniform float4 texOffset1 : register(c2);

struct VtxAttrs {
    float4 f_position : POSITION;
    float4 f_texcoord_0 : TEXCOORD0;
};

struct VtxVaryings {
    float4 f_position : POSITION;
    float2 f_texcoord_0 : TEXCOORD0;
    float2 f_texcoord_1 : TEXCOORD1;
};

VtxVaryings main(VtxAttrs va) {
    VtxVaryings vr;

    const int4 loc_const_3 = {2, -2, 0, 0};

    vr.f_position.xy = va.f_position.xy - (loc_const_3.xy * geometryOffset.xy);
    vr.f_position.zw = va.f_position.zw;
    vr.f_texcoord_0.xy = va.f_texcoord_0.xy + texOffset0.xy;
    vr.f_texcoord_1.xy = va.f_texcoord_0.xy + texOffset1.xy;

    return vr;
};

