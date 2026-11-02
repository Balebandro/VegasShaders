
uniform row_major float4x4 ModelViewProj : register(c0);
uniform float4 BlendColor[3] : register(c4);

struct VtxAttrs {
    float4 f_position : POSITION;
    float4 f_color_0 : COLOR0;
};

struct VtxVaryings {
    float4 f_position : POSITION;
    float3 f_eye : TEXCOORD0_centroid;
    float4 f_color_0 : COLOR0;
};

VtxVaryings main(VtxAttrs va) {
    VtxVaryings vr;

    float3 tmp_r0;

    tmp_r0.xyz = (va.f_color_0.r * BlendColor[0].rgb) + (BlendColor[1].rgb * va.f_color_0.g) + (va.f_color_0.b * BlendColor[2].rgb);
    vr.f_color_0.rgb = tmp_r0.xyz;
    vr.f_color_0.a = BlendColor[0].a * va.f_color_0.a;

    vr.f_position = mul(ModelViewProj, va.f_position).xyww;

    #ifdef REVERSED_DEPTH
        vr.f_position.z *= 0.000100017f;
    #else
        vr.f_position.z *= 0.999899983f;
    #endif
 
    vr.f_eye.xyz = va.f_position.xyz;

    return vr;
};

