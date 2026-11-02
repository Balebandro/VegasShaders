// UNOFFICIAL 2026-10-27 displacement shadows ([Shaders.Terrain.Displacement] CastShadows, core/DisplaceShadows.h; plan step 2 of
// docs/next/FORK2-DISPLACE.md). Loaded only with [Main.Main.Performance] NvrDxvkDisplacement on at startup and the NVR DXVK
// tessellation interface present.
// The land drawn into the sun's shadow cascades through the land's own tessellation (Compute/Tess/TerrainDisplace.tesc and the
// SHADOW_CASTER variants of TerrainDisplace.tese), so the maps hold the same bumps the camera sees. The outputs are TerrainTemplate's
// vertex shader's, at the same places (the TCS hands them on as they are), with these differences:
//  - TEXCOORD6 (and the position) is the shadow map's clip position, pancaked as ShadowMap.vso does;
//  - TEXCOORD7 is the MAIN camera's eye in object space (the TCS measures the splitting, the distance fade and the quadrant borders
//    from it, so they are the main view's, point for point, and not the light's);
//  - the free w of TEXCOORD3/4/5/7 hold the shadow map's ModelViewProj times the normal (the TES moves the clip position by it);
//  - c0-c3 is last frame's main-camera ModelViewProj (ObjectBlend reads last frame's depth copy with it; the TES finds it through the
//    registration's constant indices, so this shader must use all four rows: they go out in TEXCOORD0.zw, TEXCOORD1.w, TEXCOORD2.w,
//    which nothing reads).
// No TESR_ constants: core/DisplaceShadows.h sets c0-c8 itself for every land piece.

uniform row_major float4x4 NvrMainModelViewProj : register(c0);	// last frame's main camera (rows: the clip position's components)
uniform row_major float4x4 NvrShadowModelViewProj : register(c4);	// this cascade (rows: the clip position's components)
uniform float4 NvrMainEye : register(c8);							// the main camera, object space (xyz)

struct VtxAttrs {
    float4 f_position : POSITION;
    float3 f_tangent : TANGENT;
    float3 f_binormal : BINORMAL;
    float3 f_normal : NORMAL;
    float4 f_uv : TEXCOORD0;
    float4 f_vertex_color : COLOR0;
    float4 f_blend_0 : TEXCOORD1;
    float4 f_blend_1 : TEXCOORD2;
};

struct VtxVaryings {
    float4 f_blend_0 : COLOR0;
    float4 f_blend_1 : COLOR1;
    float4 f_position : POSITION;
    float4 f_uv : TEXCOORD0;
    float4 f_vertex_color : TEXCOORD1;
    float4 f_l_position : TEXCOORD2;
    float4 f_tangent : TEXCOORD3;
    float4 f_binormal : TEXCOORD4;
    float4 f_normal : TEXCOORD5;
    float4 f_projection_position : TEXCOORD6;
    float4 f_view_position : TEXCOORD7;
};

VtxVaryings main(VtxAttrs va) {
    VtxVaryings vr;

    const float4x4 main_mvp = float4x4(NvrMainModelViewProj[0], NvrMainModelViewProj[1], NvrMainModelViewProj[2], NvrMainModelViewProj[3]);
    const float4x4 shadow_mvp = float4x4(NvrShadowModelViewProj[0], NvrShadowModelViewProj[1], NvrShadowModelViewProj[2], NvrShadowModelViewProj[3]);
    const float4 main_clip = mul(main_mvp, va.f_position);
    float4 shadow_clip = mul(shadow_mvp, va.f_position);
    shadow_clip.z = max(shadow_clip.z, 0.0f);	// pancaking, as ShadowMap.vso
    const float4 shadow_normal = float4(dot(shadow_mvp[0].xyz, va.f_normal), dot(shadow_mvp[1].xyz, va.f_normal),
        dot(shadow_mvp[2].xyz, va.f_normal), dot(shadow_mvp[3].xyz, va.f_normal));

    vr.f_blend_0 = va.f_blend_0;
    vr.f_blend_1 = va.f_blend_1;
    vr.f_position = shadow_clip;
    vr.f_uv = float4(va.f_uv.xy, main_clip.xy);
    vr.f_vertex_color = float4(saturate(va.f_vertex_color.rgb), main_clip.z);
    vr.f_l_position = float4(va.f_position.xyz, main_clip.w);
    vr.f_tangent = float4(va.f_tangent, shadow_normal.x);
    vr.f_binormal = float4(va.f_binormal, shadow_normal.y);
    vr.f_normal = float4(va.f_normal, shadow_normal.z);
    vr.f_projection_position = shadow_clip;
    vr.f_view_position = float4(NvrMainEye.xyz, shadow_normal.w);
    return vr;
}
