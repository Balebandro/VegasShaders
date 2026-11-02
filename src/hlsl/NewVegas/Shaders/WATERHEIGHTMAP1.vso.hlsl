uniform float4 geometryOffset : register(c0);
uniform float4 texOffset0 : register(c1);

struct VtxAttrs {
    float4 f_lposition : POSITION;
    float4 f_ltexcoord_0 : TEXCOORD0;
};

struct VtxVaryings {
    float4 f_position : POSITION;
    float2 f_texcoord_0 : TEXCOORD0;
};

VtxVaryings main(VtxAttrs va) {
    VtxVaryings vr;

    const int4 loc_const_2 = {2, -2, 0, 0};

    vr.f_position.xy = va.f_lposition.xy - (loc_const_2.xy * geometryOffset.xy);
    vr.f_position.zw = va.f_lposition.zw;
    vr.f_texcoord_0.xy = va.f_ltexcoord_0.xy + texOffset0.xy;
    return vr;
	
};