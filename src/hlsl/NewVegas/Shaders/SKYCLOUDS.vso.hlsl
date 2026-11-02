// vertex shader for cloud layer

uniform float4 BlendColor[3] : register(c4);
uniform row_major float4x4 ModelViewProj : register(c0);
uniform float TexCoordYOff : register(c12);

struct VtxAttrs {
    float4 f_position : POSITION;
    float4 f_texcoord_0 : TEXCOORD0;
    float3 f_tangent : TANGENT;
    float3 f_binormal : BINORMAL;
    float3 f_normal : NORMAL;
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

    vr.f_color_0.rgb = (va.f_color_0.r * BlendColor[0].rgb) + (va.f_color_0.g * BlendColor[1].rgb) + (va.f_color_0.b * BlendColor[2].rgb);
    vr.f_color_0.a = BlendColor[0].a * va.f_color_0.a;

    vr.f_location = va.f_position.xyz;

    va.f_position.z -= 5; // lower mesh to avoid visiible seam at the bottom
    vr.f_position = mul(ModelViewProj, va.f_position).xyww;

    #ifdef REVERSED_DEPTH
        vr.f_position.z *= 0.000100017f; // invert depth
    #else
        vr.f_position.z *= 0.999899983f; // scale to appear in front of the moon mask
    #endif

    float2 tmp_r0;
    tmp_r0.y = va.f_texcoord_0.y + TexCoordYOff.x; // scroll clouds
    tmp_r0.x = va.f_texcoord_0.x;

    vr.f_texcoord_0.xy = tmp_r0.xy;
    vr.f_texcoord_1.xy = tmp_r0.xy;

    vr.f_color_1 = float4(0, 1, 0, 1); // identify this object as being clouds

    return vr;
};

