// Renders the star mask in the shadow of the moon

uniform float4 BlendColor[3] : register(c4);
uniform row_major float4x4 ModelViewProj : register(c0);

struct VtxAttrs {
    float4 f_position : POSITION;
    float4 f_texcoord_0 : TEXCOORD0;
    float4 f_color_0 : COLOR0;
};

struct VtxVaryings {
    float4 f_position : POSITION;
    float2 f_texcoord_0 : TEXCOORD0;
    float2 f_texcoord_1 : TEXCOORD1;
    float3 f_location : TEXCOORD2;
    float4 f_color_0 : COLOR0;
    float4 f_color_1 : COLOR1;
};

VtxVaryings main(VtxAttrs va) {
    VtxVaryings vr;

    vr.f_color_0.rgb = (va.f_color_0.r * BlendColor[0].rgb) + (BlendColor[1].rgb * va.f_color_0.g) + (va.f_color_0.b * BlendColor[2].rgb);
    vr.f_color_0.a = BlendColor[0].a * va.f_color_0.a;

    vr.f_position = mul(ModelViewProj, va.f_position).xyww;
    
    #ifdef REVERSED_DEPTH
        vr.f_position.z *= 0.000050008f; // invert depth
    #else
        vr.f_position.z *= 0.999949992f; // place in front of the sky to avoid Z fighting
    #endif

    vr.f_texcoord_0.xy = va.f_texcoord_0.xy;
    vr.f_texcoord_1.xy = va.f_texcoord_0.xy;

    vr.f_location = va.f_position.xyz;
    vr.f_color_1 = float4(0, 0, 1, 1);

    return vr;
};

